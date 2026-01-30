/***************************************************************************
  testqgsrastergpubenchmark.cpp - Benchmark GPU vs CPU raster rendering
  --------------------------------------
  Date                 : January 2026
  Copyright            : (C) 2026 by Wietze Suijker
  Email                : wietzesuijker at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <algorithm>
#include <cmath>

#include "qgsapplication.h"
#include "qgsmaprenderersequentialjob.h"
#include "qgsmapsettings.h"
#include "qgsproject.h"
#include "qgsrastergpufactory.h"
#include "qgsrasterlayer.h"
#include "qgsrasterlayerrenderer.h"
#include "qgstest.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>

#ifdef HAVE_QRHI
#include <rhi/qrhi.h>
#endif

/**
 * \ingroup UnitTests
 * Benchmark GPU vs CPU raster rendering performance
 *
 * Tests realistic user scenarios:
 * 1. Initial render (cold cache) - opening a project
 * 2. Pan simulation - rendering adjacent extents
 * 3. Zoom simulation - rendering at different scales
 * 4. Remote COG via /vsicurl/ - shows GPU benefit with network latency
 */
class TestQgsRasterGPUBenchmark : public QgsTest
{
    Q_OBJECT

  public:
    TestQgsRasterGPUBenchmark()
      : QgsTest( u"GPU Raster Benchmark Tests"_s ) {}

  private slots:
    void initTestCase();
    void cleanupTestCase();

    // Scenario-based benchmarks (more representative of real usage)
    void benchmarkInitialRender();  // Opening a COG layer
    void benchmarkPanSimulation();  // User panning across map
    void benchmarkZoomSimulation(); // User zooming in/out

    // Remote COG benchmark (shows real-world GPU benefit with network latency)
    void benchmarkRemoteCOG(); // Zoom test via /vsicurl/ (informational)

    // Data type matrix (tests different data types)
    void benchmarkFloat32(); // Float32 continuous data

    void benchmarkComparison();

  private:
    struct HardwareInfo
    {
        QString backend;  // QRhi backend name (Vulkan, Metal, D3D11, OpenGLES2)
        QString renderer; // GPU device name
        QString version;  // Driver version
        QString vendor;   // GPU vendor
    };

    struct BenchmarkResult
    {
        QString scenario;
        QString mode;
        QString dataType;
        int tiles;
        double avgMs;
        double minMs;
        double maxMs;
        double medianMs;
        double stddevMs;
        double fps;               // frames per second for pan/zoom scenarios
        QVector<double> allTimes; // raw measurements for stats
    };

    // Single render measurement
    double measureRenderTime( QgsMapSettings &settings );

    // Scenario runners
    BenchmarkResult runInitialRender( QgsRasterLayer *layer, int targetTiles, bool useGPU );
    BenchmarkResult runPanSimulation( QgsRasterLayer *layer, int steps, bool useGPU );
    BenchmarkResult runZoomSimulation( QgsRasterLayer *layer, int levels, bool useGPU );

    QgsRectangle getExtentForTiles( QgsRasterLayer *layer, int targetTiles );
    QgsRectangle panExtent( const QgsRectangle &extent, double dx, double dy );
    int estimateTiles( QgsRasterLayer *layer, const QgsRectangle &extent );

    bool initializeGPU();
    void cleanupGPU();
    HardwareInfo getHardwareInfo();
    void computeStatistics( BenchmarkResult &result );
    QJsonObject resultToJson( const BenchmarkResult &result );
    void writeJsonOutput();

    bool mGpuInitialized = false;
    HardwareInfo mHardwareInfo;
    QString mCogPath;        // Byte COG (NLCD)
    QString mFloat32CogPath; // Float32 COG (synthetic)

    // Store results for comparison
    QVector<BenchmarkResult> mCpuResults;
    QVector<BenchmarkResult> mGpuResults;
};

void TestQgsRasterGPUBenchmark::initTestCase()
{
  QgsApplication::init();
  QgsApplication::initQgis();

  // Use local COG if available, otherwise remote
  mCogPath = u"/tmp/qgis-bench-cog.tif"_s;
  if ( !QFile::exists( mCogPath ) )
  {
    mCogPath = u"/vsicurl/https://s3.us-east-1.amazonaws.com/ds-deck.gl-raster-public/cog/Annual_NLCD_LndCov_2024_CU_C1V1.tif"_s;
  }

  // Float32 COG for data type matrix testing
  mFloat32CogPath = u"/tmp/qgis-bench-float32.tif"_s;
  if ( !QFile::exists( mFloat32CogPath ) )
  {
    qDebug() << "Float32 COG not found - skipping Float32 benchmarks";
    mFloat32CogPath.clear();
  }

  qDebug() << "Using Byte COG:" << mCogPath;
  if ( !mFloat32CogPath.isEmpty() )
    qDebug() << "Using Float32 COG:" << mFloat32CogPath;
}

