/***************************************************************************
  qgsrastergpurenderer.h
  ----------------------
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

#ifndef QGSRASTERGPURENDERER_H
#define QGSRASTERGPURENDERER_H

#include <memory>

#include "qgis_gui.h"
#include "qgis_sip.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>

class QgsRasterGPUTileUploader;
class QgsRasterGPUShaders;
class QgsRenderContext;
struct QgsRasterViewPort;
class QgsCoordinateTransform;
class QgsFeedback;
class QgsRectangle;
class QOpenGLShaderProgram;
class QgsCoordinateReferenceSystem;

/**
 * \ingroup gui
 * \class QgsRasterGPURenderer
 * \brief GPU-accelerated raster tile renderer using OpenGL.
 *
 * Renders COG raster tiles directly on GPU, bypassing the traditional
 * CPU-based per-pixel color mapping. Uses:
 *
 * - QgsRasterTileReader for fast tile access via GDALReadBlock()
 * - QgsRasterGPUTileUploader for texture uploads with LRU caching
 * - QgsRasterGPUShaders for GLSL-based color mapping
 * - QOpenGLFramebufferObject for off-screen rendering
 *
 * Rendering pipeline:
 * 1. Calculate visible tiles from viewport
 * 2. Select best overview level based on map scale
 * 3. Upload tiles to GPU textures (cached with LRU)
 * 4. Render textured quads to FBO with shader-based color mapping
 * 5. Read FBO to QImage and composite via QPainter
 *
 * Thread safety: GPU rendering only works on the thread that initialized
 * QgsRasterGPUFactory. Other threads fall back to CPU rendering automatically.
 *
 * Supports both single-band (with colormap) and multi-band RGB rendering.
 * Use setRGBBands() to enable multi-band color rendering from QgsMultiBandColorRenderer.
 *
 * Current limitation:
 *
 * - No on-GPU reprojection. Falls back to CPU when CRS transformation needed.
 *
 * Inspired by deck.gl-raster's WebGL rendering approach.
 *
 * \since QGIS 3.40
 */
class GUI_EXPORT QgsRasterGPURenderer : protected QOpenGLFunctions
{
  public:
    /**
     * Constructor
     * \param tileUploader GPU tile uploader instance
     */
    explicit QgsRasterGPURenderer( QgsRasterGPUTileUploader *tileUploader );

    ~QgsRasterGPURenderer();

    /**
     * Render raster to the given render context
     * \param renderContext QGIS render context with QPainter
     * \param rasterViewPort viewport defining extent and resolution
     * \param feedback optional feedback object for cancellation
     * \returns true on success
     */
    bool render( QgsRenderContext &renderContext, QgsRasterViewPort *rasterViewPort, QgsFeedback *feedback = nullptr );

    /**
     * Set opacity for rendering (0.0 - 1.0)
     */
    void setOpacity( float opacity ) { mOpacity = opacity; }

    /**
     * Get current opacity
     */
    float opacity() const { return mOpacity; }

    /**
     * Set RGB band numbers for multi-band color rendering.
     * Call this with band numbers from QgsMultiBandColorRenderer.
     * When RGB mode is set, the renderer uses 3-channel textures
     * instead of single-band with colormap lookup.
     * \param redBand Red band number (1-based)
     * \param greenBand Green band number (1-based)
     * \param blueBand Blue band number (1-based)
     */
    void setRGBBands( int redBand, int greenBand, int blueBand );

    /**
     * Returns TRUE if RGB mode is enabled (multi-band rendering).
     */
    bool isRGBMode() const { return mRGBMode; }

  private:
    struct TileCoord
    {
        int level;
        int x;
        int y;
    };

    /**
     * Calculate visible tiles for the given viewport
     */
    QVector<TileCoord> calculateVisibleTiles(
      const QgsRasterViewPort *viewport,
      int overviewLevel,
      const QgsCoordinateTransform &transform
    );

    /**
     * Select best overview level based on map scale
     */
    int selectOverviewLevel( const QgsRasterViewPort *viewport );

    /**
     * Render a single tile quad with texture
     */
    void renderTileQuad( GLuint textureId, const QgsRectangle &tileExtent, const QgsRenderContext &context );

    /**
     * Create shader program for current data type
     */
    bool createShaderProgram();

    /**
     * Setup OpenGL viewport and state
     */
    void setupOpenGLState( const QgsRenderContext &context );

    /**
     * Restore OpenGL state
     */
    void restoreOpenGLState();

    QgsRasterGPUTileUploader *mTileUploader = nullptr;
    QOpenGLShaderProgram *mShaderProgram = nullptr;
    std::unique_ptr<QOpenGLFramebufferObject> mFBO;

    float mOpacity = 1.0f;
    quint64 mFrameNumber = 0;

    // RGB mode (multi-band rendering)
    bool mRGBMode = false;
    int mRedBand = 1;
    int mGreenBand = 2;
    int mBlueBand = 3;

    // Vertex buffer for tile quads
    GLuint mVBO = 0;

    // Colormap texture (1D lookup, deck.gl pattern)
    GLuint mColormapTexture = 0;
};

#endif // QGSRASTERGPURENDERER_H
