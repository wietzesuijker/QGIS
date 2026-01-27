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

  // Render each tile
  int tilesRendered = 0;
  for ( const TileCoord &tileCoord : visibleTiles )
  {
    // Check for cancellation
    if ( feedback && feedback->isCanceled() )
    {
      break;
    }

    // Upload tile to GPU (uses cache)
    // TODO: Support multi-band rendering by getting band selection from QgsRasterRenderer
    const int bandNumber = 1;
    const auto gpuTile = mTileUploader->getTile( tileCoord.level, tileCoord.x, tileCoord.y, bandNumber, mFrameNumber );

    if ( !gpuTile.isValid )
    {
      QgsDebugMsgLevel( u"Failed to upload tile %1/%2/%3"_s.arg( tileCoord.level ).arg( tileCoord.x ).arg( tileCoord.y ), 4 );
      continue;
    }

    // Calculate tile extent in map coordinates
    const QgsRectangle tileExtent = mTileUploader->tileExtent( tileCoord.level, tileCoord.x, tileCoord.y );

    // Render the tile quad
    renderTileQuad( gpuTile.textureId, tileExtent, renderContext );

    tilesRendered++;
  }

  // Release shader
  mShaderProgram->release();

  // Restore OpenGL state
  restoreOpenGLState();

  // Read FBO content to QImage
  // toImage() handles glReadPixels internally and returns RGBA
  QImage gpuImage = mFBO->toImage();

  // Release FBO
  mFBO->release();

  // Draw the GPU-rendered image to QPainter for compositing with labels, vectors, etc.
  QPainter *painter = renderContext.painter();
  if ( painter && !gpuImage.isNull() )
  {
    // Calculate device coordinates for the raster
    // The image covers the viewport extent
    const QPointF topLeft = renderContext.mapToPixel().transform( rasterViewPort->mDrawnExtent.xMinimum(), rasterViewPort->mDrawnExtent.yMaximum() ).toQPointF();

    painter->drawImage( topLeft, gpuImage );

    QgsDebugMsgLevel( u"GPU image composited to QPainter at (%1, %2)"_s.arg( topLeft.x() ).arg( topLeft.y() ), 3 );
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
  if ( !tileInfo.isTiled )
  {
    QgsDebugMsgLevel( u"Raster is not tiled, falling back to CPU path"_s, 2 );
    return tiles;
  }

  // Get raster extent
  const QgsRectangle rasterExtent = mTileUploader->rasterExtent();

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

void QgsRasterGPURenderer::renderTileQuad( GLuint textureId, const QgsRectangle &tileExtent, const QgsRenderContext &context )
{
  // Bind tile texture to unit 0
  glActiveTexture( GL_TEXTURE0 );
  glBindTexture( GL_TEXTURE_2D, textureId );
  mShaderProgram->setUniformValue( "uTileTexture", 0 );

  // Bind colormap texture to unit 1 (deck.gl pattern)
  glActiveTexture( GL_TEXTURE1 );
  glBindTexture( GL_TEXTURE_2D, mColormapTexture );
  mShaderProgram->setUniformValue( "uColormapTexture", 1 );

  // Build MVP matrix (map extent → screen coordinates)
  // Transform from tile's map coordinates to normalized device coordinates [-1, 1]
  QMatrix4x4 mvpMatrix;

  const QgsRectangle &viewExtent = context.extent();

  // Scale: map units → normalized device coordinates
  const float scaleX = 2.0f / viewExtent.width();
  const float scaleY = 2.0f / viewExtent.height();

  // Translate: center map extent at origin
  const float translateX = -( viewExtent.xMinimum() + viewExtent.xMaximum() ) / viewExtent.width();
  const float translateY = -( viewExtent.yMinimum() + viewExtent.yMaximum() ) / viewExtent.height();

  mvpMatrix.scale( scaleX, scaleY );
  mvpMatrix.translate( translateX, translateY );

  mShaderProgram->setUniformValue( "uMVPMatrix", mvpMatrix );

  // Define tile quad vertices (in map coordinates)
  const float x0 = tileExtent.xMinimum();
  const float y0 = tileExtent.yMinimum();
  const float x1 = tileExtent.xMaximum();
  const float y1 = tileExtent.yMaximum();

  // Vertex data: position (x, y) + texcoord (u, v)
  const float vertices[] = {
    // Triangle 1
    x0, y0, 0.0f, 1.0f, // bottom-left
    x1, y0, 1.0f, 1.0f, // bottom-right
    x1, y1, 1.0f, 0.0f, // top-right

    // Triangle 2
    x0, y0, 0.0f, 1.0f, // bottom-left
    x1, y1, 1.0f, 0.0f, // top-right
    x0, y1, 0.0f, 0.0f, // top-left
  };

  // Upload vertex data
  if ( !mVBO )
  {
    glGenBuffers( 1, &mVBO );
  }

  glBindBuffer( GL_ARRAY_BUFFER, mVBO );
  glBufferData( GL_ARRAY_BUFFER, sizeof( vertices ), vertices, GL_DYNAMIC_DRAW );

  // Setup vertex attributes
  glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), ( void * ) 0 );
  glEnableVertexAttribArray( 0 );

  glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), ( void * ) ( 2 * sizeof( float ) ) );
  glEnableVertexAttribArray( 1 );

  // Draw tile quad
  glDrawArrays( GL_TRIANGLES, 0, 6 );

  // Cleanup
  glDisableVertexAttribArray( 0 );
  glDisableVertexAttribArray( 1 );
  glBindBuffer( GL_ARRAY_BUFFER, 0 );
  glBindTexture( GL_TEXTURE_2D, 0 );
}

bool QgsRasterGPURenderer::createShaderProgram()
{
  // Create shader configuration with default values
  // Future enhancement: Get data type and color ramp from QgsRasterRenderer
  QgsRasterGPUShaders::ShaderConfig config;
  config.type = QgsRasterGPUShaders::ShaderType::Byte;
  config.opacity = mOpacity;

  // Default grayscale color ramp (black → white)
  config.colorRamp = {
    { 0.0f, QColor( 0, 0, 0 ) },
    { 1.0f, QColor( 255, 255, 255 ) }
  };

  config.minValue = 0.0f;
  config.maxValue = 255.0f;

  mShaderProgram = QgsRasterGPUShaders::createShaderProgram( config );

  if ( !mShaderProgram )
  {
    QgsDebugError( u"Failed to create shader program"_s );
    return false;
  }

  // Create colormap texture (deck.gl pattern: 1D texture lookup)
  mColormapTexture = QgsRasterGPUShaders::createColormapTexture( config.colorRamp );
  if ( !mColormapTexture )
  {
    QgsDebugError( u"Failed to create colormap texture"_s );
    delete mShaderProgram;
    mShaderProgram = nullptr;
    return false;
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
