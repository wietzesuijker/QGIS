/***************************************************************************
  testqgsrastergpuparity.cpp - GPU vs CPU rendering parity tests
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

#include <cpl_conv.h>
#include <gdal.h>

#include "qgsapplication.h"
#include "qgsbrightnesscontrastfilter.h"
#include "qgscolorrampshader.h"
#include "qgshuesaturationfilter.h"
#include "qgsmaprenderersequentialjob.h"
#include "qgsmapsettings.h"
#include "qgspalettedrasterrenderer.h"
#include "qgsproject.h"
#include "qgsrastergpufactory.h"
#include "qgsrasterlayer.h"
#include "qgsrasterlayerrenderer.h"
#include "qgsrasterpipe.h"
#include "qgsrastershader.h"
#include "qgssinglebandpseudocolorrenderer.h"
#include "qgstest.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTemporaryFile>

using namespace Qt::StringLiterals;

/**
 * \ingroup UnitTests
 * GPU vs CPU rendering parity tests.
 *
 * Verifies that GPU-accelerated raster rendering produces output
 * visually identical to CPU rendering (within tolerance).
 *
 * Test tolerance: color_tolerance=1 (1/255 per channel), max_mismatch=5 pixels.
 * Rationale: float→int conversion in shader vs CPU may differ by ±1.
 */
class TestQgsRasterGPUParity : public QgsTest
{
    Q_OBJECT

  public:
    TestQgsRasterGPUParity()
      : QgsTest( u"GPU/CPU Raster Parity Tests"_s ) {}

  private slots:
    void initTestCase();
    void cleanupTestCase();

    // Brightness/Contrast/Gamma filter tests
    void testBrightnessPositive();
    void testBrightnessNegative();
    void testContrastPositive();
    void testContrastNegative();
    void testGammaLow();
    void testGammaHigh();
    void testBrightnessContrastCombined();

    // Hue/Saturation filter tests
    void testSaturationPositive();
    void testSaturationNegative();
    void testGrayscaleLightness();
    void testGrayscaleLuminosity();
    void testGrayscaleAverage();
    void testInvertColors();
    void testColorize();
    void testColorizeWithStrength();

    // Boundary condition tests (Tier 1 critical)
    void testBrightnessMinMax();
    void testContrastMinMax();
    void testGammaMinMax();
    void testSaturationMinMax();

    // Precision and combination tests
    void testNeutralGray();
    void testBrightnessPlusSaturation();
    void testInvertPlusColorize();
    void testGrayscalePlusColorize();

    // Layer opacity test
    void testLayerOpacity();

    // Pseudocolor renderer tests
    void testPseudocolorLinear();
    void testPseudocolorDiscrete();

    // Paletted raster renderer tests
    void testPalettedRaster();

  private:
    /**
     * Compare two QImages pixel-by-pixel with tolerance.
     * \param cpu CPU-rendered image
     * \param gpu GPU-rendered image
     * \param colorTolerance Maximum difference per color channel (0-255)
     * \param maxMismatch Maximum number of mismatched pixels allowed
     * \returns true if images are within tolerance
     */
    bool compareImages( const QImage &cpu, const QImage &gpu, int colorTolerance, int maxMismatch );

    /**
     * Render the layer using current settings.
     * \param layer Raster layer to render
     * \param useGpu If true, use GPU path; if false, temporarily disable GPU
     * \returns Rendered QImage
     */
    QImage renderLayer( QgsRasterLayer *layer, bool useGpu );

    /**
     * Run parity test with current layer settings.
     * \param testName Name for debug output
     * \returns true if GPU and CPU outputs match within tolerance
     */
    bool runParityTest( const QString &testName );

    /**
     * Create a synthetic single-band GeoTIFF for testing.
     * \param filePath Path to create the file
     * \param width Image width
     * \param height Image height
     * \param minValue Minimum raster value
     * \param maxValue Maximum raster value
     * \returns true if creation succeeded
     */
    bool createSyntheticRaster( const QString &filePath, int width, int height, double minValue, double maxValue );

    /**
     * Set up test layer from default raster. Skips test if unavailable.
     * \returns true if layer is ready for testing
     */
    bool setupLayer();

    QString mTestDataDir;
    QString mRasterPath;
    std::unique_ptr<QgsRasterLayer> mLayer;
    QgsRectangle mExtent;
    QSize mOutputSize { 256, 256 };
    QTemporaryDir mTempDir;

