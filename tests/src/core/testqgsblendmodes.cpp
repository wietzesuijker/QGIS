/***************************************************************************
     testqgsblendmodes.cpp
     --------------------------------------
    Date                 : May 2013
    Copyright            : (C) 2013 by Nyall Dawson, Tim Sutton
    Email                : nyall dot dawson at gmail.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgstest.h"

#include <QApplication>
#include <QDir>
#include <QObject>
#include <QPainter>
#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

//qgis includes...
#include <qgsmapsettings.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgsapplication.h>
#include <qgsproviderregistry.h>
#include <qgsproject.h>
#include <qgsmultibandcolorrenderer.h>
#include <qgsrasterlayer.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsrastertransparency.h>
#include "qgsrasterdataprovider.h"

/**
 * \ingroup UnitTests
 * This is a unit test for layer blend modes
 */
class TestQgsBlendModes : public QgsTest
{
    Q_OBJECT

  public:
    TestQgsBlendModes()
      : QgsTest( u"Blending modes"_s ) {}

    ~TestQgsBlendModes() override
    {
      delete mMapSettings;
    }

  private slots:
    void initTestCase();    // will be called before the first testfunction is executed.
    void cleanupTestCase(); // will be called after the last testfunction was executed.
    void init() {}          // will be called before each testfunction is executed.
    void cleanup() {}       // will be called after every testfunction.

    void vectorBlending();
    void featureBlending();
    void vectorLayerTransparency();
    void rasterBlending();
    void rasterBlendingWithTransparency();

  private:
    QgsMapSettings *mMapSettings = nullptr;
    QgsMapLayer *mpPointsLayer = nullptr;
    QgsVectorLayer *mpPolysLayer = nullptr;
    QgsVectorLayer *mpLinesLayer = nullptr;
    QgsRasterLayer *mRasterLayer1 = nullptr;
    QgsRasterLayer *mRasterLayer2 = nullptr;
    QString mTestDataDir;
    QgsRectangle mExtent;
};


void TestQgsBlendModes::initTestCase()
{
  // init QGIS's paths - true means that all path will be inited from prefix
  QgsApplication::init();
  QgsApplication::initQgis();
  QgsApplication::showSettings();

  mMapSettings = new QgsMapSettings();

  mMapSettings->setOutputDpi( 96 );
  //create some objects that will be used in tests

  //create a point layer
  const QString myDataDir( TEST_DATA_DIR ); //defined in CmakeLists.txt
  mTestDataDir = myDataDir + '/';
  const QString myPointsFileName = mTestDataDir + "points.shp";
  const QFileInfo myPointFileInfo( myPointsFileName );
  mpPointsLayer = new QgsVectorLayer( myPointFileInfo.filePath(), myPointFileInfo.completeBaseName(), u"ogr"_s );

  //create a poly layer that will be used in tests
  const QString myPolysFileName = mTestDataDir + "polys.shp";
  const QFileInfo myPolyFileInfo( myPolysFileName );
  mpPolysLayer = new QgsVectorLayer( myPolyFileInfo.filePath(), myPolyFileInfo.completeBaseName(), u"ogr"_s );

  QgsVectorSimplifyMethod simplifyMethod;
  simplifyMethod.setSimplifyHints( Qgis::VectorRenderingSimplificationFlags() );

  mpPolysLayer->setSimplifyMethod( simplifyMethod );

  //create a line layer that will be used in tests
  const QString myLinesFileName = mTestDataDir + "lines.shp";
  const QFileInfo myLineFileInfo( myLinesFileName );
  mpLinesLayer = new QgsVectorLayer( myLineFileInfo.filePath(), myLineFileInfo.completeBaseName(), u"ogr"_s );
  mpLinesLayer->setSimplifyMethod( simplifyMethod );

  //create two raster layers
  const QFileInfo rasterFileInfo( mTestDataDir + "rgb256x256.png" );
  mRasterLayer1 = new QgsRasterLayer( rasterFileInfo.filePath(), rasterFileInfo.completeBaseName() );
  mRasterLayer2 = new QgsRasterLayer( rasterFileInfo.filePath(), rasterFileInfo.completeBaseName() );
  QgsMultiBandColorRenderer *rasterRenderer = new QgsMultiBandColorRenderer( mRasterLayer1->dataProvider(), 1, 2, 3 );
  mRasterLayer1->setRenderer( rasterRenderer );
  mRasterLayer2->setRenderer( ( QgsRasterRenderer * ) rasterRenderer->clone() );

  // points extent was not always reliable
  mExtent = QgsRectangle( -118.8888888888887720, 22.8002070393376783, -83.3333333333331581, 46.8719806763287536 );
}
void TestQgsBlendModes::cleanupTestCase()
{
  delete mpPointsLayer;
  delete mpPolysLayer;
  delete mpLinesLayer;
  delete mRasterLayer1;
  delete mRasterLayer2;

  QgsApplication::exitQgis();
}

