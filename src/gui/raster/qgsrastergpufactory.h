/***************************************************************************
  qgsrastergpufactory.h
  ---------------------
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

#ifndef QGSRASTERGPUFACTORY_H
#define QGSRASTERGPUFACTORY_H

#define SIP_NO_FILE

#include "qgis_gui.h"

#ifdef HAVE_QRHI
#include <rhi/qrhi.h>
#endif

/**
 * \ingroup gui
 * \class QgsRasterGPUFactory
 * \brief Factory for registering GPU-accelerated raster rendering.
 *
 * This class provides initialization for GPU raster rendering using Qt's
 * Rendering Hardware Interface (QRhi), enabling backend-agnostic GPU
 * acceleration across Vulkan, Metal, Direct3D 11, and OpenGL.
 *
 * Call initialize() during application startup to enable GPU rendering
 * for compatible layers. The factory automatically selects the best
 * available graphics backend for the platform.
 *
 * \note QGIS Server and other headless environments should not call initialize()
 * as no display context is available. In those cases, raster rendering falls
 * back to the standard CPU path automatically.
 *
 * \since QGIS 3.44
 */
class GUI_EXPORT QgsRasterGPUFactory
{
  public:
    /**
     * \brief Initialize GPU rendering support
     *
     * Registers the GPU renderer factory with QgsRasterLayerRenderer and
     * initializes the QRhi graphics context.
     *
     * Must be called during application initialization (GUI thread).
     *
     * Backend selection priority:
     *
     * - macOS: Metal
     * - Windows: Direct3D 11
     * - Linux: Vulkan (fallback to OpenGL)
     *
     * GPU rendering will be automatically used for:
     *
     * - GDAL raster layers (COG format recommended)
     * - Layers without reprojection (same CRS)
     * - When QRhi context is successfully initialized
     */
    static void initialize();

    /**
     * \brief Cleanup GPU rendering support
     *
     * Releases QRhi resources and unregisters the GPU renderer factory.
     * Called during application shutdown.
     */
    static void cleanup();

#ifdef HAVE_QRHI
    /**
     * \brief Get the QRhi instance
     * \returns QRhi instance, or nullptr if not initialized
     */
    static QRhi *rhi();
#endif

  private:
    QgsRasterGPUFactory() = delete;
};

#endif // QGSRASTERGPUFACTORY_H