    // Tolerance settings
    static constexpr int COLOR_TOLERANCE = 1;          // Allow 1/255 difference per channel
    static constexpr int MAX_MISMATCH = 5;             // Allow 5 mismatched pixels per 256x256
    static constexpr int BOUNDARY_COLOR_TOLERANCE = 2; // Slightly higher for extreme values
    static constexpr int BOUNDARY_MAX_MISMATCH = 10;   // Allow more mismatch for boundary tests
};

void TestQgsRasterGPUParity::initTestCase()
{
  // Set up OpenGL context sharing (required for QRhi)
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

  QgsApplication::init();
  QgsApplication::initQgis();

  // Initialize GPU rendering
  QgsRasterGPUFactory::initialize();

  mTestDataDir = QStringLiteral( TEST_DATA_DIR );

  // Find a suitable test raster
  mRasterPath = mTestDataDir + "/landsat.tif";
  if ( !QFile::exists( mRasterPath ) )
  {
    // Try alternate locations
    QDir testDir( mTestDataDir + "/raster" );
    const QStringList tiffs = testDir.entryList( QStringList() << "*.tif" << "*.tiff", QDir::Files );
    if ( !tiffs.isEmpty() )
    {
      mRasterPath = testDir.absoluteFilePath( tiffs.first() );
    }
  }

  qDebug() << "Using test raster:" << mRasterPath;
}

void TestQgsRasterGPUParity::cleanupTestCase()
{
  mLayer.reset();
  QgsRasterGPUFactory::cleanup();
  QgsApplication::exitQgis();
}

bool TestQgsRasterGPUParity::setupLayer()
{
  if ( !QFile::exists( mRasterPath ) )
    return false;

  mLayer = std::make_unique<QgsRasterLayer>( mRasterPath, u"test"_s, u"gdal"_s );
  if ( !mLayer->isValid() )
    return false;

  mExtent = mLayer->extent();
  return true;
}

bool TestQgsRasterGPUParity::compareImages( const QImage &cpu, const QImage &gpu, int colorTolerance, int maxMismatch )
{
  if ( cpu.size() != gpu.size() )
  {
    qDebug() << "Image size mismatch:" << cpu.size() << "vs" << gpu.size();
    return false;
  }

  int mismatchCount = 0;
  for ( int y = 0; y < cpu.height(); ++y )
  {
    for ( int x = 0; x < cpu.width(); ++x )
    {
      const QColor cpuPixel = cpu.pixelColor( x, y );
      const QColor gpuPixel = gpu.pixelColor( x, y );

      const int dr = std::abs( cpuPixel.red() - gpuPixel.red() );
      const int dg = std::abs( cpuPixel.green() - gpuPixel.green() );
      const int db = std::abs( cpuPixel.blue() - gpuPixel.blue() );
      const int da = std::abs( cpuPixel.alpha() - gpuPixel.alpha() );

      if ( dr > colorTolerance || dg > colorTolerance || db > colorTolerance || da > colorTolerance )
      {
        ++mismatchCount;
        if ( mismatchCount <= 3 )
        {
          qDebug() << QString( "Pixel mismatch at (%1,%2): CPU=(%3,%4,%5,%6) GPU=(%7,%8,%9,%10) diff=(%11,%12,%13,%14)" )
                        .arg( x )
                        .arg( y )
                        .arg( cpuPixel.red() )
                        .arg( cpuPixel.green() )
                        .arg( cpuPixel.blue() )
                        .arg( cpuPixel.alpha() )
                        .arg( gpuPixel.red() )
                        .arg( gpuPixel.green() )
                        .arg( gpuPixel.blue() )
                        .arg( gpuPixel.alpha() )
                        .arg( dr )
                        .arg( dg )
                        .arg( db )
                        .arg( da );
        }
      }
    }
  }

  if ( mismatchCount > maxMismatch )
  {
    qDebug() << "Too many mismatched pixels:" << mismatchCount << "(max allowed:" << maxMismatch << ")";
    return false;
  }

  if ( mismatchCount > 0 )
  {
    qDebug() << "Parity check passed with" << mismatchCount << "minor mismatches (within tolerance)";
  }

  return true;
}

