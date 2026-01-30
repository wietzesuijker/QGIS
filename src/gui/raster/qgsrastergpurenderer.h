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

#define SIP_NO_FILE

#include <memory>

#include "qgis_gui.h"

#include <QColor>

#ifdef HAVE_QRHI
#include <rhi/qrhi.h>
#endif

class QgsRasterGPUTileUploader;
class QgsRenderContext;
struct QgsRasterViewPort;
class QgsCoordinateTransform;
class QgsFeedback;
class QgsRectangle;
class QgsCoordinateReferenceSystem;

/**
 * \ingroup gui
 * \class QgsRasterGPURenderer
 * \brief GPU-accelerated raster tile renderer using QRhi.
 *
 * Renders COG raster tiles directly on GPU using Qt's Rendering Hardware
 * Interface (QRhi), providing backend-agnostic GPU acceleration across
 * Vulkan, Metal, Direct3D 11, and OpenGL.
 *
 * Uses:
 *
 * - QgsRasterTileReader for fast tile access via GDALReadBlock()
 * - QgsRasterGPUTileUploader for texture uploads with LRU caching
 * - Precompiled .qsb shaders for GLSL-based color mapping
 * - QRhiTextureRenderTarget for off-screen rendering
 *
 * Rendering pipeline:
 * 1. Calculate visible tiles from viewport
 * 2. Select best overview level based on map scale
 * 3. Upload tiles to GPU textures (cached with LRU)
 * 4. Render textured quads to render target with shader-based color mapping
 * 5. Synchronous readback via endOffscreenFrame()
 * 6. Composite via QPainter
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
 * \since QGIS 3.44
 */
class GUI_EXPORT QgsRasterGPURenderer
{
  public:
#ifdef HAVE_QRHI
    /**
     * Constructor
     * \param tileUploader GPU tile uploader instance (ownership NOT transferred)
     * \param rhi QRhi instance (ownership NOT transferred)
     */
    explicit QgsRasterGPURenderer( QgsRasterGPUTileUploader *tileUploader, QRhi *rhi );

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

    /**
     * Set contrast enhancement parameters for single-band rendering.
     * Values are extracted from QgsSingleBandGrayRenderer's contrast enhancement.
     * \param minValue Minimum value for stretch (maps to 0)
     * \param maxValue Maximum value for stretch (maps to 255)
     * \param invertGradient TRUE for WhiteToBlack gradient
     */
    void setContrastEnhancement( double minValue, double maxValue, bool invertGradient = false );

    /**
     * Set per-band contrast enhancement for RGB rendering.
     * Values are extracted from QgsMultiBandColorRenderer's per-band enhancements.
     * \param redMin Red band minimum value
     * \param redMax Red band maximum value
     * \param greenMin Green band minimum value
     * \param greenMax Green band maximum value
     * \param blueMin Blue band minimum value
     * \param blueMax Blue band maximum value
     */
    void setRGBContrastEnhancement( double redMin, double redMax, double greenMin, double greenMax, double blueMin, double blueMax );

    /**
     * Set the input band for single-band rendering.
     * \param band Band number (1-based)
     */
    void setInputBand( int band ) { mInputBand = band; }

    /**
     * Set contrast enhancement algorithm.
     * \param algorithm 0=NoEnhancement, 1=StretchToMinimumMaximum, 2=ClipToMinimumMaximum, 3=StretchAndClipToMinimumMaximum
     */
    void setContrastEnhancementAlgorithm( int algorithm ) { mAlgorithm = algorithm; }

    /**
     * Set per-band NoData values for RGB rendering.
     * \param redNoData NoData value for red band
     * \param greenNoData NoData value for green band
     * \param blueNoData NoData value for blue band
     */
    void setRGBNoData( double redNoData, double greenNoData, double blueNoData );

    /**
     * Set NoData tolerance for float comparison.
     * \param tolerance Absolute tolerance for NoData comparison (default 0.5)
     */
    void setNoDataTolerance( double tolerance ) { mNoDataTolerance = tolerance; }

