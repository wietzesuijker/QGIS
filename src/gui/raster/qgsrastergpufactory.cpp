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
#include "qgsrasterlayerrenderer.h"

#include <QString>
#include <QThread>

#ifdef HAVE_QRHI

#include <QOffscreenSurface>
#include <QSurfaceFormat>

#include "qgscontrastenhancement.h"
#include "qgsmultibandcolorrenderer.h"
#include "qgsproject.h"
#include "qgsrasterdataprovider.h"
#include "qgsrastergpucachemanager.h"
#include "qgsrasterlayer.h"
#include "qgsrastergpurenderer.h"
#include "qgsrastergputileuploader.h"
#include "qgsrasterpipe.h"
#include "qgsrasterrenderer.h"
#include "qgsrendercontext.h"
#include "qgssinglebandgrayrenderer.h"

using namespace Qt::StringLiterals;

/**
 * Architecture note: Core/GUI Separation
 *
 * GPU rendering requires QRhi (via Qt6::GuiPrivate) which lives in gui.
 * To avoid making core depend on gui, we use a factory callback pattern:
 *
 * - QgsRasterLayerRenderer (core) defines GpuRendererFactory callback type
 * - QgsRasterGPUFactory (gui) implements and registers the factory at startup
 * - QgsRasterTileReader (core) handles GDAL tile reading - no GPU code
 * - QgsRasterGPUTileUploader (gui) handles texture upload via QRhi
 *
 * This keeps libqgis_core.so free of GPU symbols while allowing optional
 * GPU acceleration when gui is loaded.
 */

namespace
{
  // Thread that initialized GPU rendering (owns the QRhi context)
  static QThread *sGpuThread = nullptr;

  // QRhi instance (owned by this factory)
  static std::unique_ptr<QRhi> sRhi;