QImage TestQgsRasterGPUParity::renderLayer( QgsRasterLayer *layer, bool useGpu )
{
  // Temporarily disable GPU if requested
  if ( !useGpu )
  {
    QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );
  }

  QgsMapSettings settings;
  settings.setLayers( { layer } );
  settings.setExtent( mExtent );
  settings.setOutputSize( mOutputSize );
  settings.setDestinationCrs( layer->crs() );
  settings.setBackgroundColor( Qt::white );

  QgsMapRendererSequentialJob job( settings );
  job.start();
  job.waitForFinished();

  QImage result = job.renderedImage();

  // Re-enable GPU if it was disabled
  if ( !useGpu )
  {
    QgsRasterGPUFactory::initialize();
  }

  return result;
}

bool TestQgsRasterGPUParity::runParityTest( const QString &testName )
{
  qDebug() << "\n=== Parity Test:" << testName << "===";

  // Render with GPU
  QImage gpuImage = renderLayer( mLayer.get(), true );
  if ( gpuImage.isNull() )
  {
    qDebug() << "GPU rendering failed";
    return false;
  }

  // Render with CPU (GPU disabled)
  QImage cpuImage = renderLayer( mLayer.get(), false );
  if ( cpuImage.isNull() )
  {
    qDebug() << "CPU rendering failed";
    return false;
  }

  // Compare
  const bool passed = compareImages( cpuImage, gpuImage, COLOR_TOLERANCE, MAX_MISMATCH );

  if ( passed )
  {
    qDebug() << "PASS:" << testName;
  }
  else
  {
    qDebug() << "FAIL:" << testName;
    // Save images for debugging
    const QString baseName = testName.toLower().replace( ' ', '_' );
    cpuImage.save( u"/tmp/%1_cpu.png"_s.arg( baseName ) );
    gpuImage.save( u"/tmp/%1_gpu.png"_s.arg( baseName ) );
    qDebug() << "Saved debug images to /tmp/";
  }

  return passed;
}

// ============================================================================
// Brightness/Contrast/Gamma Tests
// ============================================================================

void TestQgsRasterGPUParity::testBrightnessPositive()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setBrightness( 50 );
  QVERIFY( runParityTest( u"Brightness +50"_s ) );
}

void TestQgsRasterGPUParity::testBrightnessNegative()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setBrightness( -50 );
  QVERIFY( runParityTest( u"Brightness -50"_s ) );
}

void TestQgsRasterGPUParity::testContrastPositive()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setContrast( 50 );
  QVERIFY( runParityTest( u"Contrast +50"_s ) );
}

void TestQgsRasterGPUParity::testContrastNegative()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setContrast( -50 );
  QVERIFY( runParityTest( u"Contrast -50"_s ) );
}

void TestQgsRasterGPUParity::testGammaLow()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setGamma( 0.5 );
  QVERIFY( runParityTest( u"Gamma 0.5"_s ) );
}

void TestQgsRasterGPUParity::testGammaHigh()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setGamma( 2.0 );
  QVERIFY( runParityTest( u"Gamma 2.0"_s ) );
}

void TestQgsRasterGPUParity::testBrightnessContrastCombined()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  auto *bcFilter = mLayer->pipe()->brightnessFilter();
  bcFilter->setBrightness( 30 );
  bcFilter->setContrast( 20 );
  bcFilter->setGamma( 1.2 );
  QVERIFY( runParityTest( u"Brightness+Contrast+Gamma"_s ) );
}

// ============================================================================
// Hue/Saturation Tests
// ============================================================================

void TestQgsRasterGPUParity::testSaturationPositive()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setSaturation( 50 );
  QVERIFY( runParityTest( u"Saturation +50"_s ) );
}

void TestQgsRasterGPUParity::testSaturationNegative()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setSaturation( -50 );
  QVERIFY( runParityTest( u"Saturation -50"_s ) );
}

void TestQgsRasterGPUParity::testGrayscaleLightness()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setGrayscaleMode( QgsHueSaturationFilter::GrayscaleLightness );
  QVERIFY( runParityTest( u"Grayscale Lightness"_s ) );
}

void TestQgsRasterGPUParity::testGrayscaleLuminosity()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setGrayscaleMode( QgsHueSaturationFilter::GrayscaleLuminosity );
  QVERIFY( runParityTest( u"Grayscale Luminosity"_s ) );
}

void TestQgsRasterGPUParity::testGrayscaleAverage()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setGrayscaleMode( QgsHueSaturationFilter::GrayscaleAverage );
  QVERIFY( runParityTest( u"Grayscale Average"_s ) );
}

void TestQgsRasterGPUParity::testInvertColors()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->hueSaturationFilter()->setInvertColors( true );
  QVERIFY( runParityTest( u"Invert Colors"_s ) );
}

