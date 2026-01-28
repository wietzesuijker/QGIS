/***************************************************************************
  qgsrastergpushaders.h
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

#ifndef QGSRASTERGPUSHADERS_H
#define QGSRASTERGPUSHADERS_H

#define SIP_NO_FILE

#include "qgis_gui.h"

#include <QColor>
#include <QMap>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QString>

/**
 * \ingroup gui
 * \class QgsRasterGPUShaders
 * \brief Provides GLSL shaders for GPU-accelerated raster rendering.
 *
 * Manages shader programs for rendering different raster data types:
 *
 * - Single-band byte (R8) with color ramps
 * - Single-band uint16 (R16) with scaling
 * - Single-band float32 (R32F) with value mapping
 * - Multi-band RGBA (RGBA8) direct rendering
 *
 * Shaders support:
 *
 * - Color ramp application (linear interpolation)
 * - No-data value handling (transparency)
 * - Value scaling and normalization
 * - Alpha blending
 *
 * Inspired by deck.gl-raster's shader approach with WebGL texture sampling.
 *
 * \since QGIS 3.40
 */
class GUI_EXPORT QgsRasterGPUShaders
{
  public:
    /**
     * Shader variant for different raster data types
     */
    enum class ShaderType
    {
      Byte,         //!< 8-bit unsigned integer (R8)
      UInt16,       //!< 16-bit unsigned integer (R16)
      Float32,      //!< 32-bit float (R32F)
      RGB8,         //!< 8-bit RGB (RGB8) - multi-band color rendering
      RGBA8,        //!< 8-bit RGBA (RGBA8)
      BytePaletted, //!< 8-bit with color table lookup
    };

    /**
     * Color ramp stop for shader-based color mapping
     */
    struct ColorStop
    {
        float value;  //!< Data value (normalized 0-1)
        QColor color; //!< Color at this stop
    };

    /**
     * Shader configuration parameters
     */
    struct ShaderConfig
    {
        ShaderType type = ShaderType::Byte;
        QVector<ColorStop> colorRamp;
        float minValue = 0.0f;
        float maxValue = 255.0f;
        float noDataValue = -9999.0f;
        bool useNoData = false;
        float opacity = 1.0f;
    };

    QgsRasterGPUShaders() = default;

    /**
     * Get vertex shader source (shared by all raster shaders)
     */
    static QString vertexShaderSource();

    /**
     * Get fragment shader source for the specified shader type
     */
    static QString fragmentShaderSource( ShaderType type );

    /**
     * Create and compile a shader program for the given configuration
     * \returns nullptr if compilation fails
     */
    static QOpenGLShaderProgram *createShaderProgram( const ShaderConfig &config );

    /**
     * Update shader uniforms based on configuration
     */
    static void updateShaderUniforms( QOpenGLShaderProgram *program, const ShaderConfig &config );

    /**
     * Create a 1D colormap texture from color ramp (deck.gl pattern)
     * \returns OpenGL texture ID, or 0 on failure
     * \note Caller is responsible for deleting the texture
     */
    static GLuint createColormapTexture( const QVector<ColorStop> &colorRamp );

  private:
    static QString singleChannelShader( float scale );
};

#endif // QGSRASTERGPUSHADERS_H