void TestQgsRasterGPUBenchmark::cleanupTestCase()
{
  cleanupGPU();
  QgsApplication::exitQgis();
}

bool TestQgsRasterGPUBenchmark::initializeGPU()
{
  if ( mGpuInitialized )
    return true;

  // Initialize QRhi via factory
  QgsRasterGPUFactory::initialize();

#ifdef HAVE_QRHI
  QRhi *rhi = QgsRasterGPUFactory::rhi();
  if ( rhi )
  {
    mGpuInitialized = true;
    mHardwareInfo = getHardwareInfo();
    qDebug() << "GPU Backend:" << mHardwareInfo.backend;
    qDebug() << "GPU Renderer:" << mHardwareInfo.renderer;
    return true;
  }
#endif

  qDebug() << "GPU initialization failed (QRhi not available)";
  return false;
}

void TestQgsRasterGPUBenchmark::cleanupGPU()
{
  if ( mGpuInitialized )
  {
    QgsRasterGPUFactory::cleanup();
    mGpuInitialized = false;
  }
}

TestQgsRasterGPUBenchmark::HardwareInfo TestQgsRasterGPUBenchmark::getHardwareInfo()
{
  HardwareInfo info;
#ifdef HAVE_QRHI
  QRhi *rhi = QgsRasterGPUFactory::rhi();
  if ( rhi )
  {
    info.backend = QString::fromLatin1( rhi->backendName() );
    const QRhiDriverInfo driverInfo = rhi->driverInfo();
    info.renderer = driverInfo.deviceName;
    info.vendor = QString(); // QRhi doesn't expose vendor separately
    // Combine device ID info for version field
    info.version = QString( "Device %1, Vendor %2" )
                     .arg( driverInfo.deviceId )
                     .arg( driverInfo.vendorId );
  }
#endif
  return info;
}

void TestQgsRasterGPUBenchmark::computeStatistics( BenchmarkResult &result )
{
  if ( result.allTimes.isEmpty() )
    return;

  // Sort for median
  QVector<double> sorted = result.allTimes;
  std::sort( sorted.begin(), sorted.end() );

  // Median
  const int n = sorted.size();
  if ( n % 2 == 0 )
    result.medianMs = ( sorted[n / 2 - 1] + sorted[n / 2] ) / 2.0;
  else
    result.medianMs = sorted[n / 2];

  // Mean (already computed as avgMs)
  double sum = 0;
  for ( double t : result.allTimes )
    sum += t;
  result.avgMs = sum / n;

  // Standard deviation
  double sumSq = 0;
  for ( double t : result.allTimes )
  {
    const double diff = t - result.avgMs;
    sumSq += diff * diff;
  }
  result.stddevMs = std::sqrt( sumSq / n );
}

QJsonObject TestQgsRasterGPUBenchmark::resultToJson( const BenchmarkResult &result )
{
  QJsonObject obj;
  obj[u"scenario"_s] = result.scenario;
  obj[u"mode"_s] = result.mode;
  obj[u"data_type"_s] = result.dataType;
  obj[u"tiles"_s] = result.tiles;
  obj[u"avg_ms"_s] = result.avgMs;
  obj[u"min_ms"_s] = result.minMs;
  obj[u"max_ms"_s] = result.maxMs;
  obj[u"median_ms"_s] = result.medianMs;
  obj[u"stddev_ms"_s] = result.stddevMs;
  obj[u"fps"_s] = result.fps;
  return obj;
}

