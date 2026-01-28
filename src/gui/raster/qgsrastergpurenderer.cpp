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

#include <cmath>

#include "qgscoordinatetransform.h"
#include "qgsfeedback.h"
#include "qgslogger.h"
#include "qgsmaptopixel.h"
#include "qgsrastergpushaders.h"
#include "qgsrastergputileuploader.h"
#include "qgsrasterviewport.h"
#include "qgsrectangle.h"
#include "qgsrendercontext.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QString>

using namespace Qt::StringLiterals;

void QgsRasterGPURenderer::setRGBBands( int redBand, int greenBand, int blueBand )
{
  mRGBMode = true;
  mRedBand = redBand;
  mGreenBand = greenBand;
  mBlueBand = blueBand;

  // Force shader recreation on next render
  if ( mShaderProgram )
  {
    delete mShaderProgram;
    mShaderProgram = nullptr;
  }
  if ( mColormapTexture )
  {
    glDeleteTextures( 1, &mColormapTexture );
    mColormapTexture = 0;
  }
}

QgsRasterGPURenderer::QgsRasterGPURenderer( QgsRasterGPUTileUploader *tileUploader )
  : mTileUploader( tileUploader )
{
  if ( QOpenGLContext::currentContext() )
  {
    initializeOpenGLFunctions();
  }
}

QgsRasterGPURenderer::~QgsRasterGPURenderer()
{
  // Clean up OpenGL resources only if we have a valid context
  if ( QOpenGLContext::currentContext() )
  {
    if ( mVBO )
    {
      glDeleteBuffers( 1, &mVBO );
    }
    if ( mColormapTexture )
    {
      glDeleteTextures( 1, &mColormapTexture );
    }
  }
  delete mShaderProgram;
}

