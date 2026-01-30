/***************************************************************************
  testqgsrastergpu.cpp - Test GPU raster rendering components
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

#include "qgstest.h"
#include "qgsapplication.h"
#include "qgsrastertextureformats.h"
#include "qgsrastertilereader.h"

#include <QDir>
#include <QFile>
#include <gdal.h>

/**
 * \ingroup UnitTests
 * Unit tests for GPU raster rendering components.
 *
 * \note Shader tests removed - QRhi uses precompiled .qsb binaries,
 * not runtime QOpenGLShaderProgram compilation.
 *
 * \note Tile uploader tests require QRhi context, tested via
 * testqgsrastergpubenchmark.cpp which has full GPU pipeline.
 */
class TestQgsRasterGPU : public QgsTest
{
    Q_OBJECT

  public:
    TestQgsRasterGPU()
      : QgsTest( QStringLiteral( "GPU Raster Rendering Tests" ) ) {}

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void testTextureFormatMapping();
    void testTileReader();

  private:
    QString mTestDataDir;
    QString mCogFile;
};

void TestQgsRasterGPU::initTestCase()
{
  QgsApplication::init();
  QgsApplication::initQgis();

  mTestDataDir = QStringLiteral( TEST_DATA_DIR );
  mCogFile = mTestDataDir + "/raster/test_cog.tif";

  // Try to find any GeoTIFF for testing
  if ( !QFile::exists( mCogFile ) )
  {
    QDir testDir( mTestDataDir + "/raster" );
    const QStringList tiffs = testDir.entryList( QStringList() << "*.tif" << "*.tiff", QDir::Files );
    if ( !tiffs.isEmpty() )
    {
      mCogFile = testDir.absoluteFilePath( tiffs.first() );
      qDebug() << "Using test file:" << mCogFile;
    }
  }
}

void TestQgsRasterGPU::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsRasterGPU::testTextureFormatMapping()
{
  // Test format mapping for different data types

  // Byte, single band
  auto format = QgsRasterTextureFormats::getFormat( Qgis::DataType::Byte, 1 );
  QVERIFY( format.isSupported );
  QCOMPARE( format.channelCount, 1 );
  QCOMPARE( format.bytesPerPixel, 1 );
  QCOMPARE( format.shaderType, QString( "u8" ) );

  // UInt16, single band
  format = QgsRasterTextureFormats::getFormat( Qgis::DataType::UInt16, 1 );
  QVERIFY( format.isSupported );
  QCOMPARE( format.channelCount, 1 );
  QCOMPARE( format.bytesPerPixel, 2 );
  QCOMPARE( format.shaderType, QString( "u16" ) );

  // Float32, single band
  format = QgsRasterTextureFormats::getFormat( Qgis::DataType::Float32, 1 );
  QVERIFY( format.isSupported );
  QCOMPARE( format.channelCount, 1 );
  QCOMPARE( format.bytesPerPixel, 4 );
  QCOMPARE( format.shaderType, QString( "f32" ) );

  // Byte, RGB
  format = QgsRasterTextureFormats::getFormat( Qgis::DataType::Byte, 3 );
  QVERIFY( format.isSupported );
  QCOMPARE( format.channelCount, 3 );
  QCOMPARE( format.bytesPerPixel, 3 );

  // Byte, RGBA
  format = QgsRasterTextureFormats::getFormat( Qgis::DataType::Byte, 4 );
  QVERIFY( format.isSupported );
  QCOMPARE( format.channelCount, 4 );
  QCOMPARE( format.bytesPerPixel, 4 );

  // Test fallback for unsupported format
  format = QgsRasterTextureFormats::getFormat( Qgis::DataType::CFloat64, 1 );
  QVERIFY( !format.isSupported ); // Should fall back with unsupported flag
}

void TestQgsRasterGPU::testTileReader()
{
  if ( !QFile::exists( mCogFile ) )
  {
    QSKIP( "No test raster file available" );
  }

  // Open dataset
  GDALDatasetH dataset = GDALOpen( mCogFile.toUtf8().constData(), GA_ReadOnly );
  QVERIFY( dataset != nullptr );

  // Create tile reader
  QgsRasterTileReader reader( dataset );
  QVERIFY( reader.isValid() );

  // Test basic properties
  QVERIFY( reader.width() > 0 );
  QVERIFY( reader.height() > 0 );
  QVERIFY( !reader.extent().isEmpty() );

  // Test tile info
  const auto info = reader.tileInfo( 0 );
  QVERIFY( info.width > 0 );
  QVERIFY( info.height > 0 );
  QVERIFY( info.bytesPerPixel > 0 );

  // Test tile reading
  QByteArray buffer;
  const bool success = reader.readTile( 0, 0, 0, 1, buffer );
  QVERIFY( success );
  QVERIFY( !buffer.isEmpty() );
  QCOMPARE( buffer.size(), info.width * info.height * info.bytesPerPixel );

  // Test overview selection
  const double mupp = reader.extent().width() / reader.width();
  const int bestOverview = reader.selectBestOverview( mupp * 2.0 );
  QVERIFY( bestOverview >= 0 );
  QVERIFY( bestOverview <= reader.overviewCount() );

  GDALClose( dataset );
}

QGSTEST_MAIN( TestQgsRasterGPU )
#include "testqgsrastergpu.moc"