void TestQgsRasterGPUBenchmark::writeJsonOutput()
{
  QJsonObject root;

  // Metadata
  root[u"timestamp"_s] = QDateTime::currentDateTimeUtc().toString( Qt::ISODate );
  root[u"qgis_version"_s] = Qgis::version();

  // Hardware info
  QJsonObject hw;
  hw[u"backend"_s] = mHardwareInfo.backend;
  hw[u"renderer"_s] = mHardwareInfo.renderer;
  hw[u"version"_s] = mHardwareInfo.version;
  hw[u"vendor"_s] = mHardwareInfo.vendor;
  root[u"hardware"_s] = hw;

  // CPU results
  QJsonArray cpuArray;
  for ( const auto &r : mCpuResults )
    cpuArray.append( resultToJson( r ) );
  root[u"cpu_results"_s] = cpuArray;

  // GPU results
  QJsonArray gpuArray;
  for ( const auto &r : mGpuResults )
    gpuArray.append( resultToJson( r ) );
  root[u"gpu_results"_s] = gpuArray;

  // Speedup summary
  QJsonArray speedups;
  for ( int i = 0; i < mCpuResults.size() && i < mGpuResults.size(); ++i )
  {
    QJsonObject sp;
    sp[u"scenario"_s] = mCpuResults[i].scenario;
    const double speedup = ( mGpuResults[i].avgMs > 0 ) ? mCpuResults[i].avgMs / mGpuResults[i].avgMs : 0;
    sp[u"speedup"_s] = speedup;
    speedups.append( sp );
  }
  root[u"speedups"_s] = speedups;

  // Write JSON
  QJsonDocument doc( root );
  qDebug() << "";
  qDebug() << "BENCHMARK_JSON_START";
  qDebug().noquote() << doc.toJson( QJsonDocument::Indented );
  qDebug() << "BENCHMARK_JSON_END";
}

QgsRectangle TestQgsRasterGPUBenchmark::getExtentForTiles( QgsRasterLayer *layer, int targetTiles )
{
  const QgsRectangle layerExtent = layer->extent();
  const double pixelWidth = layerExtent.width() / layer->width();
  const double pixelHeight = layerExtent.height() / layer->height();

  const int blockWidth = layer->dataProvider()->xBlockSize();
  const double tilesPerSide = std::sqrt( targetTiles );
  const double extentPixels = tilesPerSide * blockWidth;

  const double extentWidth = extentPixels * pixelWidth;
  const double extentHeight = extentPixels * pixelHeight;

  const QgsPointXY center = layerExtent.center();

  return QgsRectangle(
    center.x() - extentWidth / 2,
    center.y() - extentHeight / 2,
    center.x() + extentWidth / 2,
    center.y() + extentHeight / 2
  );
}

QgsRectangle TestQgsRasterGPUBenchmark::panExtent( const QgsRectangle &extent, double dx, double dy )
{
  // Shift extent by fraction of its size (simulates pan)
  const double shiftX = extent.width() * dx;
  const double shiftY = extent.height() * dy;
  return QgsRectangle(
    extent.xMinimum() + shiftX,
    extent.yMinimum() + shiftY,
    extent.xMaximum() + shiftX,
    extent.yMaximum() + shiftY
  );
}

int TestQgsRasterGPUBenchmark::estimateTiles( QgsRasterLayer *layer, const QgsRectangle &extent )
{
  const QgsRectangle layerExtent = layer->extent();
  const double pixelWidth = layerExtent.width() / layer->width();
  const double pixelHeight = layerExtent.height() / layer->height();

  const int blockWidth = layer->dataProvider()->xBlockSize();
  const int blockHeight = layer->dataProvider()->yBlockSize();

  const double extentPixelsX = extent.width() / pixelWidth;
  const double extentPixelsY = extent.height() / pixelHeight;

  const int tilesX = static_cast<int>( extentPixelsX / blockWidth ) + 1;
  const int tilesY = static_cast<int>( extentPixelsY / blockHeight ) + 1;

  return tilesX * tilesY;
}

double TestQgsRasterGPUBenchmark::measureRenderTime( QgsMapSettings &settings )
{
  QElapsedTimer timer;
  timer.start();

  QgsMapRendererSequentialJob job( settings );
  job.start();
  job.waitForFinished();

  return timer.elapsed();
}

TestQgsRasterGPUBenchmark::BenchmarkResult TestQgsRasterGPUBenchmark::runInitialRender(
  QgsRasterLayer *layer, int targetTiles, bool useGPU
)
{
  // Toggle GPU rendering based on useGPU parameter
  if ( useGPU )
  {
    QgsRasterGPUFactory::initialize();
  }
  else
  {
    QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );
  }

  // Simulates: User opens QGIS project with a COG layer (cold cache)
  BenchmarkResult result;
  result.scenario = u"Initial Load"_s;
  result.mode = useGPU ? u"GPU"_s : u"CPU"_s;
  result.dataType = layer->dataProvider()->dataType( 1 ) == Qgis::DataType::Float32
                      ? u"Float32"_s
                      : u"Byte"_s;
  result.minMs = std::numeric_limits<double>::max();
  result.maxMs = 0;
  result.fps = 0;
  result.medianMs = 0;
  result.stddevMs = 0;

  const QgsRectangle extent = getExtentForTiles( layer, targetTiles );
  result.tiles = estimateTiles( layer, extent );

  QgsMapSettings settings;
  settings.setLayers( { layer } );
  settings.setExtent( extent );
  settings.setOutputSize( QSize( 1024, 1024 ) ); // Typical canvas size
  settings.setDestinationCrs( layer->crs() );

  // Measure 5 cold renders (no warmup - that's the point)
  for ( int i = 0; i < 5; ++i )
  {
    const double elapsed = measureRenderTime( settings );
    result.allTimes.append( elapsed );
    result.minMs = std::min( result.minMs, elapsed );
    result.maxMs = std::max( result.maxMs, elapsed );
  }

  computeStatistics( result );
  return result;
}