bool QgsRasterGPURenderer::render( QgsRenderContext &renderContext, QgsRasterViewPort *rasterViewPort, QgsFeedback *feedback )
{
  if ( !mTileUploader || !rasterViewPort )
  {
    QgsDebugError( u"Invalid tile uploader or viewport"_s );
    return false;
  }

  // Create shader program if needed
  if ( !mShaderProgram && !createShaderProgram() )
  {
    QgsDebugError( u"Failed to create shader program"_s );
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

  // Create or resize FBO for off-screen rendering
  if ( !mFBO || mFBO->width() != outputWidth || mFBO->height() != outputHeight )
  {
    mFBO = std::make_unique<QOpenGLFramebufferObject>( outputWidth, outputHeight, QOpenGLFramebufferObject::CombinedDepthStencil );
    if ( !mFBO->isValid() )
    {
      QgsDebugError( u"Failed to create FBO"_s );
      mFBO.reset();
      return false;
    }
  }

  // Bind FBO for off-screen rendering
  if ( !mFBO->bind() )
  {
    QgsDebugError( u"Failed to bind FBO"_s );
    return false;
  }

  // Set viewport to FBO size
  glViewport( 0, 0, outputWidth, outputHeight );

  // Clear framebuffer (transparent)
  glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
  glClear( GL_COLOR_BUFFER_BIT );

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
    mFBO->release();
    QgsDebugMsgLevel( u"No visible tiles to render"_s, 3 );
    return true;
  }

  // Setup OpenGL rendering state
  setupOpenGLState( renderContext );

  // Bind shader program
  mShaderProgram->bind();

  // Pre-fetch all GPU tiles using batch lookup (single mutex lock)
  struct TileRenderData
  {
      GLuint textureId;
      QgsRectangle extent;
  };
  QVector<TileRenderData> tilesToRender;
  tilesToRender.reserve( visibleTiles.size() );

  if ( mRGBMode )
  {
    // RGB mode: fetch tiles individually (different band combinations)
    for ( const TileCoord &tileCoord : visibleTiles )
    {
      if ( feedback && feedback->isCanceled() )
        break;

      auto gpuTile = mTileUploader->getRGBTile( tileCoord.level, tileCoord.x, tileCoord.y, mRedBand, mGreenBand, mBlueBand, mFrameNumber );
      if ( !gpuTile.isValid )
        continue;

      const QgsRectangle tileExtent = mTileUploader->tileExtent( tileCoord.level, tileCoord.x, tileCoord.y );
      if ( !tileExtent.isEmpty() )
        tilesToRender.append( { gpuTile.textureId, tileExtent } );
    }
  }
  else
  {
    // Single-band mode: use batch lookup (single mutex lock for all tiles)
    QVector<QgsRasterGPUTileUploader::TileCoord> coords;
    coords.reserve( visibleTiles.size() );
    for ( const TileCoord &tc : visibleTiles )
    {
      coords.append( { tc.level, tc.x, tc.y } );
    }

    const auto gpuTiles = mTileUploader->getTiles( coords, 1, mFrameNumber );

    for ( int i = 0; i < gpuTiles.size(); ++i )
    {
      if ( feedback && feedback->isCanceled() )
        break;

      if ( !gpuTiles[i].isValid )
        continue;

      const QgsRectangle tileExtent = mTileUploader->tileExtent( visibleTiles[i].level, visibleTiles[i].x, visibleTiles[i].y );
      if ( !tileExtent.isEmpty() )
        tilesToRender.append( { gpuTiles[i].textureId, tileExtent } );
    }
  }

  if ( tilesToRender.isEmpty() )
  {
    mFBO->release();
    return false;
  }

  // Build batched vertex buffer for all tiles (6 vertices per tile, 4 floats per vertex)
  constexpr int FLOATS_PER_VERTEX = 4; // x, y, u, v
  constexpr int VERTICES_PER_TILE = 6;
  QVector<float> batchedVertices;
  batchedVertices.reserve( tilesToRender.size() * VERTICES_PER_TILE * FLOATS_PER_VERTEX );

  for ( const TileRenderData &tile : tilesToRender )
  {
    const float x0 = tile.extent.xMinimum();
    const float y0 = tile.extent.yMinimum();
    const float x1 = tile.extent.xMaximum();
    const float y1 = tile.extent.yMaximum();

    // Triangle 1
    batchedVertices << x0 << y0 << 0.0f << 1.0f;
    batchedVertices << x1 << y0 << 1.0f << 1.0f;
    batchedVertices << x1 << y1 << 1.0f << 0.0f;
    // Triangle 2
    batchedVertices << x0 << y0 << 0.0f << 1.0f;
    batchedVertices << x1 << y1 << 1.0f << 0.0f;
    batchedVertices << x0 << y1 << 0.0f << 0.0f;
  }

  // Upload batched vertex data once
  if ( !mVBO )
  {
    glGenBuffers( 1, &mVBO );
  }
  glBindBuffer( GL_ARRAY_BUFFER, mVBO );
  glBufferData( GL_ARRAY_BUFFER, batchedVertices.size() * sizeof( float ), batchedVertices.constData(), GL_DYNAMIC_DRAW );

  // Setup vertex attributes (once for all tiles)
  glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof( float ), ( void * ) 0 );
  glEnableVertexAttribArray( 0 );
  glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, FLOATS_PER_VERTEX * sizeof( float ), ( void * ) ( 2 * sizeof( float ) ) );
  glEnableVertexAttribArray( 1 );

  // Build MVP matrix (shared for all tiles)
  QMatrix4x4 mvpMatrix;
  const QgsRectangle &viewExtent = renderContext.extent();
  const float scaleX = 2.0f / viewExtent.width();
  const float scaleY = 2.0f / viewExtent.height();
  const float translateX = -( viewExtent.xMinimum() + viewExtent.xMaximum() ) / viewExtent.width();
  const float translateY = -( viewExtent.yMinimum() + viewExtent.yMaximum() ) / viewExtent.height();
  mvpMatrix.scale( scaleX, scaleY );
  mvpMatrix.translate( translateX, translateY );
  mShaderProgram->setUniformValue( "uMVPMatrix", mvpMatrix );

  // Bind colormap texture once (single-band mode)
  if ( !mRGBMode && mColormapTexture )
  {
    glActiveTexture( GL_TEXTURE1 );
    glBindTexture( GL_TEXTURE_2D, mColormapTexture );
    mShaderProgram->setUniformValue( "uColormapTexture", 1 );
  }

  // Render each tile (only texture binding changes per tile)
  int tilesRendered = 0;
  for ( int i = 0; i < tilesToRender.size(); ++i )
  {
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, tilesToRender[i].textureId );
    mShaderProgram->setUniformValue( "uTileTexture", 0 );

    // Draw 6 vertices (2 triangles) starting at this tile's offset
    glDrawArrays( GL_TRIANGLES, i * VERTICES_PER_TILE, VERTICES_PER_TILE );
    tilesRendered++;
  }

  // Cleanup vertex state
  glDisableVertexAttribArray( 0 );
  glDisableVertexAttribArray( 1 );
  glBindBuffer( GL_ARRAY_BUFFER, 0 );
  glBindTexture( GL_TEXTURE_2D, 0 );

  // Release shader
  mShaderProgram->release();

  // Restore OpenGL state
  restoreOpenGLState();

  // Read FBO content to QImage
  // toImage() handles glReadPixels internally and returns RGBA
  QImage gpuImage = mFBO->toImage();

  // Release FBO
  mFBO->release();

  // Composite GPU-rendered image via QPainter for labels, vectors, etc.
  QPainter *painter = renderContext.painter();
  if ( gpuImage.isNull() )
  {
    QgsDebugError( u"FBO toImage() returned null image"_s );
  }
  else if ( painter )
  {
    const QPointF topLeft = renderContext.mapToPixel().transform( rasterViewPort->mDrawnExtent.xMinimum(), rasterViewPort->mDrawnExtent.yMaximum() ).toQPointF();
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
    QgsDebugMsgLevel( u"Raster is not tiled or has invalid tile dimensions, falling back to CPU path"_s, 2 );
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
  // Division is safe: tilesX/Y > 0 and extent is non-empty (positive dimensions)
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

bool QgsRasterGPURenderer::createShaderProgram()
{
  // Create shader configuration based on rendering mode
  QgsRasterGPUShaders::ShaderConfig config;
  config.opacity = mOpacity;

  if ( mRGBMode )
  {
    // RGB mode: direct RGB output, no colormap
    config.type = QgsRasterGPUShaders::ShaderType::RGB8;
  }
  else
  {
    // Single-band mode: colormap lookup
    config.type = QgsRasterGPUShaders::ShaderType::Byte;

    // Default grayscale color ramp (black → white)
    config.colorRamp = {
      { 0.0f, QColor( 0, 0, 0 ) },
      { 1.0f, QColor( 255, 255, 255 ) }
    };

    config.minValue = 0.0f;
    config.maxValue = 255.0f;
  }

  mShaderProgram = QgsRasterGPUShaders::createShaderProgram( config );

  if ( !mShaderProgram )
  {
    QgsDebugError( u"Failed to create shader program"_s );
    return false;
  }

  // Create colormap texture only for single-band mode
  if ( !mRGBMode )
  {
    mColormapTexture = QgsRasterGPUShaders::createColormapTexture( config.colorRamp );
    if ( !mColormapTexture )
    {
      QgsDebugError( u"Failed to create colormap texture"_s );
      delete mShaderProgram;
      mShaderProgram = nullptr;
      return false;
    }
  }

  // Update uniforms
  QgsRasterGPUShaders::updateShaderUniforms( mShaderProgram, config );

  return true;
}

void QgsRasterGPURenderer::setupOpenGLState( const QgsRenderContext &context )
{
  Q_UNUSED( context )

  // Enable blending for transparency
  glEnable( GL_BLEND );
  glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

  // Disable depth test (2D rendering)
  glDisable( GL_DEPTH_TEST );

  // Note: Viewport is managed by QGIS painter context
  // No explicit glViewport() call needed here
}

void QgsRasterGPURenderer::restoreOpenGLState()
{
  // Restore default state
  glDisable( GL_BLEND );
  glEnable( GL_DEPTH_TEST );
}
