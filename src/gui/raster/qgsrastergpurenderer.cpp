/***************************************************************************
  qgsrastergpurenderer.cpp
  ------------------------
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

#include "qgsrastergpurenderer.h"

#include <QString>

#ifdef HAVE_QRHI

#include <cmath>
#include <gdal.h>

#include "qgscoordinatetransform.h"
#include "qgsfeedback.h"
#include "qgslogger.h"
#include "qgsmaptopixel.h"
#include "qgsrastergputileuploader.h"
#include "qgsrasterviewport.h"
#include "qgsrectangle.h"
#include "qgsrendercontext.h"

#include <QFile>
#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QString>

using namespace Qt::StringLiterals;

// Uniform buffer layout (must match shader std140 layout)
// See raster_colormap.frag and raster_rgb.frag
//
// std140 alignment rules:
// - float: 4 bytes
// - vec2: 8 bytes
// - vec3/vec4: 16 bytes
// - mat4: 64 bytes (4x vec4)
//
// Contrast enhancement algorithm values:
// 0 = NoEnhancement
// 1 = StretchToMinimumMaximum (linear stretch, saturate at bounds)
// 2 = ClipToMinimumMaximum (discard pixels outside range)
// 3 = StretchAndClipToMinimumMaximum (clip then stretch)
//
// Brightness/Contrast/Gamma formulas (match QgsBrightnessContrastFilter):
// - contrastFactor = pow((contrast+100)/100, 2)
// - gammaCorrection = 1/gamma
//
// Hue/Saturation formulas (match QgsHueSaturationFilter):
// - saturationScale = (saturation/100)+1, range 0-2
// - Luminosity weights: 0.21R + 0.72G + 0.07B
// - Saturation boost: 1 - pow(1-s, scale^2)
struct UniformBlock
{
    float mvpMatrix[16]; // mat4 uMVPMatrix (64 bytes, offset 0)
    float scale;         // uScale (4 bytes, offset 64)
    float opacity;       // uOpacity (4 bytes, offset 68)
    float useNoData;     // uUseNoData (4 bytes, offset 72)
    float algorithm;     // Contrast enhancement algorithm (4 bytes, offset 76)
    // Per-band min/max for contrast enhancement
    float minR; // Red/Gray min (4 bytes, offset 80)
    float maxR; // Red/Gray max (4 bytes, offset 84)
    float minG; // Green min (4 bytes, offset 88)
    float maxG; // Green max (4 bytes, offset 92)
    float minB; // Blue min (4 bytes, offset 96)
    float maxB; // Blue max (4 bytes, offset 100)
    // Per-band NoData values
    float noDataR;         // Red/Gray NoData (4 bytes, offset 104)
    float noDataG;         // Green NoData (4 bytes, offset 108)
    float noDataB;         // Blue NoData (4 bytes, offset 112)
    float noDataTolerance; // NoData comparison tolerance (4 bytes, offset 116)
    float _pad1[2];        // Alignment (8 bytes, offset 120)
                           // Subtotal: 128 bytes

    // Brightness/Contrast/Gamma filter (offset 128)
    float brightness;      // -255 to 255 (4 bytes, offset 128)
    float contrastFactor;  // pow((contrast+100)/100, 2) (4 bytes, offset 132)
    float gammaCorrection; // 1/gamma (4 bytes, offset 136)
    float _pad2;           // Alignment (4 bytes, offset 140)

    // Hue/Saturation filter (offset 144)
    float invertColors;     // 0 or 1 (4 bytes, offset 144)
    float grayscaleMode;    // 0=off, 1=lightness, 2=luminosity, 3=average (4 bytes, offset 148)
    float saturationScale;  // (saturation/100)+1, range 0-2 (4 bytes, offset 152)
    float colorizeOn;       // 0 or 1 (4 bytes, offset 156)
    float colorizeH;        // 0-1 hue (4 bytes, offset 160)
    float colorizeS;        // 0-1 saturation (4 bytes, offset 164)
    float colorizeStrength; // 0-1 strength (4 bytes, offset 168)
    float _pad3;            // Alignment (4 bytes, offset 172)

    // Filter enable flags (offset 176)
    float useBrightnessFilter; // 1 if filter active (4 bytes, offset 176)
    float useHueSatFilter;     // 1 if filter active (4 bytes, offset 180)

    // Hillshade parameters (offset 184)
    float hillshadeMode;     // 1 if hillshade rendering (4 bytes, offset 184)
    float hillshadeAzimuth;  // Sun azimuth in radians (4 bytes, offset 188)
    float hillshadeAltitude; // Sun altitude in radians (4 bytes, offset 192)
    float hillshadeZFactor;  // Z exaggeration (4 bytes, offset 196)
    float hillshadeMultiDir; // Multi-directional flag (4 bytes, offset 200)
    // Pre-computed trig values for efficiency
    float cosAzCosAlt; // cos(az) * cos(alt) (4 bytes, offset 204)
    float sinAzCosAlt; // sin(az) * cos(alt) (4 bytes, offset 208)
    float sinAlt;      // sin(alt) (4 bytes, offset 212)
    float cellSizeX;   // Cell width for derivative (4 bytes, offset 216)
    float cellSizeY;   // Cell height for derivative (4 bytes, offset 220)

    float _reserved[8]; // Pad to 256 bytes (32 bytes, offset 224)
                        // Total: 256 bytes
};

void QgsRasterGPURenderer::setRGBBands( int redBand, int greenBand, int blueBand )
{
  mRGBMode = true;
  mRedBand = redBand;
  mGreenBand = greenBand;
  mBlueBand = blueBand;

  // Force pipeline recreation on next render
  mPipelineNeedsRebuild = true;
}

void QgsRasterGPURenderer::setContrastEnhancement( double minValue, double maxValue, bool invertGradient )
{
  mMinValue = minValue;
  mMaxValue = maxValue;
  mInvertGradient = invertGradient;
  mHasContrastEnhancement = true;

  // Colormap needs rebuild to apply gradient inversion
  mColormapUploaded = false;
}

void QgsRasterGPURenderer::setRGBContrastEnhancement( double redMin, double redMax, double greenMin, double greenMax, double blueMin, double blueMax )
{
  mRedMin = redMin;
  mRedMax = redMax;
  mGreenMin = greenMin;
  mGreenMax = greenMax;
  mBlueMin = blueMin;
  mBlueMax = blueMax;
  mHasRGBContrastEnhancement = true;
}

void QgsRasterGPURenderer::setRGBNoData( double redNoData, double greenNoData, double blueNoData )
{
  mNoDataR = redNoData;
  mNoDataG = greenNoData;
  mNoDataB = blueNoData;
  mHasPerBandNoData = true;
}

void QgsRasterGPURenderer::setBrightnessContrastGamma( int brightness, int contrast, double gamma )
{
  mBrightness = std::clamp( brightness, -255, 255 );
  mContrast = std::clamp( contrast, -100, 100 );
  mGamma = std::clamp( gamma, 0.1, 10.0 );
  mHasBrightnessFilter = true;
}

void QgsRasterGPURenderer::setHueSaturationFilter( bool invert, int grayscaleMode, int saturation, bool colorizeOn, const QColor &colorizeColor, int colorizeStrength )
{
  mInvertColors = invert;
  mGrayscaleMode = std::clamp( grayscaleMode, 0, 3 );
  mSaturation = std::clamp( saturation, -100, 100 );
  mColorizeOn = colorizeOn;
  mColorizeColor = colorizeColor;
  mColorizeStrength = std::clamp( colorizeStrength, 0, 100 );
  mHasHueSatFilter = true;
}

void QgsRasterGPURenderer::setPseudocolorColormap( const QByteArray &colormapData, double minValue, double maxValue, int interpolationType )
{
  if ( colormapData.size() != 256 * 4 )
  {
    QgsDebugError( u"Pseudocolor colormap must be 1024 bytes (256 RGBA entries), got %1"_s.arg( colormapData.size() ) );
    return;
  }
  mPseudocolorData = colormapData;
  mPseudocolorMin = minValue;
  mPseudocolorMax = maxValue;
  mPseudocolorInterpolationType = interpolationType;
  mHasPseudocolorColormap = true;
  mColormapUploaded = false; // Force colormap re-upload with new data

  // Also set contrast enhancement to use pseudocolor min/max
  mMinValue = minValue;
  mMaxValue = maxValue;
  mHasContrastEnhancement = true;
}

void QgsRasterGPURenderer::setHillshadeParams( double azimuth, double altitude, double zFactor, bool multiDirectional )
{
  mHillshadeAzimuth = azimuth;
  mHillshadeAltitude = std::clamp( altitude, 0.0, 90.0 );
  mHillshadeZFactor = zFactor;
  mHillshadeMultiDirectional = multiDirectional;
  mHillshadeMode = true;

  // Hillshade doesn't use colormap texture - generate grayscale output directly
  mColormapUploaded = false;
}

QgsRasterGPURenderer::QgsRasterGPURenderer( QgsRasterGPUTileUploader *tileUploader, QRhi *rhi )
  : mTileUploader( tileUploader )
  , mRhi( rhi )
{
}

QgsRasterGPURenderer::~QgsRasterGPURenderer()
{
  releaseRhiResources();
}

void QgsRasterGPURenderer::releaseRhiResources()
{
  // Release in reverse order of creation
  mPipeline.reset();
  mSrbLayout.reset();
  mPlaceholderTexture.reset(); // Must be after mSrbLayout (SRB references it)
  mUniformBuffer.reset();
  mVertexBuffer.reset();
  mSampler.reset();
  mColormapTexture.reset();
  mRenderPassDesc.reset();
  mRenderTarget.reset();
  mRenderTexture.reset();
}

QRhiShaderResourceBindings *QgsRasterGPURenderer::createTileSrb( QRhiTexture *tileTexture )
{
  if ( !mRhi || !mUniformBuffer || !mSampler || !tileTexture )
    return nullptr;

  // For single-band mode, colormap texture is required
  if ( !mRGBMode && !mColormapTexture )
    return nullptr;

  QRhiShaderResourceBindings *srb = mRhi->newShaderResourceBindings();

  QVector<QRhiShaderResourceBinding> bindings;

  // Binding 0: Uniform buffer (vertex + fragment stages)
  bindings.append( QRhiShaderResourceBinding::uniformBuffer(
    0,
    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
    mUniformBuffer.get()
  ) );

  // Binding 1: Tile texture (fragment stage)
  bindings.append( QRhiShaderResourceBinding::sampledTexture(
    1,
    QRhiShaderResourceBinding::FragmentStage,
    tileTexture,
    mSampler.get()
  ) );

  // Binding 2: Colormap texture (fragment stage, single-band mode only)
  if ( !mRGBMode )
  {
    bindings.append( QRhiShaderResourceBinding::sampledTexture(
      2,
      QRhiShaderResourceBinding::FragmentStage,
      mColormapTexture.get(),
      mSampler.get()
    ) );
  }

  srb->setBindings( bindings.begin(), bindings.end() );
  if ( !srb->create() )
  {
    delete srb;
    return nullptr;
  }

  return srb;
}

bool QgsRasterGPURenderer::ensureRenderTarget( int width, int height )
{
  // Check if existing render target matches size
  if ( mRenderTexture && mRenderTexture->pixelSize() == QSize( width, height ) )
    return true;

  // Release existing render target
  mRenderPassDesc.reset();
  mRenderTarget.reset();
  mRenderTexture.reset();

  // Create render texture
  mRenderTexture.reset( mRhi->newTexture(
    QRhiTexture::RGBA8,
    QSize( width, height ),
    1,
    QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource
  ) );

  if ( !mRenderTexture->create() )
  {
    QgsDebugError( u"Failed to create render texture"_s );
    mRenderTexture.reset();
    return false;
  }

  // Create render target
  QRhiTextureRenderTargetDescription rtDesc( mRenderTexture.get() );
  mRenderTarget.reset( mRhi->newTextureRenderTarget( rtDesc ) );

  mRenderPassDesc.reset( mRenderTarget->newCompatibleRenderPassDescriptor() );
  mRenderTarget->setRenderPassDescriptor( mRenderPassDesc.get() );

  if ( !mRenderTarget->create() )
  {
    QgsDebugError( u"Failed to create render target"_s );
    mRenderTarget.reset();
    mRenderPassDesc.reset();
    mRenderTexture.reset();
    return false;
  }

  return true;
}

bool QgsRasterGPURenderer::initializeRhiResources( int width, int height )
{
  if ( !mRhi )
    return false;

  // Ensure render target exists
  if ( !ensureRenderTarget( width, height ) )
    return false;

  // Only rebuild pipeline if needed
  if ( !mPipelineNeedsRebuild && mPipeline )
    return true;

  // Release old pipeline resources
  mPipeline.reset();
  mSrbLayout.reset();
  mUniformBuffer.reset();
  mVertexBuffer.reset();
  mSampler.reset();
  mColormapTexture.reset();
  mColormapUploaded = false;

  // Load vertex shader
  QShader vertexShader;
  {
    QFile f( u":/shaders/raster_tile.vert.qsb"_s );
    if ( !f.open( QIODevice::ReadOnly ) )
    {
      QgsDebugError( u"Failed to open vertex shader: %1"_s.arg( f.errorString() ) );
      return false;
    }
    vertexShader = QShader::fromSerialized( f.readAll() );
    if ( !vertexShader.isValid() )
    {
      QgsDebugError( u"Invalid vertex shader"_s );
      return false;
    }
  }

  // Load fragment shader based on mode
  QShader fragmentShader;
  {
    const QString fragPath = mRGBMode
                               ? u":/shaders/raster_rgb.frag.qsb"_s
                               : u":/shaders/raster_colormap.frag.qsb"_s;
    QFile f( fragPath );
    if ( !f.open( QIODevice::ReadOnly ) )
    {
      QgsDebugError( u"Failed to open fragment shader: %1"_s.arg( f.errorString() ) );
      return false;
    }
    fragmentShader = QShader::fromSerialized( f.readAll() );
    if ( !fragmentShader.isValid() )
    {
      QgsDebugError( u"Invalid fragment shader"_s );
      return false;
    }
  }

  // Create uniform buffer
  mUniformBuffer.reset( mRhi->newBuffer(
    QRhiBuffer::Dynamic,
    QRhiBuffer::UniformBuffer,
    sizeof( UniformBlock )
  ) );
  if ( !mUniformBuffer->create() )
  {
    QgsDebugError( u"Failed to create uniform buffer"_s );
    return false;
  }

  // Create sampler for tile textures
  mSampler.reset( mRhi->newSampler(
    QRhiSampler::Linear,
    QRhiSampler::Linear,
    QRhiSampler::None,
    QRhiSampler::ClampToEdge,
    QRhiSampler::ClampToEdge
  ) );
  if ( !mSampler->create() )
  {
    QgsDebugError( u"Failed to create sampler"_s );
    return false;
  }

  // Create colormap texture for single-band mode
  if ( !mRGBMode )
  {
    // Create 256x1 grayscale colormap (black to white)
    QByteArray colormapData( 256 * 4, 0 );
    for ( int i = 0; i < 256; ++i )
    {
      colormapData[i * 4 + 0] = static_cast<char>( i );   // R
      colormapData[i * 4 + 1] = static_cast<char>( i );   // G
      colormapData[i * 4 + 2] = static_cast<char>( i );   // B
      colormapData[i * 4 + 3] = static_cast<char>( 255 ); // A
    }

    mColormapTexture.reset( mRhi->newTexture(
      QRhiTexture::RGBA8,
      QSize( 256, 1 ),
      1,
      QRhiTexture::Flag()
    ) );
    if ( !mColormapTexture->create() )
    {
      QgsDebugError( u"Failed to create colormap texture"_s );
      return false;
    }

    // Upload colormap data in the next resource batch
  }

  // Create vertex buffer (will be filled per-frame)
  // Reserve space for up to 256 tiles (6 vertices * 4 floats * 256 tiles)
  constexpr int MAX_TILES = 256;
  constexpr int FLOATS_PER_VERTEX = 4;
  constexpr int VERTICES_PER_TILE = 6;
  mVertexBuffer.reset( mRhi->newBuffer(
    QRhiBuffer::Dynamic,
    QRhiBuffer::VertexBuffer,
    MAX_TILES * VERTICES_PER_TILE * FLOATS_PER_VERTEX * sizeof( float )
  ) );
  if ( !mVertexBuffer->create() )
  {
    QgsDebugError( u"Failed to create vertex buffer"_s );
    return false;
  }

  // Create shader resource bindings layout
  // Binding 0: Uniform buffer
  // Binding 1: Tile texture + sampler (per-tile SRBs created in render())
  // Binding 2: Colormap texture (single-band mode only)
  mSrbLayout.reset( mRhi->newShaderResourceBindings() );

  // Create placeholder texture for SRB layout (must outlive mSrbLayout)
  // Actual tile textures are bound via per-tile SRBs in render()
  mPlaceholderTexture.reset( mRhi->newTexture( QRhiTexture::RGBA8, QSize( 1, 1 ) ) );
  if ( !mPlaceholderTexture->create() )
  {
    QgsDebugError( u"Failed to create placeholder texture"_s );
    return false;
  }

  QVector<QRhiShaderResourceBinding> layoutBindings;
  layoutBindings.append( QRhiShaderResourceBinding::uniformBuffer(
    0,
    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
    mUniformBuffer.get()
  ) );
  layoutBindings.append( QRhiShaderResourceBinding::sampledTexture(
    1,
    QRhiShaderResourceBinding::FragmentStage,
    mPlaceholderTexture.get(),
    mSampler.get()
  ) );

  // For single-band mode, add colormap texture binding
  if ( !mRGBMode )
  {
    layoutBindings.append( QRhiShaderResourceBinding::sampledTexture(
      2,
      QRhiShaderResourceBinding::FragmentStage,
      mPlaceholderTexture.get(), // placeholder, actual colormap bound in per-tile SRBs
      mSampler.get()
    ) );
  }

  mSrbLayout->setBindings( layoutBindings.begin(), layoutBindings.end() );
  if ( !mSrbLayout->create() )
  {
    QgsDebugError( u"Failed to create shader resource bindings layout"_s );
    return false;
  }

  // Create graphics pipeline
  mPipeline.reset( mRhi->newGraphicsPipeline() );

  mPipeline->setShaderStages( { { QRhiShaderStage::Vertex, vertexShader }, { QRhiShaderStage::Fragment, fragmentShader } } );

  // Vertex input layout: position (vec2) + texcoord (vec2)
  QRhiVertexInputLayout inputLayout;
  inputLayout.setBindings( {
    { 4 * sizeof( float ) } // stride
  } );
  inputLayout.setAttributes( {
    { 0, 0, QRhiVertexInputAttribute::Float2, 0 },                  // position
    { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof( float ) } // texcoord
  } );
  mPipeline->setVertexInputLayout( inputLayout );

  mPipeline->setShaderResourceBindings( mSrbLayout.get() );
  mPipeline->setRenderPassDescriptor( mRenderPassDesc.get() );

  // Enable alpha blending
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  mPipeline->setTargetBlends( { blend } );

  mPipeline->setTopology( QRhiGraphicsPipeline::Triangles );
  mPipeline->setCullMode( QRhiGraphicsPipeline::None );
  mPipeline->setDepthTest( false );
  mPipeline->setDepthWrite( false );

  if ( !mPipeline->create() )
  {
    QgsDebugError( u"Failed to create graphics pipeline"_s );
    return false;
  }

  mPipelineNeedsRebuild = false;
  return true;
}

bool QgsRasterGPURenderer::render( QgsRenderContext &renderContext, QgsRasterViewPort *rasterViewPort, QgsFeedback *feedback )
{
  if ( !mTileUploader || !rasterViewPort || !mRhi )
  {
    QgsDebugError( u"Invalid tile uploader, viewport, or QRhi"_s );
    return false;
  }

  // Get output size from viewport
  const int outputWidth = rasterViewPort->mWidth;
  const int outputHeight = rasterViewPort->mHeight;

  if ( outputWidth <= 0 || outputHeight <= 0 )
  {
    QgsDebugError( u"Invalid viewport size: %1x%2"_s.arg( outputWidth ).arg( outputHeight ) );
    return false;
  }

  // Initialize QRhi resources if needed
  if ( !initializeRhiResources( outputWidth, outputHeight ) )
  {
    QgsDebugError( u"Failed to initialize QRhi resources"_s );
    return false;
  }

  // Select best overview level for current scale
  const int overviewLevel = selectOverviewLevel( rasterViewPort );

  // Get coordinate transform (map → raster)
  QgsCoordinateTransform transform;
  if ( rasterViewPort->mSrcCRS.isValid() && rasterViewPort->mDestCRS.isValid() )
  {
    transform = QgsCoordinateTransform( rasterViewPort->mDestCRS, rasterViewPort->mSrcCRS, renderContext.transformContext() );
  }

  // Calculate visible tiles
  const QVector<TileCoord> visibleTiles = calculateVisibleTiles( rasterViewPort, overviewLevel, transform );

  if ( visibleTiles.isEmpty() )
  {
    QgsDebugMsgLevel( u"No visible tiles to render"_s, 3 );
    return true;
  }

  // Pre-fetch all GPU tiles
  struct TileRenderData
  {
      QRhiTexture *texture;
      QgsRectangle extent;
  };
  QVector<TileRenderData> tilesToRender;
  tilesToRender.reserve( visibleTiles.size() );

  if ( mRGBMode )
  {
    // RGB mode: fetch tiles individually
    for ( const TileCoord &tileCoord : visibleTiles )
    {
      if ( feedback && feedback->isCanceled() )
        break;

      auto gpuTile = mTileUploader->getRGBTile( tileCoord.level, tileCoord.x, tileCoord.y, mRedBand, mGreenBand, mBlueBand, mFrameNumber );
      if ( !gpuTile.isValid || !gpuTile.texture )
        continue;

      const QgsRectangle tileExtent = mTileUploader->tileExtent( tileCoord.level, tileCoord.x, tileCoord.y );
      if ( !tileExtent.isEmpty() )
        tilesToRender.append( { gpuTile.texture, tileExtent } );
    }
  }
  else
  {
    // Single-band mode: use batch lookup
    QVector<QgsRasterGPUTileUploader::TileCoord> coords;
    coords.reserve( visibleTiles.size() );
    for ( const TileCoord &tc : visibleTiles )
    {
      coords.append( { tc.level, tc.x, tc.y } );
    }

    const auto gpuTiles = mTileUploader->getTiles( coords, mInputBand, mFrameNumber );

    for ( int i = 0; i < gpuTiles.size(); ++i )
    {
      if ( feedback && feedback->isCanceled() )
        break;

      if ( !gpuTiles[i].isValid || !gpuTiles[i].texture )
        continue;

      const QgsRectangle tileExtent = mTileUploader->tileExtent( visibleTiles[i].level, visibleTiles[i].x, visibleTiles[i].y );
      if ( !tileExtent.isEmpty() )
        tilesToRender.append( { gpuTiles[i].texture, tileExtent } );
    }
  }

  if ( tilesToRender.isEmpty() )
  {
    return false;
  }

  // Validate tile count against vertex buffer capacity
  constexpr int MAX_TILES = 256;
  if ( tilesToRender.size() > MAX_TILES )
  {
    QgsDebugError( u"Too many tiles: %1 > %2, falling back"_s.arg( tilesToRender.size() ).arg( MAX_TILES ) );
    return false;
  }

  // Build vertex data for all tiles
  constexpr int FLOATS_PER_VERTEX = 4;
  constexpr int VERTICES_PER_TILE = 6;
  QVector<float> vertexData;
  vertexData.reserve( tilesToRender.size() * VERTICES_PER_TILE * FLOATS_PER_VERTEX );

  for ( const TileRenderData &tile : tilesToRender )
  {
    const float x0 = static_cast<float>( tile.extent.xMinimum() );
    const float y0 = static_cast<float>( tile.extent.yMinimum() );
    const float x1 = static_cast<float>( tile.extent.xMaximum() );
    const float y1 = static_cast<float>( tile.extent.yMaximum() );

    // Triangle 1
    vertexData << x0 << y0 << 0.0f << 1.0f;
    vertexData << x1 << y0 << 1.0f << 1.0f;
    vertexData << x1 << y1 << 1.0f << 0.0f;
    // Triangle 2
    vertexData << x0 << y0 << 0.0f << 1.0f;
    vertexData << x1 << y1 << 1.0f << 0.0f;
    vertexData << x0 << y1 << 0.0f << 0.0f;
  }

  // Build MVP matrix
  QMatrix4x4 mvpMatrix;
  const QgsRectangle &viewExtent = renderContext.extent();
  const float scaleX = 2.0f / static_cast<float>( viewExtent.width() );
  const float scaleY = 2.0f / static_cast<float>( viewExtent.height() );
  const float translateX = -static_cast<float>( viewExtent.xMinimum() + viewExtent.xMaximum() ) / static_cast<float>( viewExtent.width() );
  const float translateY = -static_cast<float>( viewExtent.yMinimum() + viewExtent.yMaximum() ) / static_cast<float>( viewExtent.height() );
  mvpMatrix.scale( scaleX, scaleY );
  mvpMatrix.translate( translateX, translateY );

  // Prepare uniform data (aggregate init zeros all fields)
  UniformBlock uniforms {};
  memcpy( uniforms.mvpMatrix, mvpMatrix.constData(), sizeof( uniforms.mvpMatrix ) );

  // Get data type info from tile uploader
  const auto tileInfo = mTileUploader->tileInfo( overviewLevel );

  // Derive scale from data type (max value for integer types, 1 for floats)
  float dataTypeScale = 255.0f;
  float dataTypeMin = 0.0f;
  float dataTypeMax = 255.0f;

  switch ( tileInfo.dataType )
  {
    case GDT_Byte:
      dataTypeScale = 255.0f;
      dataTypeMin = 0.0f;
      dataTypeMax = 255.0f;
      break;
    case GDT_UInt16:
      dataTypeScale = 65535.0f;
      dataTypeMin = 0.0f;
      dataTypeMax = 65535.0f;
      break;
    case GDT_Int16:
      dataTypeScale = 32767.0f;
      dataTypeMin = -32768.0f;
      dataTypeMax = 32767.0f;
      break;
    case GDT_Float32:
    case GDT_Float64:
      dataTypeScale = 1.0f;
      dataTypeMin = 0.0f;
      dataTypeMax = 1.0f;
      break;
    default:
      break;
  }

  uniforms.scale = dataTypeScale;
  uniforms.opacity = mOpacity;
  uniforms.useNoData = tileInfo.hasNoData ? 1.0f : 0.0f;
  uniforms.algorithm = static_cast<float>( mAlgorithm );

  // Set per-band contrast enhancement values
  if ( mHasRGBContrastEnhancement && mRGBMode )
  {
    // RGB mode: use per-band contrast enhancement from QgsMultiBandColorRenderer
    uniforms.minR = static_cast<float>( mRedMin );
    uniforms.maxR = static_cast<float>( mRedMax );
    uniforms.minG = static_cast<float>( mGreenMin );
    uniforms.maxG = static_cast<float>( mGreenMax );
    uniforms.minB = static_cast<float>( mBlueMin );
    uniforms.maxB = static_cast<float>( mBlueMax );
  }
  else if ( mHasContrastEnhancement && !mRGBMode )
  {
    // Single-band mode: use same value for all channels (shader uses R channel)
    uniforms.minR = static_cast<float>( mMinValue );
    uniforms.maxR = static_cast<float>( mMaxValue );
    uniforms.minG = uniforms.minR;
    uniforms.maxG = uniforms.maxR;
    uniforms.minB = uniforms.minR;
    uniforms.maxB = uniforms.maxR;
  }
  else
  {
    // No contrast enhancement, use data type range
    uniforms.minR = dataTypeMin;
    uniforms.maxR = dataTypeMax;
    uniforms.minG = dataTypeMin;
    uniforms.maxG = dataTypeMax;
    uniforms.minB = dataTypeMin;
    uniforms.maxB = dataTypeMax;
  }

  // Set per-band NoData values
  if ( mHasPerBandNoData && mRGBMode )
  {
    uniforms.noDataR = static_cast<float>( mNoDataR );
    uniforms.noDataG = static_cast<float>( mNoDataG );
    uniforms.noDataB = static_cast<float>( mNoDataB );
  }
  else
  {
    // Single value for all bands (from raster metadata)
    const float noDataVal = static_cast<float>( tileInfo.noDataValue );
    uniforms.noDataR = noDataVal;
    uniforms.noDataG = noDataVal;
    uniforms.noDataB = noDataVal;
  }

  // NoData tolerance: use relative epsilon for float data, absolute for integer
  if ( tileInfo.dataType == GDT_Float32 || tileInfo.dataType == GDT_Float64 )
  {
    // For floats, use a small relative tolerance or user-provided value
    uniforms.noDataTolerance = static_cast<float>( mNoDataTolerance > 0.5 ? mNoDataTolerance : 0.0001 );
  }
  else
  {
    uniforms.noDataTolerance = static_cast<float>( mNoDataTolerance );
  }

  // Brightness/Contrast/Gamma filter
  // Formula: contrastFactor = pow((contrast+100)/100, 2), gammaCorrection = 1/gamma
  if ( mHasBrightnessFilter )
  {
    uniforms.brightness = static_cast<float>( mBrightness );
    uniforms.contrastFactor = static_cast<float>( std::pow( ( mContrast + 100 ) / 100.0, 2 ) );
    uniforms.gammaCorrection = static_cast<float>( 1.0 / mGamma );
    uniforms.useBrightnessFilter = 1.0f;
  }
  else
  {
    uniforms.brightness = 0.0f;
    uniforms.contrastFactor = 1.0f;
    uniforms.gammaCorrection = 1.0f;
    uniforms.useBrightnessFilter = 0.0f;
  }

  // Hue/Saturation filter
  // Order: Invert → Grayscale/Saturation → Colorize
  if ( mHasHueSatFilter )
  {
    uniforms.invertColors = mInvertColors ? 1.0f : 0.0f;
    uniforms.grayscaleMode = static_cast<float>( mGrayscaleMode );
    uniforms.saturationScale = static_cast<float>( ( mSaturation / 100.0 ) + 1.0 );
    uniforms.colorizeOn = mColorizeOn ? 1.0f : 0.0f;
    uniforms.colorizeH = static_cast<float>( mColorizeColor.hueF() );
    uniforms.colorizeS = static_cast<float>( mColorizeColor.saturationF() );
    uniforms.colorizeStrength = static_cast<float>( mColorizeStrength / 100.0 );
    uniforms.useHueSatFilter = 1.0f;
  }
  else
  {
    uniforms.invertColors = 0.0f;
    uniforms.grayscaleMode = 0.0f;
    uniforms.saturationScale = 1.0f;
    uniforms.colorizeOn = 0.0f;
    uniforms.colorizeH = 0.0f;
    uniforms.colorizeS = 0.0f;
    uniforms.colorizeStrength = 0.0f;
    uniforms.useHueSatFilter = 0.0f;
  }
  // Note: Hillshade and reserved fields are already zeroed by aggregate init

  // Begin offscreen frame (enables synchronous readback)
  QRhiCommandBuffer *cb = nullptr;
  if ( mRhi->beginOffscreenFrame( &cb ) != QRhi::FrameOpSuccess )
  {
    QgsDebugError( u"Failed to begin offscreen frame"_s );
    return false;
  }

  // Create resource update batch for uploads
  QRhiResourceUpdateBatch *uploadBatch = mRhi->nextResourceUpdateBatch();

  // Upload vertex data
  uploadBatch->updateDynamicBuffer( mVertexBuffer.get(), 0, vertexData.size() * sizeof( float ), vertexData.constData() );

  // Upload uniform data
  uploadBatch->updateDynamicBuffer( mUniformBuffer.get(), 0, sizeof( UniformBlock ), &uniforms );

  // Collect pending tile texture uploads
  mTileUploader->collectPendingUploads( uploadBatch );

  // Upload colormap texture once (single-band mode)
  if ( !mRGBMode && mColormapTexture && !mColormapUploaded )
  {
    QByteArray colormapData;

    if ( mHasPseudocolorColormap && mPseudocolorData.size() == 256 * 4 )
    {
      // Use pseudocolor colormap from QgsSingleBandPseudoColorRenderer
      colormapData = mPseudocolorData;
    }
    else
    {
      // Create grayscale colormap, respecting gradient direction
      colormapData.resize( 256 * 4 );
      for ( int i = 0; i < 256; ++i )
      {
        // Apply gradient inversion if WhiteToBlack
        const int value = mInvertGradient ? ( 255 - i ) : i;
        colormapData[i * 4 + 0] = static_cast<char>( value );
        colormapData[i * 4 + 1] = static_cast<char>( value );
        colormapData[i * 4 + 2] = static_cast<char>( value );
        colormapData[i * 4 + 3] = static_cast<char>( 255 );
      }
    }

    QRhiTextureSubresourceUploadDescription colormapUpload( colormapData.constData(), colormapData.size() );
    uploadBatch->uploadTexture( mColormapTexture.get(), QRhiTextureUploadDescription( { { 0, 0, colormapUpload } } ) );
    mColormapUploaded = true;
  }

  // Begin render pass
  cb->beginPass( mRenderTarget.get(), Qt::transparent, { 1.0f, 0 }, uploadBatch );

  cb->setGraphicsPipeline( mPipeline.get() );
  cb->setViewport( { 0, 0, static_cast<float>( outputWidth ), static_cast<float>( outputHeight ) } );

  // Bind vertex buffer
  const QRhiCommandBuffer::VertexInput vbufBinding( mVertexBuffer.get(), 0 );
  cb->setVertexInput( 0, 1, &vbufBinding );

  // Render each tile with per-tile texture binding
  int tilesRendered = 0;
  QVector<QRhiShaderResourceBindings *> tileSrbs; // Track for cleanup
  tileSrbs.reserve( tilesToRender.size() );

  for ( int i = 0; i < tilesToRender.size(); ++i )
  {
    const TileRenderData &tile = tilesToRender[i];
    if ( !tile.texture )
      continue;

    // Create per-tile SRB with this tile's texture
    QRhiShaderResourceBindings *tileSrb = createTileSrb( tile.texture );
    if ( !tileSrb )
    {
      QgsDebugMsgLevel( u"Failed to create SRB for tile %1"_s.arg( i ), 3 );
      continue;
    }
    tileSrbs.append( tileSrb );

    cb->setShaderResources( tileSrb );
    cb->draw( VERTICES_PER_TILE, 1, i * VERTICES_PER_TILE, 0 );
    tilesRendered++;
  }

  // Queue readback
  QRhiReadbackResult readbackResult;
  QRhiResourceUpdateBatch *readbackBatch = mRhi->nextResourceUpdateBatch();
  readbackBatch->readBackTexture( { mRenderTexture.get() }, &readbackResult );

  cb->endPass( readbackBatch );

  // End frame - blocks until GPU complete, readbackResult now valid
  mRhi->endOffscreenFrame();

  // Clean up per-tile SRBs (safe after frame ends)
  qDeleteAll( tileSrbs );
  tileSrbs.clear();

  // Convert readback data to QImage and composite
  if ( readbackResult.data.isEmpty() )
  {
    QgsDebugError( u"Readback returned empty data"_s );
    return false;
  }

  QImage gpuImage(
    reinterpret_cast<const uchar *>( readbackResult.data.constData() ),
    outputWidth,
    outputHeight,
    QImage::Format_RGBA8888
  );

  // Must copy since readbackResult.data will be invalid after this function
  gpuImage = gpuImage.copy();

  // Composite GPU-rendered image via QPainter
  QPainter *painter = renderContext.painter();
  if ( painter && !gpuImage.isNull() )
  {
    const QPointF topLeft = renderContext.mapToPixel().transform(
                                                        rasterViewPort->mDrawnExtent.xMinimum(),
                                                        rasterViewPort->mDrawnExtent.yMaximum()
    )
                              .toQPointF();
    painter->drawImage( topLeft, gpuImage );
  }

  // Increment frame number for LRU cache
  mFrameNumber++;

  QgsDebugMsgLevel( u"Rendered %1 tiles at overview level %2"_s.arg( tilesRendered ).arg( overviewLevel ), 2 );

  return tilesRendered > 0;
}

QVector<QgsRasterGPURenderer::TileCoord> QgsRasterGPURenderer::calculateVisibleTiles(
  const QgsRasterViewPort *viewport,
  int overviewLevel,
  const QgsCoordinateTransform &transform
)
{
  QVector<TileCoord> tiles;

  if ( !viewport )
    return tiles;

  // Get viewport extent in raster CRS
  QgsRectangle extent = viewport->mDrawnExtent;

  // Transform extent if needed
  if ( transform.isValid() )
  {
    try
    {
      extent = transform.transformBoundingBox( extent );
    }
    catch ( const QgsCsException &e )
    {
      QgsDebugError( u"Coordinate transform failed: %1"_s.arg( e.what() ) );
      return tiles;
    }
  }

  // Get tile info for this overview level
  const auto tileInfo = mTileUploader->tileInfo( overviewLevel );
  if ( !tileInfo.isTiled || tileInfo.tilesX <= 0 || tileInfo.tilesY <= 0 )
  {
    QgsDebugMsgLevel( u"Raster is not tiled or has invalid tile dimensions"_s, 2 );
    return tiles;
  }

  // Get raster extent
  const QgsRectangle rasterExtent = mTileUploader->rasterExtent();
  if ( rasterExtent.isEmpty() )
  {
    QgsDebugMsgLevel( u"Raster extent is empty"_s, 2 );
    return tiles;
  }

  // Calculate tile size in georeferenced units
  const double tileWidth = rasterExtent.width() / tileInfo.tilesX;
  const double tileHeight = rasterExtent.height() / tileInfo.tilesY;

  // Find tile range that intersects viewport
  const int minTileX = std::max( 0, static_cast<int>( std::floor( ( extent.xMinimum() - rasterExtent.xMinimum() ) / tileWidth ) ) );
  const int maxTileX = std::min( tileInfo.tilesX - 1, static_cast<int>( std::ceil( ( extent.xMaximum() - rasterExtent.xMinimum() ) / tileWidth ) ) );

  const int minTileY = std::max( 0, static_cast<int>( std::floor( ( rasterExtent.yMaximum() - extent.yMaximum() ) / tileHeight ) ) );
  const int maxTileY = std::min( tileInfo.tilesY - 1, static_cast<int>( std::ceil( ( rasterExtent.yMaximum() - extent.yMinimum() ) / tileHeight ) ) );

  // Generate tile coordinates
  for ( int tileY = minTileY; tileY <= maxTileY; ++tileY )
  {
    for ( int tileX = minTileX; tileX <= maxTileX; ++tileX )
    {
      tiles.append( TileCoord { overviewLevel, tileX, tileY } );
    }
  }

  return tiles;
}

int QgsRasterGPURenderer::selectOverviewLevel( const QgsRasterViewPort *viewport )
{
  if ( !viewport || !mTileUploader )
    return 0;

  // Calculate map units per pixel (MUPP)
  const double mapMupp = viewport->mDrawnExtent.width() / viewport->mWidth;

  // Get best overview from tile uploader
  return mTileUploader->selectBestOverview( mapMupp );
}

#endif // HAVE_QRHI