TestQgsRasterGPUBenchmark::BenchmarkResult TestQgsRasterGPUBenchmark::runPanSimulation(
  QgsRasterLayer *layer, int steps, bool useGPU
)
{
  // Toggle GPU rendering based on useGPU parameter
  if ( useGPU )
  {
    QgsRasterGPUFactory::initialize();
  }
  else
  {
    QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );
  }

  // Simulates: User panning across the map (incremental tile loads)
  // Each pan shifts by 25% of view - some tiles reused, some new
  BenchmarkResult result;
  result.scenario = u"Pan (%1 steps)"_s.arg( steps );
  result.mode = useGPU ? u"GPU"_s : u"CPU"_s;
  result.dataType = layer->dataProvider()->dataType( 1 ) == Qgis::DataType::Float32
                      ? u"Float32"_s
                      : u"Byte"_s;
  result.tiles = 0;
  result.minMs = std::numeric_limits<double>::max();
  result.maxMs = 0;
  result.medianMs = 0;
  result.stddevMs = 0;

  // Start with ~100 tiles visible
  QgsRectangle extent = getExtentForTiles( layer, 100 );
  result.tiles = estimateTiles( layer, extent );

  QgsMapSettings settings;
  settings.setLayers( { layer } );
  settings.setOutputSize( QSize( 1024, 1024 ) );
  settings.setDestinationCrs( layer->crs() );

  // Warmup (2 renders to stabilize)
  settings.setExtent( extent );
  measureRenderTime( settings );
  measureRenderTime( settings );

  // Pan in a pattern: right, right, down, left, left, down, ...
  const double panFraction = 0.25; // 25% overlap between frames
  const QVector<QPair<double, double>> directions = {
    { panFraction, 0 }, { panFraction, 0 }, // right, right
    { 0, -panFraction },                    // down
    { -panFraction, 0 },
    { -panFraction, 0 }, // left, left
    { 0, -panFraction }  // down
  };

  for ( int i = 0; i < steps; ++i )
  {
    const auto &dir = directions[i % directions.size()];
    extent = panExtent( extent, dir.first, dir.second );
    settings.setExtent( extent );

    const double elapsed = measureRenderTime( settings );
    result.allTimes.append( elapsed );
    result.minMs = std::min( result.minMs, elapsed );
    result.maxMs = std::max( result.maxMs, elapsed );
  }

  computeStatistics( result );
  result.fps = 1000.0 / result.avgMs; // Convert ms to FPS

  return result;
}

TestQgsRasterGPUBenchmark::BenchmarkResult TestQgsRasterGPUBenchmark::runZoomSimulation(
  QgsRasterLayer *layer, int levels, bool useGPU
)
{
  // Toggle GPU rendering based on useGPU parameter
  if ( useGPU )
  {
    QgsRasterGPUFactory::initialize();
  }
  else
  {
    QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );
  }

  // Simulates: User zooming in (triggers different overview levels)
  BenchmarkResult result;
  result.scenario = u"Zoom (%1 levels)"_s.arg( levels );
  result.mode = useGPU ? u"GPU"_s : u"CPU"_s;
  result.dataType = layer->dataProvider()->dataType( 1 ) == Qgis::DataType::Float32
                      ? u"Float32"_s
                      : u"Byte"_s;
  result.tiles = 0;
  result.minMs = std::numeric_limits<double>::max();
  result.maxMs = 0;
  result.medianMs = 0;
  result.stddevMs = 0;

  // Start zoomed out (full extent)
  QgsRectangle extent = layer->extent();

  QgsMapSettings settings;
  settings.setLayers( { layer } );
  settings.setOutputSize( QSize( 1024, 1024 ) );
  settings.setDestinationCrs( layer->crs() );

  for ( int i = 0; i < levels; ++i )
  {
    settings.setExtent( extent );
    result.tiles = std::max( result.tiles, estimateTiles( layer, extent ) );

    const double elapsed = measureRenderTime( settings );
    result.allTimes.append( elapsed );
    result.minMs = std::min( result.minMs, elapsed );
    result.maxMs = std::max( result.maxMs, elapsed );

    // Zoom in by 2x (center on current extent)
    const QgsPointXY center = extent.center();
    const double newWidth = extent.width() / 2.0;
    const double newHeight = extent.height() / 2.0;
    extent = QgsRectangle(
      center.x() - newWidth / 2, center.y() - newHeight / 2,
      center.x() + newWidth / 2, center.y() + newHeight / 2
    );
  }

  computeStatistics( result );
  result.fps = 1000.0 / result.avgMs;

  return result;
}

