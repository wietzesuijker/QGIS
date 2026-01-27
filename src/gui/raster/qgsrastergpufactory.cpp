/***************************************************************************
  qgsrastergpufactory.cpp
  -----------------------
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

#include "qgsrastergpufactory.h"

#include "qgslogger.h"
#include "qgsrasterdataprovider.h"
#include "qgsrastergpucachemanager.h"
#include "qgsrastergpurenderer.h"
#include "qgsrastergputileuploader.h"
#include "qgsrasterlayerrenderer.h"
#include "qgsrasterpipe.h"
#include "qgsrendercontext.h"

#include <QCoreApplication>
#include <QOpenGLContext>
#include <QString>
#include <QThread>

using namespace Qt::StringLiterals;

namespace
{
  // Thread that initialized GPU rendering (owns the OpenGL context)
  static QThread *sGpuThread = nullptr;

  /**
   * \brief GPU renderer factory implementation
   *
   * This function is called by QgsRasterLayerRenderer when GPU rendering
   * should be attempted. Uses persistent tile cache for texture reuse.
   *
   * Thread safety: GPU rendering only works on the thread that initialized it
   * (typically the main/GUI thread). Other threads fall back to CPU rendering.
   * This is because OpenGL contexts are thread-local and cannot be safely
   * shared without explicit context sharing setup.
   */
  bool gpuRendererFactoryImpl(
    QgsRenderContext &context,
    QgsRasterViewPort *viewport,
    QgsRasterPipe *pipe,
    QgsFeedback *feedback
  )
  {
    // Thread safety: Only allow GPU rendering on the thread that initialized it
    // OpenGL contexts are thread-local; using them from other threads causes crashes
    if ( sGpuThread && QThread::currentThread() != sGpuThread )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: wrong thread (expected %1, got %2)"_s.arg( reinterpret_cast<quintptr>( sGpuThread ) ).arg( reinterpret_cast<quintptr>( QThread::currentThread() ) ), 4 );
      return false;
    }

    // Check for OpenGL context
    if ( !QOpenGLContext::currentContext() )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: no OpenGL context"_s, 4 );
      return false;
    }

    // Get data provider
    QgsRasterDataProvider *provider = pipe ? pipe->provider() : nullptr;
    if ( !provider )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: no data provider"_s, 4 );
      return false;
    }

    // Get or create cached tile uploader (persistent across renders)
    const QString dataSource = provider->dataSourceUri();
    QgsRasterGPUTileUploader *uploader = QgsRasterGPUCacheManager::instance()->getOrCreateUploader( dataSource );

    if ( !uploader )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: could not create uploader"_s, 4 );
      return false;
    }

    // Create GPU renderer (lightweight, uses cached uploader and textures)
    QgsRasterGPURenderer gpuRenderer( uploader );

    // Attempt GPU rendering
    try
    {
      const bool success = gpuRenderer.render( context, viewport, feedback );
      if ( success )
      {
        QgsDebugMsgLevel( u"GPU rendering completed successfully"_s, 3 );
      }
      return success;
    }
    catch ( const std::exception &e )
    {
      QgsDebugError( u"GPU rendering exception: %1"_s.arg( e.what() ) );
      return false;
    }
  }
} //namespace

void QgsRasterGPUFactory::initialize()
{
  QgsDebugMsgLevel( u"Initializing GPU raster rendering support"_s, 2 );

  // Record the thread that owns the GPU context
  // GPU rendering will only work on this thread
  sGpuThread = QThread::currentThread();

  // Register the GPU renderer factory
  QgsRasterLayerRenderer::setGpuRendererFactory( gpuRendererFactoryImpl );

  QgsDebugMsgLevel( u"GPU raster rendering support enabled on thread %1"_s.arg( reinterpret_cast<quintptr>( sGpuThread ) ), 2 );
}

void QgsRasterGPUFactory::cleanup()
{
  QgsDebugMsgLevel( u"Cleaning up GPU raster rendering support"_s, 2 );

  // Clear all cached GPU resources
  QgsRasterGPUCacheManager::instance()->clearAll();

  // Unregister the factory
  QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );

  // Clear thread ownership
  sGpuThread = nullptr;
}