void TestQgsRasterGPUParity::testColorize()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  auto *hsFilter = mLayer->pipe()->hueSaturationFilter();
  hsFilter->setColorizeOn( true );
  hsFilter->setColorizeColor( QColor( 200, 150, 100 ) );
  hsFilter->setColorizeStrength( 100 );
  QVERIFY( runParityTest( u"Colorize 100%"_s ) );
}

void TestQgsRasterGPUParity::testColorizeWithStrength()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  auto *hsFilter = mLayer->pipe()->hueSaturationFilter();
  hsFilter->setColorizeOn( true );
  hsFilter->setColorizeColor( QColor( 100, 150, 200 ) );
  hsFilter->setColorizeStrength( 50 );
  QVERIFY( runParityTest( u"Colorize 50%"_s ) );
}

// ============================================================================
// Boundary Condition Tests (Tier 1 Critical)
// ============================================================================

void TestQgsRasterGPUParity::testBrightnessMinMax()
{
  // Test boundary values: -255 and +255
  auto testBoundary = [this]( int value ) {
    if ( !setupLayer() )
      return false;
    mLayer->pipe()->brightnessFilter()->setBrightness( value );
    bool ok = compareImages( renderLayer( mLayer.get(), false ), renderLayer( mLayer.get(), true ), BOUNDARY_COLOR_TOLERANCE, BOUNDARY_MAX_MISMATCH );
    qDebug() << u"Brightness %1: %2"_s.arg( value ).arg( ok ? "PASS" : "FAIL" );
    return ok;
  };
  QVERIFY( testBoundary( -255 ) );
  QVERIFY( testBoundary( 255 ) );
}

void TestQgsRasterGPUParity::testContrastMinMax()
{
  // Test boundary values: -100 and +100
  auto testBoundary = [this]( int value ) {
    if ( !setupLayer() )
      return false;
    mLayer->pipe()->brightnessFilter()->setContrast( value );
    bool ok = compareImages( renderLayer( mLayer.get(), false ), renderLayer( mLayer.get(), true ), BOUNDARY_COLOR_TOLERANCE, BOUNDARY_MAX_MISMATCH );
    qDebug() << u"Contrast %1: %2"_s.arg( value ).arg( ok ? "PASS" : "FAIL" );
    return ok;
  };
  QVERIFY( testBoundary( -100 ) );
  QVERIFY( testBoundary( 100 ) );
}

void TestQgsRasterGPUParity::testGammaMinMax()
{
  // Test boundary values: 0.1 and 10.0
  auto testBoundary = [this]( double value ) {
    if ( !setupLayer() )
      return false;
    mLayer->pipe()->brightnessFilter()->setGamma( value );
    bool ok = compareImages( renderLayer( mLayer.get(), false ), renderLayer( mLayer.get(), true ), BOUNDARY_COLOR_TOLERANCE, BOUNDARY_MAX_MISMATCH );
    qDebug() << u"Gamma %1: %2"_s.arg( value ).arg( ok ? "PASS" : "FAIL" );
    return ok;
  };
  QVERIFY( testBoundary( 0.1 ) );
  QVERIFY( testBoundary( 10.0 ) );
}

void TestQgsRasterGPUParity::testSaturationMinMax()
{
  // Test boundary values: -100 and +100
  auto testBoundary = [this]( int value ) {
    if ( !setupLayer() )
      return false;
    mLayer->pipe()->hueSaturationFilter()->setSaturation( value );
    bool ok = compareImages( renderLayer( mLayer.get(), false ), renderLayer( mLayer.get(), true ), BOUNDARY_COLOR_TOLERANCE, BOUNDARY_MAX_MISMATCH );
    qDebug() << u"Saturation %1: %2"_s.arg( value ).arg( ok ? "PASS" : "FAIL" );
    return ok;
  };
  QVERIFY( testBoundary( -100 ) );
  QVERIFY( testBoundary( 100 ) );
}

// ============================================================================
// Precision and Combination Tests
// ============================================================================

void TestQgsRasterGPUParity::testNeutralGray()
{
  // Neutral gray (128,128,128) most sensitive to contrast artifacts
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setContrast( 75 );
  QVERIFY( runParityTest( u"Neutral Gray Contrast Sensitivity"_s ) );
}

void TestQgsRasterGPUParity::testBrightnessPlusSaturation()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->pipe()->brightnessFilter()->setBrightness( 40 );
  mLayer->pipe()->brightnessFilter()->setContrast( 30 );
  mLayer->pipe()->hueSaturationFilter()->setSaturation( 50 );
  QVERIFY( runParityTest( u"Brightness+Saturation Combined"_s ) );
}