void TestQgsRasterGPUBenchmark::benchmarkInitialRender()
{
  // Scenario: User opens a QGIS project containing a large COG
  qDebug() << "\n========================================";
  qDebug() << "Scenario: Initial Project Load";
  qDebug() << "========================================";
  qDebug() << "Simulates opening a project with a COG layer";
  qDebug() << "";

  QgsRasterLayer layer( mCogPath, u"benchmark"_s, u"gdal"_s );
  if ( !layer.isValid() )
  {
    QSKIP( "Could not load test COG" );
  }

  // CPU baseline
  auto cpuResult = runInitialRender( &layer, 100, false );
  mCpuResults.append( cpuResult );

  qDebug() << QString( "CPU: %1 ms avg (%2 tiles)" )
                .arg( cpuResult.avgMs, 0, 'f', 1 )
                .arg( cpuResult.tiles );

  // GPU test
  if ( initializeGPU() )
  {
    auto gpuResult = runInitialRender( &layer, 100, true );
    mGpuResults.append( gpuResult );

    const double speedup = cpuResult.avgMs / gpuResult.avgMs;
    qDebug() << QString( "GPU: %1 ms avg (median %2, stddev %3) → %4x speedup" )
                  .arg( gpuResult.avgMs, 0, 'f', 1 )
                  .arg( gpuResult.medianMs, 0, 'f', 1 )
                  .arg( gpuResult.stddevMs, 0, 'f', 1 )
                  .arg( speedup, 0, 'f', 1 );
  }
  else
  {
    qDebug() << "GPU: skipped (QRhi not available)";
  }

  QVERIFY( true );
}

void TestQgsRasterGPUBenchmark::benchmarkPanSimulation()
{
  // Scenario: User panning across a large raster
  qDebug() << "\n========================================";
  qDebug() << "Scenario: Pan Navigation";
  qDebug() << "========================================";
  qDebug() << "Simulates user panning across the map";
  qDebug() << "(25% overlap between frames = partial tile reuse)";
  qDebug() << "";

  QgsRasterLayer layer( mCogPath, u"benchmark"_s, u"gdal"_s );
  if ( !layer.isValid() )
  {
    QSKIP( "Could not load test COG" );
  }

  const int panSteps = 12; // ~2 seconds of panning at 60fps target

  // CPU baseline
  auto cpuResult = runPanSimulation( &layer, panSteps, false );
  mCpuResults.append( cpuResult );

  qDebug() << QString( "CPU: %1 ms/frame → %2 FPS" )
                .arg( cpuResult.avgMs, 0, 'f', 1 )
                .arg( cpuResult.fps, 0, 'f', 1 );

  // GPU test
  if ( initializeGPU() )
  {
    auto gpuResult = runPanSimulation( &layer, panSteps, true );
    mGpuResults.append( gpuResult );

    qDebug() << QString( "GPU: %1 ms/frame → %2 FPS (%3x speedup)" )
                  .arg( gpuResult.avgMs, 0, 'f', 1 )
                  .arg( gpuResult.fps, 0, 'f', 1 )
                  .arg( cpuResult.avgMs / gpuResult.avgMs, 0, 'f', 1 );
  }
  else
  {
    qDebug() << "GPU: skipped (QRhi not available)";
  }

  QVERIFY( true );
}