void TestQgsBlendModes::vectorBlending()
{
  //Add two vector layers
  QList<QgsMapLayer *> myLayers;
  myLayers << mpLinesLayer;
  myLayers << mpPolysLayer;
  mMapSettings->setLayers( myLayers );

  //Set blending modes for both layers
  mpLinesLayer->setBlendMode( QPainter::CompositionMode_Difference );
  mpPolysLayer->setBlendMode( QPainter::CompositionMode_Difference );
  mMapSettings->setExtent( mExtent );
  const bool res = QGSRENDERMAPSETTINGSCHECK( u"vector_blendmodes"_s, u"vector_blendmodes"_s, *mMapSettings, 20, 5 );

  //Reset layers
  mpLinesLayer->setBlendMode( QPainter::CompositionMode_SourceOver );
  mpPolysLayer->setBlendMode( QPainter::CompositionMode_SourceOver );

  QVERIFY( res );
}

void TestQgsBlendModes::featureBlending()
{
  //Add two vector layers
  QList<QgsMapLayer *> myLayers;
  myLayers << mpLinesLayer;
  myLayers << mpPolysLayer;
  mMapSettings->setLayers( myLayers );

  //Set feature blending modes for point layer
  mpLinesLayer->setFeatureBlendMode( QPainter::CompositionMode_Plus );
  mMapSettings->setExtent( mExtent );
  const bool res = QGSRENDERMAPSETTINGSCHECK( u"vector_featureblendmodes"_s, u"vector_featureblendmodes"_s, *mMapSettings, 20, 5 );

  //Reset layers
  mpLinesLayer->setFeatureBlendMode( QPainter::CompositionMode_SourceOver );

  QVERIFY( res );
}

void TestQgsBlendModes::vectorLayerTransparency()
{
  //Add two vector layers
  QList<QgsMapLayer *> myLayers;
  myLayers << mpLinesLayer;
  myLayers << mpPolysLayer;
  mMapSettings->setLayers( myLayers );

  //Set feature blending modes for point layer
  mpLinesLayer->setOpacity( 0.50 );
  mMapSettings->setExtent( mExtent );
  const bool res = QGSRENDERMAPSETTINGSCHECK( u"vector_layertransparency"_s, u"vector_layertransparency"_s, *mMapSettings, 20, 5 );

  //Reset layers
  mpLinesLayer->setOpacity( 1.0 );

  QVERIFY( res );
}

void TestQgsBlendModes::rasterBlending()
{
  //Add two raster layers to map renderer
  QList<QgsMapLayer *> myLayers;
  myLayers << mRasterLayer1;
  myLayers << mRasterLayer2;
  mMapSettings->setLayers( myLayers );
  mMapSettings->setExtent( mRasterLayer1->extent() );

  // set blending mode for top layer
  mRasterLayer1->setBlendMode( QPainter::CompositionMode_Difference );
  QGSVERIFYRENDERMAPSETTINGSCHECK( u"raster_blendmodes"_s, u"raster_blendmodes"_s, *mMapSettings, 20, 5 );
}