void TestQgsRasterGPUParity::testInvertPlusColorize()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  auto *hsFilter = mLayer->pipe()->hueSaturationFilter();
  hsFilter->setInvertColors( true );
  hsFilter->setColorizeOn( true );
  hsFilter->setColorizeColor( QColor( 180, 120, 80 ) );
  hsFilter->setColorizeStrength( 70 );
  QVERIFY( runParityTest( u"Invert+Colorize Combined"_s ) );
}

void TestQgsRasterGPUParity::testGrayscalePlusColorize()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  auto *hsFilter = mLayer->pipe()->hueSaturationFilter();
  hsFilter->setGrayscaleMode( QgsHueSaturationFilter::GrayscaleLuminosity );
  hsFilter->setColorizeOn( true );
  hsFilter->setColorizeColor( QColor( 100, 100, 180 ) );
  hsFilter->setColorizeStrength( 80 );
  QVERIFY( runParityTest( u"Grayscale+Colorize Combined"_s ) );
}

// ============================================================================
// Layer Opacity Test
// ============================================================================

void TestQgsRasterGPUParity::testLayerOpacity()
{
  if ( !setupLayer() )
    QSKIP( "Test raster not found" );
  mLayer->setOpacity( 0.5 );
  QVERIFY( runParityTest( u"Layer Opacity 50%"_s ) );
}

// ============================================================================
// Pseudocolor Renderer Tests
// ============================================================================

bool TestQgsRasterGPUParity::createSyntheticRaster( const QString &filePath, int width, int height, double minValue, double maxValue )
{
  GDALAllRegister();

  GDALDriverH driver = GDALGetDriverByName( "GTiff" );
  if ( !driver )
  {
    qDebug() << "Failed to get GTiff driver";
    return false;
  }

  GDALDatasetH dataset = GDALCreate( driver, filePath.toUtf8().constData(), width, height, 1, GDT_Float32, nullptr );
  if ( !dataset )
  {
    qDebug() << "Failed to create dataset";
    return false;
  }

  // Set up a simple georeferencing
  double geoTransform[6] = { 0.0, 1.0, 0.0, static_cast<double>( height ), 0.0, -1.0 };
  GDALSetGeoTransform( dataset, geoTransform );

  // Create a gradient raster from minValue to maxValue
  GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
  std::vector<float> data( width * height );

  for ( int y = 0; y < height; ++y )
  {
    for ( int x = 0; x < width; ++x )
    {
      // Create a diagonal gradient
      double t = ( static_cast<double>( x + y ) ) / ( width + height - 2 );
      data[y * width + x] = static_cast<float>( minValue + t * ( maxValue - minValue ) );
    }
  }

  CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, width, height, data.data(), width, height, GDT_Float32, 0, 0 );
  if ( err != CE_None )
  {
    qDebug() << "Failed to write raster data";
    GDALClose( dataset );
    return false;
  }

  GDALClose( dataset );
  return true;
}

void TestQgsRasterGPUParity::testPseudocolorLinear()
{
  // Create a synthetic single-band raster for pseudocolor testing
  const QString syntheticPath = mTempDir.filePath( u"synthetic_gradient.tif"_s );
  if ( !createSyntheticRaster( syntheticPath, 256, 256, 0.0, 100.0 ) )
  {
    QSKIP( "Failed to create synthetic raster" );
  }

  mLayer = std::make_unique<QgsRasterLayer>( syntheticPath, u"test"_s, u"gdal"_s );
  QVERIFY( mLayer->isValid() );
  mExtent = mLayer->extent();

  // Create a linear color ramp shader
  auto shader = std::make_unique<QgsRasterShader>();
  auto colorRampShader = std::make_unique<QgsColorRampShader>();

  QList<QgsColorRampShader::ColorRampItem> colorRampItems;
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 0.0, QColor( 0, 0, 255 ), u"Low"_s ) );    // Blue
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 50.0, QColor( 0, 255, 0 ), u"Mid"_s ) );   // Green
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 100.0, QColor( 255, 0, 0 ), u"High"_s ) ); // Red

  colorRampShader->setColorRampItemList( colorRampItems );
  colorRampShader->setColorRampType( Qgis::ShaderInterpolationMethod::Linear );

  shader->setRasterShaderFunction( colorRampShader.release() );

  auto renderer = std::make_unique<QgsSingleBandPseudoColorRenderer>( mLayer->dataProvider(), 1, shader.release() );
  mLayer->setRenderer( renderer.release() );

  QVERIFY( runParityTest( u"Pseudocolor Linear"_s ) );
}