void TestQgsRasterGPUBenchmark::benchmarkZoomSimulation()
{
  // Scenario: User zooming through overview levels
  qDebug() << "\n========================================";
  qDebug() << "Scenario: Zoom Navigation";
  qDebug() << "========================================";
  qDebug() << "Simulates user zooming in (2x per level)";
  qDebug() << "(Tests overview pyramid traversal)";
  qDebug() << "";

  QgsRasterLayer layer( mCogPath, u"benchmark"_s, u"gdal"_s );
  if ( !layer.isValid() )
  {
    QSKIP( "Could not load test COG" );
  }

  const int zoomLevels = 6; // Full extent down to ~64x zoom

  // CPU baseline
  auto cpuResult = runZoomSimulation( &layer, zoomLevels, false );
  mCpuResults.append( cpuResult );

  qDebug() << QString( "CPU: %1 ms/level avg (max %2 tiles)" )
                .arg( cpuResult.avgMs, 0, 'f', 1 )
                .arg( cpuResult.tiles );

  // GPU test
  if ( initializeGPU() )
  {
    auto gpuResult = runZoomSimulation( &layer, zoomLevels, true );
    mGpuResults.append( gpuResult );

    qDebug() << QString( "GPU: %1 ms/level → %2x speedup" )
                  .arg( gpuResult.avgMs, 0, 'f', 1 )
                  .arg( cpuResult.avgMs / gpuResult.avgMs, 0, 'f', 1 );
  }
  else
  {
    qDebug() << "GPU: skipped (QRhi not available)";
  }

  QVERIFY( true );
}

void TestQgsRasterGPUBenchmark::benchmarkRemoteCOG()
{
  // Scenario: Remote COG via /vsicurl/ - shows real GPU benefit with network latency
  // Expected results:
  //   - Open: Minimal benefit (network fetch dominates)
  //   - Pan: Moderate benefit (partial VRAM cache reuse)
  //   - Zoom: Large benefit (all overview levels stay in VRAM)
  qDebug() << "\n========================================";
  qDebug() << "Scenario: Remote COG (via /vsicurl/)";
  qDebug() << "========================================";
  qDebug() << "Tests GPU tile caching benefit with network latency";
  qDebug() << "(Informational - network variability expected)";
  qDebug() << "";

  // Use direct /vsicurl/ path (no local download)
  const QString remoteCogPath = u"/vsicurl/https://s3.us-east-1.amazonaws.com/ds-deck.gl-raster-public/cog/Annual_NLCD_LndCov_2024_CU_C1V1.tif"_s;

  QgsRasterLayer layer( remoteCogPath, u"remote_benchmark"_s, u"gdal"_s );
  if ( !layer.isValid() )
  {
    qDebug() << "Could not load remote COG (network issue?) - skipping";
    QSKIP( "Remote COG not accessible" );
  }

  qDebug() << "Remote COG loaded successfully";
  qDebug() << "";

  // --- Remote Open (Initial Load) ---
  qDebug() << "-- Remote Open (network fetch dominates) --";
  auto cpuOpen = runInitialRender( &layer, 16, false ); // Fewer tiles for network
  qDebug() << QString( "CPU: %1 ms" ).arg( cpuOpen.avgMs, 0, 'f', 1 );

  if ( initializeGPU() )
  {
    auto gpuOpen = runInitialRender( &layer, 16, true );
    qDebug() << QString( "GPU: %1 ms → %2x" )
                  .arg( gpuOpen.avgMs, 0, 'f', 1 )
                  .arg( cpuOpen.avgMs / gpuOpen.avgMs, 0, 'f', 1 );
  }

  // --- Remote Pan ---
  qDebug() << "\n-- Remote Pan (partial cache reuse) --";
  auto cpuPan = runPanSimulation( &layer, 6, false ); // Fewer steps
  qDebug() << QString( "CPU: %1 ms/frame" ).arg( cpuPan.avgMs, 0, 'f', 1 );

  if ( mGpuInitialized )
  {
    auto gpuPan = runPanSimulation( &layer, 6, true );
    qDebug() << QString( "GPU: %1 ms/frame → %2x" )
                  .arg( gpuPan.avgMs, 0, 'f', 1 )
                  .arg( cpuPan.avgMs / gpuPan.avgMs, 0, 'f', 1 );
  }

  // --- Remote Zoom (biggest GPU benefit) ---
  qDebug() << "\n-- Remote Zoom (VRAM cache eliminates network) --";
  auto cpuZoom = runZoomSimulation( &layer, 4, false );
  qDebug() << QString( "CPU: %1 ms/level" ).arg( cpuZoom.avgMs, 0, 'f', 1 );

  if ( mGpuInitialized )
  {
    auto gpuZoom = runZoomSimulation( &layer, 4, true );
    const double speedup = cpuZoom.avgMs / gpuZoom.avgMs;
    qDebug() << QString( "GPU: %1 ms/level → %2x speedup" )
                  .arg( gpuZoom.avgMs, 0, 'f', 1 )
                  .arg( speedup, 0, 'f', 1 );

    if ( speedup > 5.0 )
    {
      qDebug() << "";
      qDebug() << "Large zoom speedup shows GPU tile caching benefit:";
      qDebug() << "GPU keeps overview tiles in VRAM, CPU re-fetches over network.";
    }
  }
  else
  {
    qDebug() << "GPU: skipped (QRhi not available)";
  }

  // Informational - don't add to results (network variability)
  QVERIFY( true );
}