  /**
   * \brief Create QRhi with platform-appropriate backend
   *
   * Backend selection:
   *
   * - macOS: Metal
   * - Windows: Direct3D 11
   * - Linux: Vulkan (fallback to OpenGL)
   *
   * \returns QRhi instance or nullptr on failure
   */
  std::unique_ptr<QRhi> createRhi()
  {
    QRhi::Flags flags;
    std::unique_ptr<QRhi> rhi;

    // Detect software rendering environment (CI, xvfb, etc.)
    const bool isSoftwareRendering = !qgetenv( "LIBGL_ALWAYS_SOFTWARE" ).isEmpty() || !qgetenv( "MESA_GL_VERSION_OVERRIDE" ).isEmpty();

    // Enable software renderer preference for CI environments
    if ( isSoftwareRendering )
    {
      flags |= QRhi::PreferSoftwareRenderer;
      QgsDebugMsgLevel( u"Software rendering detected, enabling PreferSoftwareRenderer flag"_s, 2 );
    }

#if defined( Q_OS_MACOS ) || defined( Q_OS_IOS )
    // macOS/iOS: Metal
    QRhiMetalInitParams metalParams;
    rhi.reset( QRhi::create( QRhi::Metal, &metalParams, flags ) );
    if ( rhi )
    {
      QgsDebugMsgLevel( u"QRhi initialized with Metal backend"_s, 2 );
      return rhi;
    }
    QgsDebugMsgLevel( u"Metal initialization failed, trying OpenGL fallback"_s, 2 );
#endif

#if defined( Q_OS_WIN )
    // Windows: Direct3D 11
    QRhiD3D11InitParams d3d11Params;
    rhi.reset( QRhi::create( QRhi::D3D11, &d3d11Params, flags ) );
    if ( rhi )
    {
      QgsDebugMsgLevel( u"QRhi initialized with D3D11 backend"_s, 2 );
      return rhi;
    }
    QgsDebugMsgLevel( u"D3D11 initialization failed, trying OpenGL fallback"_s, 2 );
#endif

#if defined( Q_OS_LINUX )
    // Linux: Try Vulkan first, but skip when:
    // - QT_QPA_PLATFORM=offscreen (headless mode)
    // - Software rendering is detected (e.g., CI with xvfb)
    const QString platform = QString::fromLatin1( qgetenv( "QT_QPA_PLATFORM" ) );

    if ( platform != u"offscreen"_s && !isSoftwareRendering )
    {
      QRhiVulkanInitParams vulkanParams;
      rhi.reset( QRhi::create( QRhi::Vulkan, &vulkanParams, flags ) );
      if ( rhi )
      {
        QgsDebugMsgLevel( u"QRhi initialized with Vulkan backend"_s, 2 );
        return rhi;
      }
      QgsDebugMsgLevel( u"Vulkan initialization failed, trying OpenGL fallback"_s, 2 );
    }
    else if ( isSoftwareRendering )
    {
      QgsDebugMsgLevel( u"Software rendering detected, skipping Vulkan"_s, 2 );
    }
#endif

    // Fallback: OpenGL ES 2 (compatible mode)
    // Per Qt docs: use newFallbackSurface() for properly configured surface
    // See: https://doc.qt.io/qt-6/qrhigles2initparams.html
    static QOffscreenSurface *sFallbackSurface = nullptr;
    if ( !sFallbackSurface )
    {
      // Try Qt's recommended method first
      sFallbackSurface = QRhiGles2InitParams::newFallbackSurface();

      // If that fails (e.g. headless CI), create manually with minimal requirements
      if ( !sFallbackSurface || !sFallbackSurface->isValid() )
      {
        qWarning() << "QRhi: newFallbackSurface() failed, trying manual creation";
        delete sFallbackSurface;

        // Use default format (most compatible)
        sFallbackSurface = new QOffscreenSurface();
        sFallbackSurface->setFormat( QSurfaceFormat::defaultFormat() );
        sFallbackSurface->create();

        if ( !sFallbackSurface->isValid() )
        {
          qWarning() << "QRhi: Failed to create OpenGL offscreen surface";
          delete sFallbackSurface;
          sFallbackSurface = nullptr;
          return nullptr;
        }
      }
      QgsDebugMsgLevel( QStringLiteral( "QRhi: Created OpenGL offscreen surface" ), 2 );
    }

    QRhiGles2InitParams glesParams;
    glesParams.fallbackSurface = sFallbackSurface;
    rhi.reset( QRhi::create( QRhi::OpenGLES2, &glesParams, flags ) );
    if ( rhi )
    {
      QgsDebugMsgLevel( QStringLiteral( "QRhi: Initialized with OpenGL ES 2 backend" ), 2 );
      return rhi;
    }

    qWarning() << "QRhi: Failed to create OpenGL ES 2 backend";
    return nullptr;
  }

  /**
   * \brief GPU renderer factory implementation
   *
   * This function is called by QgsRasterLayerRenderer when GPU rendering
   * should be attempted. Uses persistent tile cache for texture reuse.
   *
   * Thread safety: GPU rendering only works on the thread that initialized it.
   * Other threads fall back to CPU rendering.
   */
  bool gpuRendererFactoryImpl(
    QgsRenderContext &context,
    QgsRasterViewPort *viewport,
    QgsRasterPipe *pipe,
    QgsFeedback *feedback
  )
  {
    // Thread safety: Only allow GPU rendering on the thread that initialized it
    if ( sGpuThread && QThread::currentThread() != sGpuThread )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: wrong thread"_s, 4 );
      return false;
    }