void TestQgsRasterGPUParity::testPseudocolorDiscrete()
{
  // Create a synthetic single-band raster for pseudocolor testing
  const QString syntheticPath = mTempDir.filePath( u"synthetic_gradient_discrete.tif"_s );
  if ( !createSyntheticRaster( syntheticPath, 256, 256, 0.0, 100.0 ) )
  {
    QSKIP( "Failed to create synthetic raster" );
  }

  mLayer = std::make_unique<QgsRasterLayer>( syntheticPath, u"test"_s, u"gdal"_s );
  QVERIFY( mLayer->isValid() );
  mExtent = mLayer->extent();

  // Create a discrete color ramp shader
  auto shader = std::make_unique<QgsRasterShader>();
  auto colorRampShader = std::make_unique<QgsColorRampShader>();

  QList<QgsColorRampShader::ColorRampItem> colorRampItems;
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 25.0, QColor( 0, 0, 255 ), u"Low"_s ) );        // Blue < 25
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 50.0, QColor( 0, 255, 0 ), u"Mid-Low"_s ) );    // Green < 50
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 75.0, QColor( 255, 255, 0 ), u"Mid-High"_s ) ); // Yellow < 75
  colorRampItems.append( QgsColorRampShader::ColorRampItem( 100.0, QColor( 255, 0, 0 ), u"High"_s ) );      // Red >= 75

  colorRampShader->setColorRampItemList( colorRampItems );
  colorRampShader->setColorRampType( Qgis::ShaderInterpolationMethod::Discrete );

  shader->setRasterShaderFunction( colorRampShader.release() );

  auto renderer = std::make_unique<QgsSingleBandPseudoColorRenderer>( mLayer->dataProvider(), 1, shader.release() );
  mLayer->setRenderer( renderer.release() );

  QVERIFY( runParityTest( u"Pseudocolor Discrete"_s ) );
}

void TestQgsRasterGPUParity::testPalettedRaster()
{
  // Create a synthetic single-band byte raster for paletted testing
  const QString syntheticPath = mTempDir.filePath( u"synthetic_paletted.tif"_s );
  if ( !createSyntheticRaster( syntheticPath, 256, 256, 0.0, 255.0 ) )
  {
    QSKIP( "Failed to create synthetic raster" );
  }

  mLayer = std::make_unique<QgsRasterLayer>( syntheticPath, u"test"_s, u"gdal"_s );
  QVERIFY( mLayer->isValid() );
  mExtent = mLayer->extent();

  // Create a paletted renderer with discrete color classes
  QgsPalettedRasterRenderer::ClassData classes;
  // Create classes for value ranges (categorical land cover style)
  classes.append( QgsPalettedRasterRenderer::Class( 0, QColor( 0, 0, 128 ), u"Water"_s ) );       // Dark blue
  classes.append( QgsPalettedRasterRenderer::Class( 50, QColor( 34, 139, 34 ), u"Forest"_s ) );   // Forest green
  classes.append( QgsPalettedRasterRenderer::Class( 100, QColor( 210, 180, 140 ), u"Urban"_s ) ); // Tan
  classes.append( QgsPalettedRasterRenderer::Class( 150, QColor( 255, 215, 0 ), u"Crop"_s ) );    // Gold
  classes.append( QgsPalettedRasterRenderer::Class( 200, QColor( 139, 69, 19 ), u"Bare"_s ) );    // Brown
  classes.append( QgsPalettedRasterRenderer::Class( 255, QColor( 255, 255, 255 ), u"Snow"_s ) );  // White

  auto renderer = std::make_unique<QgsPalettedRasterRenderer>( mLayer->dataProvider(), 1, classes );
  mLayer->setRenderer( renderer.release() );

  QVERIFY( runParityTest( u"Paletted Raster"_s ) );
}

// Custom main to set up OpenGL context for QRhi
int main( int argc, char *argv[] )
{
  QCoreApplication::setAttribute( Qt::AA_ShareOpenGLContexts, true );
  QgsApplication app( argc, argv, false );
  app.init();
  app.setAttribute( Qt::AA_Use96Dpi, true );

  TestQgsRasterGPUParity tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "testqgsrastergpuparity.moc"