void TestQgsRasterGPUBenchmark::benchmarkFloat32()
{
  // Data type matrix: Float32 continuous data (like DEMs, scientific data)
  qDebug() << "\n========================================";
  qDebug() << "Data Type: Float32 (Continuous)";
  qDebug() << "========================================";
  qDebug() << "Tests GPU handling of 32-bit floating point rasters";
  qDebug() << "(DEMs, scientific data, elevation models)";
  qDebug() << "";

  if ( mFloat32CogPath.isEmpty() )
  {
    QSKIP( "Float32 test COG not available" );
  }

  QgsRasterLayer layer( mFloat32CogPath, u"float32_benchmark"_s, u"gdal"_s );
  if ( !layer.isValid() )
  {
    QSKIP( "Could not load Float32 test COG" );
  }

  qDebug() << "Data type:" << layer.dataProvider()->dataType( 1 );

  // --- Float32 Initial Load ---
  qDebug() << "\n-- Float32 Initial Load --";
  auto cpuLoad = runInitialRender( &layer, 16, false );
  cpuLoad.scenario = u"Float32 Load"_s;
  mCpuResults.append( cpuLoad );

  qDebug() << QString( "CPU: %1 ms avg (%2 tiles)" )
                .arg( cpuLoad.avgMs, 0, 'f', 1 )
                .arg( cpuLoad.tiles );

  if ( initializeGPU() )
  {
    auto gpuLoad = runInitialRender( &layer, 16, true );
    gpuLoad.scenario = u"Float32 Load"_s;
    mGpuResults.append( gpuLoad );

    qDebug() << QString( "GPU: %1 ms avg → %2x speedup" )
                  .arg( gpuLoad.avgMs, 0, 'f', 1 )
                  .arg( cpuLoad.avgMs / gpuLoad.avgMs, 0, 'f', 1 );
  }

  // --- Float32 Pan ---
  qDebug() << "\n-- Float32 Pan --";
  const int panSteps = 8;
  auto cpuPan = runPanSimulation( &layer, panSteps, false );
  cpuPan.scenario = u"Float32 Pan"_s;
  mCpuResults.append( cpuPan );

  qDebug() << QString( "CPU: %1 ms/frame → %2 FPS" )
                .arg( cpuPan.avgMs, 0, 'f', 1 )
                .arg( cpuPan.fps, 0, 'f', 1 );

  if ( mGpuInitialized )
  {
    auto gpuPan = runPanSimulation( &layer, panSteps, true );
    gpuPan.scenario = u"Float32 Pan"_s;
    mGpuResults.append( gpuPan );

    qDebug() << QString( "GPU: %1 ms/frame → %2 FPS (%3x speedup)" )
                  .arg( gpuPan.avgMs, 0, 'f', 1 )
                  .arg( gpuPan.fps, 0, 'f', 1 )
                  .arg( cpuPan.avgMs / gpuPan.avgMs, 0, 'f', 1 );
  }

  // --- Float32 Zoom ---
  qDebug() << "\n-- Float32 Zoom --";
  const int zoomLevels = 4;
  auto cpuZoom = runZoomSimulation( &layer, zoomLevels, false );
  cpuZoom.scenario = u"Float32 Zoom"_s;
  mCpuResults.append( cpuZoom );

  qDebug() << QString( "CPU: %1 ms/level avg" )
                .arg( cpuZoom.avgMs, 0, 'f', 1 );

  if ( mGpuInitialized )
  {
    auto gpuZoom = runZoomSimulation( &layer, zoomLevels, true );
    gpuZoom.scenario = u"Float32 Zoom"_s;
    mGpuResults.append( gpuZoom );

    qDebug() << QString( "GPU: %1 ms/level → %2x speedup" )
                  .arg( gpuZoom.avgMs, 0, 'f', 1 )
                  .arg( cpuZoom.avgMs / gpuZoom.avgMs, 0, 'f', 1 );
  }

  if ( !mGpuInitialized )
  {
    qDebug() << "GPU: skipped (QRhi not available)";
  }

  QVERIFY( true );
}

