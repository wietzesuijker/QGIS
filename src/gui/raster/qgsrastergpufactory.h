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

/**
 * \ingroup gui
 * \class QgsRasterGPUFactory
 * \brief Factory for registering GPU-accelerated raster rendering.
 *
 * This class provides initialization for GPU raster rendering by registering
 * a factory function with the core rendering system. Call initialize() during
 * application startup to enable GPU rendering for compatible layers.
 *
 * \note This requires a valid OpenGL context and is intended for desktop QGIS only.
 * QGIS Server and other headless environments should not call initialize() as no
 * OpenGL context is available. In those cases, raster rendering falls back to
 * the standard CPU path automatically.
 *
 * \since QGIS 3.40
 */
class GUI_EXPORT QgsRasterGPUFactory
{
  public:
    /**
     * \brief Initialize GPU rendering support
     *
     * Registers the GPU renderer factory with QgsRasterLayerRenderer.
     * Must be called during application initialization (GUI thread).
     *
     * GPU rendering will be automatically used for:
     *
     * - GDAL raster layers (COG format recommended)
     * - Layers without reprojection (same CRS)
     * - When OpenGL context is available
     */
    static void initialize();

    /**
     * \brief Cleanup GPU rendering support
     *
     * Unregisters the GPU renderer factory. Called during application shutdown.
     */
    static void cleanup();

  private:
    QgsRasterGPUFactory() = delete;
};

#endif // QGSRASTERGPUFACTORY_H