    /**
     * Set brightness/contrast/gamma filter parameters.
     * These match the QgsBrightnessContrastFilter pipeline stage.
     *
     * Formulas (matching CPU implementation):
     *
     * - contrastFactor = pow((contrast+100)/100, 2)
     * - gammaCorrection = 1/gamma
     *
     * \param brightness Brightness adjustment (-255 to 255)
     * \param contrast Contrast adjustment (-100 to 100)
     * \param gamma Gamma correction (0.1 to 10.0)
     */
    void setBrightnessContrastGamma( int brightness, int contrast, double gamma );

    /**
     * Set hue/saturation filter parameters.
     * These match the QgsHueSaturationFilter pipeline stage.
     *
     * Processing order: Invert → Grayscale/Saturation → Colorize
     *
     * \param invert Whether to invert colors (1 - RGB)
     * \param grayscaleMode 0=off, 1=lightness, 2=luminosity, 3=average
     * \param saturation Saturation adjustment (-100 to 100)
     * \param colorizeOn Whether colorization is enabled
     * \param colorizeColor Color to use for colorization
     * \param colorizeStrength Colorization blend strength (0-100)
     */
    void setHueSaturationFilter( bool invert, int grayscaleMode, int saturation, bool colorizeOn, const QColor &colorizeColor, int colorizeStrength );

    /**
     * Set custom colormap for pseudocolor rendering.
     * The colormap is a 256-entry lookup table with RGBA values.
     * Each entry is 4 bytes: R, G, B, A (0-255).
     *
     * \param colormapData 256 * 4 = 1024 bytes of RGBA data
     * \param minValue Minimum value for normalization
     * \param maxValue Maximum value for normalization
     * \param interpolationType 0=Linear, 1=Discrete, 2=Exact
     */
    void setPseudocolorColormap( const QByteArray &colormapData, double minValue, double maxValue, int interpolationType = 0 );

    /**
     * Returns TRUE if pseudocolor mode is enabled (has custom colormap).
     */
    bool isPseudocolorMode() const { return mHasPseudocolorColormap; }

    /**
     * Set hillshade rendering parameters.
     * Enables hillshade mode which computes shaded relief from DEM data.
     *
     * The shader implements the Horn (1981) slope algorithm using a 3x3 window:
     *
     * - X derivative: ((z13+z23+z23+z33)-(z11+z21+z21+z31))/(8*cellX)
     * - Y derivative: ((z31+z32+z32+z33)-(z11+z12+z12+z13))/(8*cellY)
     *
     * \param azimuth Sun azimuth angle in degrees (0-360, 315=NW typical)
     * \param altitude Sun altitude angle in degrees (0-90, 45 typical)
     * \param zFactor Vertical exaggeration factor (default 1.0)
     * \param multiDirectional Use multi-directional shading (4 azimuths)
     */
    void setHillshadeParams( double azimuth, double altitude, double zFactor, bool multiDirectional );

    /**
     * Returns TRUE if hillshade mode is enabled.
     */
    bool isHillshadeMode() const { return mHillshadeMode; }

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
     * Initialize QRhi graphics pipeline and resources
     */
    bool initializeRhiResources( int width, int height );

    /**
     * Create or resize render target
     */
    bool ensureRenderTarget( int width, int height );

    /**
     * Release QRhi resources
     */
    void releaseRhiResources();

    QgsRasterGPUTileUploader *mTileUploader = nullptr;
    QRhi *mRhi = nullptr;

    // Render target resources
    std::unique_ptr<QRhiTexture> mRenderTexture;
    std::unique_ptr<QRhiTextureRenderTarget> mRenderTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> mRenderPassDesc;

    // Pipeline resources
    std::unique_ptr<QRhiGraphicsPipeline> mPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mSrbLayout; //!< SRB layout for pipeline compatibility
    std::unique_ptr<QRhiBuffer> mUniformBuffer;
    std::unique_ptr<QRhiBuffer> mVertexBuffer;
    std::unique_ptr<QRhiSampler> mSampler;