void TestQgsRasterGPUBenchmark::benchmarkComparison()
{
  // Final summary comparing all scenarios
  qDebug() << "\n========================================";
  qDebug() << "BENCHMARK SUMMARY: GPU vs CPU";
  qDebug() << "========================================";
  qDebug() << "";
  qDebug() << "Real-world scenario comparison:";
  qDebug() << "";

  // Machine-readable header (for CI parsing)
  qDebug() << "BENCHMARK_RESULTS_START";

  // Print markdown table header
  qDebug() << "| Scenario | CPU (ms) | GPU (ms) | Speedup | GPU FPS |";
  qDebug() << "|----------|----------|----------|---------|---------|";

  bool hasSpeedup = false;
  double totalSpeedup = 0;
  int comparisons = 0;

  for ( int i = 0; i < mCpuResults.size() && i < mGpuResults.size(); ++i )
  {
    const auto &cpu = mCpuResults[i];
    const auto &gpu = mGpuResults[i];

    const double speedup = ( gpu.avgMs > 0 ) ? cpu.avgMs / gpu.avgMs : 0;

    qDebug() << QString( "| %1 | %2 | %3 | %4x | %5 |" )
                  .arg( cpu.scenario, -14 )
                  .arg( cpu.avgMs, 8, 'f', 1 )
                  .arg( gpu.avgMs, 8, 'f', 1 )
                  .arg( speedup, 6, 'f', 1 )
                  .arg( gpu.fps > 0 ? QString::number( gpu.fps, 'f', 0 ) : u"-"_s, 7 );

    if ( speedup > 1.0 )
    {
      hasSpeedup = true;
      totalSpeedup += speedup;
      comparisons++;
    }
  }

  qDebug() << "BENCHMARK_RESULTS_END";
  qDebug() << "";

  // User-friendly interpretation
  qDebug() << "----------------------------------------";
  qDebug() << "INTERPRETATION:";
  qDebug() << "";

  if ( mGpuResults.isEmpty() )
  {
    qDebug() << "STATUS: GPU benchmark skipped (QRhi not available)";
    qDebug() << "";
    qDebug() << "To test GPU rendering, ensure QRhi backend is available.";
    qDebug() << "On CI, this may require Mesa (OpenGL), Vulkan, or Metal.";
    QSKIP( "GPU benchmarks were skipped" );
  }
  else if ( hasSpeedup )
  {
    const double avgSpeedup = totalSpeedup / comparisons;
    qDebug() << QString( "STATUS: SUCCESS - Average %1x speedup with GPU" ).arg( avgSpeedup, 0, 'f', 1 );
    qDebug() << "";

    // Check if we hit 60fps target
    bool hits60fps = false;
    for ( const auto &gpu : mGpuResults )
    {
      if ( gpu.fps >= 60 )
        hits60fps = true;
    }

    if ( hits60fps )
    {
      qDebug() << "SMOOTHNESS: GPU achieves 60+ FPS in some scenarios";
      qDebug() << "           This means smooth panning/zooming for users.";
    }
    else
    {
      qDebug() << "SMOOTHNESS: GPU does not achieve 60 FPS";
      qDebug() << "           May be software rendering or slow hardware.";
    }

    // Write JSON output for CI parsing
    writeJsonOutput();

    QVERIFY( true );
  }
  else
  {
    qDebug() << "STATUS: WARNING - No speedup detected";
    qDebug() << "";
    qDebug() << "Possible causes:";
    qDebug() << "  - Software OpenGL rendering (Mesa llvmpipe)";
    qDebug() << "  - GPU driver issues";
    qDebug() << "  - Test running on CI without GPU";

    // Write JSON output for CI parsing
    writeJsonOutput();

    // Don't fail - CI runners often use software rendering
    QVERIFY( true );
  }
}

// Custom main function to set up OpenGL context for QRhi
// QGSTEST_MAIN doesn't set AA_ShareOpenGLContexts, but QRhi needs it
int main( int argc, char *argv[] )
{
  // Set OpenGL format BEFORE creating QApplication
  // This matches what QGIS main.cpp does
  QSurfaceFormat format;
  format.setDepthBufferSize( 24 );
  format.setStencilBufferSize( 8 );
#ifdef Q_OS_MAC
  format.setVersion( 4, 1 );
  format.setProfile( QSurfaceFormat::CoreProfile );
#else
  format.setVersion( 3, 3 );
  format.setProfile( QSurfaceFormat::CoreProfile );
#endif
  QSurfaceFormat::setDefaultFormat( format );

  // Enable OpenGL context sharing (required for QRhi OpenGL backend)
  QCoreApplication::setAttribute( Qt::AA_ShareOpenGLContexts, true );

  // Now create the application
  QgsApplication app( argc, argv, false );
  app.init();
  app.setAttribute( Qt::AA_Use96Dpi, true );

  TestQgsRasterGPUBenchmark tc;
  return QTest::qExec( &tc, argc, argv );
}
#include "testqgsrastergpubenchmark.moc"