    // Check for QRhi
    if ( !sRhi )
    {
      QgsDebugMsgLevel( u"GPU rendering skipped: no QRhi context"_s, 4 );
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
    QgsRasterGPURenderer gpuRenderer( uploader, sRhi.get() );

    // Extract styling from QGIS raster renderer
    QgsRasterRenderer *rasterRenderer = pipe ? pipe->renderer() : nullptr;
    if ( rasterRenderer )
    {
      // Check for multi-band color renderer (RGB)
      if ( QgsMultiBandColorRenderer *mbRenderer = dynamic_cast<QgsMultiBandColorRenderer *>( rasterRenderer ) )
      {
        const int redBand = mbRenderer->redBand();
        const int greenBand = mbRenderer->greenBand();
        const int blueBand = mbRenderer->blueBand();

        if ( redBand > 0 && greenBand > 0 && blueBand > 0 )
        {
          gpuRenderer.setRGBBands( redBand, greenBand, blueBand );

          // Extract per-band contrast enhancement
          const QgsContrastEnhancement *redCE = mbRenderer->redContrastEnhancement();
          const QgsContrastEnhancement *greenCE = mbRenderer->greenContrastEnhancement();
          const QgsContrastEnhancement *blueCE = mbRenderer->blueContrastEnhancement();

          if ( redCE || greenCE || blueCE )
          {
            // Use enhancement values if available, otherwise fall back to 0-255
            const double redMin = redCE ? redCE->minimumValue() : 0.0;
            const double redMax = redCE ? redCE->maximumValue() : 255.0;
            const double greenMin = greenCE ? greenCE->minimumValue() : 0.0;
            const double greenMax = greenCE ? greenCE->maximumValue() : 255.0;
            const double blueMin = blueCE ? blueCE->minimumValue() : 0.0;
            const double blueMax = blueCE ? blueCE->maximumValue() : 255.0;

            gpuRenderer.setRGBContrastEnhancement( redMin, redMax, greenMin, greenMax, blueMin, blueMax );

            // Extract algorithm from first available enhancement (typically all same)
            const QgsContrastEnhancement *firstCE = redCE ? redCE : ( greenCE ? greenCE : blueCE );
            if ( firstCE )
            {
              gpuRenderer.setContrastEnhancementAlgorithm( static_cast<int>( firstCE->contrastEnhancementAlgorithm() ) );
            }

            QgsDebugMsgLevel( u"GPU rendering RGB contrast: R[%1-%2], G[%3-%4], B[%5-%6] algo=%7"_s.arg( redMin ).arg( redMax ).arg( greenMin ).arg( greenMax ).arg( blueMin ).arg( blueMax ).arg( firstCE ? static_cast<int>( firstCE->contrastEnhancementAlgorithm() ) : 1 ), 3 );
          }

          // Extract per-band NoData from provider
          if ( provider->sourceHasNoDataValue( redBand ) || provider->sourceHasNoDataValue( greenBand ) || provider->sourceHasNoDataValue( blueBand ) )
          {
            const double redNoData = provider->sourceHasNoDataValue( redBand ) ? provider->sourceNoDataValue( redBand ) : 0.0;
            const double greenNoData = provider->sourceHasNoDataValue( greenBand ) ? provider->sourceNoDataValue( greenBand ) : 0.0;
            const double blueNoData = provider->sourceHasNoDataValue( blueBand ) ? provider->sourceNoDataValue( blueBand ) : 0.0;
            gpuRenderer.setRGBNoData( redNoData, greenNoData, blueNoData );
          }

          QgsDebugMsgLevel( u"GPU rendering using RGB bands: R=%1, G=%2, B=%3"_s.arg( redBand ).arg( greenBand ).arg( blueBand ), 3 );
        }
      }
      // Check for single-band gray renderer
      else if ( QgsSingleBandGrayRenderer *grayRenderer = dynamic_cast<QgsSingleBandGrayRenderer *>( rasterRenderer ) )
      {
        const int grayBand = grayRenderer->inputBand();
        if ( grayBand > 0 )
        {
          gpuRenderer.setInputBand( grayBand );

          // Extract contrast enhancement
          const QgsContrastEnhancement *ce = grayRenderer->contrastEnhancement();
          if ( ce )
          {
            const bool invertGradient = ( grayRenderer->gradient() == QgsSingleBandGrayRenderer::WhiteToBlack );
            gpuRenderer.setContrastEnhancement( ce->minimumValue(), ce->maximumValue(), invertGradient );
            gpuRenderer.setContrastEnhancementAlgorithm( static_cast<int>( ce->contrastEnhancementAlgorithm() ) );
            QgsDebugMsgLevel( u"GPU rendering gray band=%1, min=%2, max=%3, invert=%4, algo=%5"_s.arg( grayBand ).arg( ce->minimumValue() ).arg( ce->maximumValue() ).arg( invertGradient ).arg( static_cast<int>( ce->contrastEnhancementAlgorithm() ) ), 3 );
          }
          else
          {
            // No contrast enhancement, use NoEnhancement algorithm
            const bool invertGradient = ( grayRenderer->gradient() == QgsSingleBandGrayRenderer::WhiteToBlack );
            gpuRenderer.setContrastEnhancementAlgorithm( 0 ); // NoEnhancement
            if ( invertGradient )
            {
              gpuRenderer.setContrastEnhancement( 0.0, 255.0, true );
            }
          }
        }
      }
      // For other renderer types (pseudocolor, etc.), fall back to CPU rendering
      else
      {
        QgsDebugMsgLevel( u"GPU rendering skipped: unsupported renderer type %1"_s.arg( rasterRenderer->type() ), 3 );
        return false;
      }
    }

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
} // namespace

QRhi *QgsRasterGPUFactory::rhi()
{
  return sRhi.get();
}

#endif // HAVE_QRHI

void QgsRasterGPUFactory::initialize()
{
#ifdef HAVE_QRHI
  QgsDebugMsgLevel( QStringLiteral( "Initializing GPU raster rendering support (QRhi)" ), 2 );

  // Record the thread that owns the GPU context
  sGpuThread = QThread::currentThread();

  // Create QRhi
  sRhi = createRhi();

  if ( !sRhi )
  {
    QgsDebugError( QStringLiteral( "Failed to create QRhi - GPU rendering disabled" ) );
    sGpuThread = nullptr;
    return;
  }

  // Register the GPU renderer factory
  QgsRasterLayerRenderer::setGpuRendererFactory( gpuRendererFactoryImpl );

  // Connect to project layer removal to clean up GPU caches
  QObject::connect( QgsProject::instance(), &QgsProject::layersRemoved, []( const QStringList &layerIds ) {
    Q_UNUSED( layerIds )
    // When layers are removed, we can't easily map layer IDs to data source URIs,
    // so we rely on LRU eviction during rendering. For explicit cleanup,
    // users can call QgsRasterGPUCacheManager::clearAll() during shutdown.
    // Future enhancement: track layer ID -> URI mapping for targeted cleanup.
    QgsDebugMsgLevel( QStringLiteral( "Layers removed, GPU cache will be evicted via LRU" ), 3 );
  } );

  QgsDebugMsgLevel( QStringLiteral( "GPU raster rendering support enabled (backend: %1)" ).arg( QString::fromLatin1( sRhi->backendName() ) ), 2 );
#else
  QgsDebugMsgLevel( QStringLiteral( "GPU raster rendering not available (HAVE_QRHI not defined)" ), 2 );
#endif
}

void QgsRasterGPUFactory::cleanup()
{
#ifdef HAVE_QRHI
  QgsDebugMsgLevel( QStringLiteral( "Cleaning up GPU raster rendering support" ), 2 );

  // Clear and delete the cache manager singleton (releases GPU resources)
  QgsRasterGPUCacheManager::cleanup();

  // Unregister the factory
  QgsRasterLayerRenderer::setGpuRendererFactory( nullptr );

  // Release QRhi (releases all GPU resources)
  sRhi.reset();

  // Clear thread ownership
  sGpuThread = nullptr;
#endif
}