    //! Create per-tile SRB with specific texture binding
    QRhiShaderResourceBindings *createTileSrb( QRhiTexture *tileTexture );

    // Colormap texture (single-band mode)
    std::unique_ptr<QRhiTexture> mColormapTexture;
    bool mColormapUploaded = false; //!< Track if colormap has been uploaded to GPU

    //! Placeholder texture for SRB layout (must outlive mSrbLayout)
    std::unique_ptr<QRhiTexture> mPlaceholderTexture;

    float mOpacity = 1.0f;
    quint64 mFrameNumber = 0;

    // RGB mode (multi-band rendering)
    bool mRGBMode = false;
    int mRedBand = 1;
    int mGreenBand = 2;
    int mBlueBand = 3;

    // Single-band rendering
    int mInputBand = 1;

    // Contrast enhancement (from QGIS renderer styling)
    // Single-band mode
    double mMinValue = 0.0;
    double mMaxValue = 255.0;
    bool mInvertGradient = false;
    bool mHasContrastEnhancement = false;

    // RGB mode per-band enhancement
    double mRedMin = 0.0, mRedMax = 255.0;
    double mGreenMin = 0.0, mGreenMax = 255.0;
    double mBlueMin = 0.0, mBlueMax = 255.0;
    bool mHasRGBContrastEnhancement = false;

    // Contrast enhancement algorithm (matches QgsContrastEnhancement::ContrastEnhancementAlgorithm)
    // 0=NoEnhancement, 1=StretchToMinimumMaximum, 2=ClipToMinimumMaximum, 3=StretchAndClipToMinimumMaximum
    int mAlgorithm = 1; // Default: StretchToMinimumMaximum

    // Per-band NoData values (for RGB mode)
    double mNoDataR = 0.0;
    double mNoDataG = 0.0;
    double mNoDataB = 0.0;
    bool mHasPerBandNoData = false;
    double mNoDataTolerance = 0.5; // Default tolerance for integer data

    // Brightness/Contrast/Gamma filter (from QgsBrightnessContrastFilter)
    // Formula: contrastFactor = pow((contrast+100)/100, 2), gammaCorrection = 1/gamma
    int mBrightness = 0; // -255 to 255
    int mContrast = 0;   // -100 to 100
    double mGamma = 1.0; // 0.1 to 10.0
    bool mHasBrightnessFilter = false;

    // Hue/Saturation filter (from QgsHueSaturationFilter)
    // Order: Invert → Grayscale/Saturation → Colorize
    bool mInvertColors = false;
    int mGrayscaleMode = 0; // 0=off, 1=lightness, 2=luminosity, 3=average
    int mSaturation = 0;    // -100 to 100
    bool mColorizeOn = false;
    QColor mColorizeColor;
    int mColorizeStrength = 100; // 0 to 100
    bool mHasHueSatFilter = false;

    // Pseudocolor colormap (from QgsSingleBandPseudoColorRenderer)
    QByteArray mPseudocolorData; // 256 * 4 bytes RGBA
    double mPseudocolorMin = 0.0;
    double mPseudocolorMax = 255.0;
    int mPseudocolorInterpolationType = 0; // 0=Linear, 1=Discrete, 2=Exact
    bool mHasPseudocolorColormap = false;

    // Hillshade rendering (from QgsHillshadeRenderer)
    // Uses Horn (1981) algorithm with pre-computed trigonometry
    double mHillshadeAzimuth = 315.0;        // Sun azimuth (0-360, 315=NW)
    double mHillshadeAltitude = 45.0;        // Sun altitude (0-90)
    double mHillshadeZFactor = 1.0;          // Vertical exaggeration
    bool mHillshadeMultiDirectional = false; // Multi-directional shading
    bool mHillshadeMode = false;

    // Track if resources need recreation
    bool mPipelineNeedsRebuild = true;

#endif // HAVE_QRHI
};

#endif // QGSRASTERGPURENDERER_H