void TestQgsBlendModes::rasterBlendingWithTransparency()
{
  // Test for issue #55628: Blending-Modes Render No-Data Transparent Pixel Values
  // This test verifies that raster layers with transparent pixels render correctly
  // when using non-Normal blend modes. The bug was that transparent pixels would
  // incorrectly affect the destination with non-SourceOver blend modes due to
  // Qt bug QTBUG-66590.

  // Use existing test raster with mask that has transparent areas
  const QFileInfo rasterFileInfo( mTestDataDir + "raster/rgb_with_mask.tif" );
  auto rasterLayer = std::make_unique<QgsRasterLayer>( rasterFileInfo.filePath(), u"masked_raster"_s );
  QVERIFY( rasterLayer->isValid() );

  // Set up map settings with a colored background
  QgsMapSettings settings;
  settings.setLayers( { rasterLayer.get() } );
  settings.setExtent( rasterLayer->extent() );
  settings.setOutputSize( QSize( 162, 150 ) );
  settings.setOutputDpi( 96 );
  const QColor backgroundColor( 255, 0, 0 ); // Red background
  settings.setBackgroundColor( backgroundColor );

  // First render with Normal mode to establish baseline
  rasterLayer->setBlendMode( QPainter::CompositionMode_SourceOver );
  QgsMapRendererSequentialJob normalJob( settings );
  normalJob.start();
  normalJob.waitForFinished();
  const QImage normalImage = normalJob.renderedImage();
  QVERIFY( !normalImage.isNull() );

  // Find a corner pixel in the transparent region
  // The test image has transparency around the edges
  const QColor normalCorner = normalImage.pixelColor( 5, 5 );

  // Verify corner shows background in Normal mode
  QVERIFY2( normalCorner.red() >= 250 && normalCorner.green() <= 5 && normalCorner.blue() <= 5, qPrintable( u"Corner should show red background in Normal mode, got RGB(%1,%2,%3)"_s.arg( normalCorner.red() ).arg( normalCorner.green() ).arg( normalCorner.blue() ) ) );

  // Test non-SourceOver blend modes - the fix ensures transparent pixels
  // don't affect the destination, so corners should still show background
  const QList<QPainter::CompositionMode> blendModes = {
    QPainter::CompositionMode_Multiply,
    QPainter::CompositionMode_Screen,
    QPainter::CompositionMode_Overlay,
    QPainter::CompositionMode_Difference
  };

  for ( QPainter::CompositionMode mode : blendModes )
  {
    rasterLayer->setBlendMode( mode );

    QgsMapRendererSequentialJob job( settings );
    job.start();
    job.waitForFinished();
    const QImage img = job.renderedImage();

    QVERIFY( !img.isNull() );
    QCOMPARE( img.size(), QSize( 162, 150 ) );

    // KEY TEST: Transparent region should still show background color
    // This is the fix for #55628 - without the fix, transparent pixels would
    // incorrectly blend with the destination and produce wrong colors
    const QColor corner = img.pixelColor( 5, 5 );
    QVERIFY2( corner.red() >= 250 && corner.green() <= 5 && corner.blue() <= 5, qPrintable( u"Corner should show red background for blend mode %1, got RGB(%2,%3,%4)"_s.arg( static_cast<int>( mode ) ).arg( corner.red() ).arg( corner.green() ).arg( corner.blue() ) ) );

    // Also verify center (opaque region) still renders properly
    const QColor center = img.pixelColor( 80, 75 );
    QVERIFY2( center.alpha() > 0 && ( center.red() > 0 || center.green() > 0 || center.blue() > 0 ), qPrintable( u"Center pixel should be rendered for blend mode %1"_s.arg( static_cast<int>( mode ) ) ) );
  }

  rasterLayer->setBlendMode( QPainter::CompositionMode_SourceOver );
}

QGSTEST_MAIN( TestQgsBlendModes )
#include "testqgsblendmodes.moc"
