/***************************************************************************
  qgsrastergpushaders.cpp
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

#include "qgsrastergpushaders.h"

#include "qgslogger.h"

#include <QOpenGLContext>
#include <QOpenGLShader>

// Detect OpenGL version and return appropriate GLSL version
// macOS Metal provides OpenGL 2.1/4.1, Linux/Windows typically 3.3+
static bool supportsGLSL330()
{
  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if ( !ctx )
    return false;

  const auto version = ctx->format().version();
  // GLSL 330 requires OpenGL 3.3+
  return ( version.first > 3 ) || ( version.first == 3 && version.second >= 3 );
}

QString QgsRasterGPUShaders::vertexShaderSource()
{
  if ( supportsGLSL330() )
  {
    // OpenGL 3.3+ path
    return R"SHADER(
#version 330 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

uniform mat4 uMVPMatrix;

void main() {
  gl_Position = uMVPMatrix * vec4(aPosition, 0.0, 1.0);
  vTexCoord = aTexCoord;
}
)SHADER";
  }
  else
  {
    // OpenGL 2.1 / GLSL 1.20 path (macOS compatibility)
    return R"SHADER(
#version 120

attribute vec2 aPosition;
attribute vec2 aTexCoord;

varying vec2 vTexCoord;

uniform mat4 uMVPMatrix;

void main() {
  gl_Position = uMVPMatrix * vec4(aPosition, 0.0, 1.0);
  vTexCoord = aTexCoord;
}
)SHADER";
  }
}

QString QgsRasterGPUShaders::singleChannelShader( float scale )
{
  if ( supportsGLSL330() )
  {
    // OpenGL 3.3+ with 1D colormap texture lookup (deck.gl pattern)
    return QString( R"SHADER(
#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTileTexture;
uniform sampler2D uColormapTexture;
uniform float uMinValue;
uniform float uMaxValue;
uniform float uNoDataValue;
uniform bool uUseNoData;
uniform float uOpacity;

void main() {
  float value = texture(uTileTexture, vTexCoord).r * %1;

  if (uUseNoData && abs(value - uNoDataValue) < 0.5) {
    discard;
  }

  // Normalize to 0-1 for colormap lookup
  float normalized = clamp((value - uMinValue) / (uMaxValue - uMinValue), 0.0, 1.0);

  // 1D colormap texture lookup (deck.gl pattern)
  vec4 color = texture(uColormapTexture, vec2(normalized, 0.5));
  fragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER" )
      .arg( scale );
  }
  else
  {
    // OpenGL 2.1 / GLSL 1.20 path
    return QString( R"SHADER(
#version 120

varying vec2 vTexCoord;

uniform sampler2D uTileTexture;
uniform sampler2D uColormapTexture;
uniform float uMinValue;
uniform float uMaxValue;
uniform float uNoDataValue;
uniform bool uUseNoData;
uniform float uOpacity;

void main() {
  float value = texture2D(uTileTexture, vTexCoord).r * %1;

  if (uUseNoData && abs(value - uNoDataValue) < 0.5) {
    discard;
  }

  // Normalize to 0-1 for colormap lookup
  float normalized = clamp((value - uMinValue) / (uMaxValue - uMinValue), 0.0, 1.0);

  // 1D colormap texture lookup (deck.gl pattern)
  vec4 color = texture2D(uColormapTexture, vec2(normalized, 0.5));
  gl_FragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER" )
      .arg( scale );
  }
}

QString QgsRasterGPUShaders::fragmentShaderSource( ShaderType type )
{
  switch ( type )
  {
    case ShaderType::Byte:
      return singleChannelShader( 255.0 );

    case ShaderType::UInt16:
      return singleChannelShader( 65535.0 );

    case ShaderType::Float32:
      return singleChannelShader( 1.0 );

    case ShaderType::RGB8:
      if ( supportsGLSL330() )
      {
        return R"SHADER(
#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTileTexture;
uniform float uOpacity;

void main() {
  vec3 color = texture(uTileTexture, vTexCoord).rgb;
  fragColor = vec4(color, uOpacity);
}
)SHADER";
      }
      else
      {
        return R"SHADER(
#version 120

varying vec2 vTexCoord;

uniform sampler2D uTileTexture;
uniform float uOpacity;

void main() {
  vec3 color = texture2D(uTileTexture, vTexCoord).rgb;
  gl_FragColor = vec4(color, uOpacity);
}
)SHADER";
      }

    case ShaderType::RGBA8:
      if ( supportsGLSL330() )
      {
        return R"SHADER(
#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTileTexture;
uniform float uOpacity;

void main() {
  vec4 color = texture(uTileTexture, vTexCoord);
  fragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER";
      }
      else
      {
        return R"SHADER(
#version 120

varying vec2 vTexCoord;

uniform sampler2D uTileTexture;
uniform float uOpacity;

void main() {
  vec4 color = texture2D(uTileTexture, vTexCoord);
  gl_FragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER";
      }

    case ShaderType::BytePaletted:
      if ( supportsGLSL330() )
      {
        return R"SHADER(
#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uTileTexture;
uniform sampler2D uPaletteTexture;
uniform float uOpacity;

void main() {
  float index = texture(uTileTexture, vTexCoord).r;
  vec4 color = texture(uPaletteTexture, vec2(index, 0.5));
  fragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER";
      }
      else
      {
        return R"SHADER(
#version 120

varying vec2 vTexCoord;

uniform sampler2D uTileTexture;
uniform sampler2D uPaletteTexture;
uniform float uOpacity;

void main() {
  float index = texture2D(uTileTexture, vTexCoord).r;
  vec4 color = texture2D(uPaletteTexture, vec2(index, 0.5));
  gl_FragColor = vec4(color.rgb, color.a * uOpacity);
}
)SHADER";
      }
  }

  return QString();
}

QOpenGLShaderProgram *QgsRasterGPUShaders::createShaderProgram( const ShaderConfig &config )
{
  QOpenGLShaderProgram *program = new QOpenGLShaderProgram();

  const QString vertexSource = vertexShaderSource();
  const QString fragmentSource = fragmentShaderSource( config.type );

  QgsDebugMsgLevel( QStringLiteral( "Compiling vertex shader (GLSL %1)" ).arg( supportsGLSL330() ? "330" : "120" ), 3 );

  // Add vertex shader
  if ( !program->addShaderFromSourceCode( QOpenGLShader::Vertex, vertexSource ) )
  {
    QgsDebugError( QStringLiteral( "Vertex shader compilation failed: %1" ).arg( program->log() ) );
    delete program;
    return nullptr;
  }

  // Add fragment shader
  if ( !program->addShaderFromSourceCode( QOpenGLShader::Fragment, fragmentSource ) )
  {
    QgsDebugError( QStringLiteral( "Fragment shader compilation failed: %1" ).arg( program->log() ) );
    delete program;
    return nullptr;
  }

  // For GLSL 1.20, bind attribute locations manually (no layout qualifiers)
  if ( !supportsGLSL330() )
  {
    program->bindAttributeLocation( "aPosition", 0 );
    program->bindAttributeLocation( "aTexCoord", 1 );
  }

  // Link program
  if ( !program->link() )
  {
    QgsDebugError( QStringLiteral( "Shader program linking failed: %1" ).arg( program->log() ) );
    delete program;
    return nullptr;
  }

  QgsDebugMsgLevel( QStringLiteral( "GPU shader program created successfully (GLSL %1)" ).arg( supportsGLSL330() ? "330" : "120" ), 2 );
  return program;
}

void QgsRasterGPUShaders::updateShaderUniforms( QOpenGLShaderProgram *program, const ShaderConfig &config )
{
  if ( !program || !program->isLinked() )
    return;

  program->bind();

  // Set common uniforms
  program->setUniformValue( "uOpacity", config.opacity );

  // Type-specific uniforms
  if ( config.type != ShaderType::RGB8 && config.type != ShaderType::RGBA8 && config.type != ShaderType::BytePaletted )
  {
    program->setUniformValue( "uMinValue", config.minValue );
    program->setUniformValue( "uMaxValue", config.maxValue );
    program->setUniformValue( "uNoDataValue", config.noDataValue );
    program->setUniformValue( "uUseNoData", config.useNoData );
  }

  program->release();
}

GLuint QgsRasterGPUShaders::createColormapTexture( const QVector<ColorStop> &colorRamp )
{
  if ( colorRamp.isEmpty() )
    return 0;

  // Create 256-wide 1D colormap texture (as 256x1 2D texture for compatibility)
  constexpr int COLORMAP_WIDTH = 256;
  std::vector<unsigned char> pixels( COLORMAP_WIDTH * 4 ); // RGBA

  // Interpolate color ramp to fill texture
  for ( int i = 0; i < COLORMAP_WIDTH; ++i )
  {
    const float t = static_cast<float>( i ) / ( COLORMAP_WIDTH - 1 );

    // Find surrounding color stops
    QColor color;
    if ( colorRamp.size() == 1 )
    {
      color = colorRamp[0].color;
    }
    else
    {
      // Find interval
      int idx = 0;
      for ( int j = 0; j < colorRamp.size() - 1; ++j )
      {
        if ( t >= colorRamp[j].value && t <= colorRamp[j + 1].value )
        {
          idx = j;
          break;
        }
        if ( t > colorRamp[j + 1].value )
          idx = j + 1;
      }

      if ( idx >= colorRamp.size() - 1 )
      {
        color = colorRamp.last().color;
      }
      else
      {
        const float v0 = colorRamp[idx].value;
        const float v1 = colorRamp[idx + 1].value;
        const float localT = ( v1 > v0 ) ? ( t - v0 ) / ( v1 - v0 ) : 0.0f;

        const QColor &c0 = colorRamp[idx].color;
        const QColor &c1 = colorRamp[idx + 1].color;

        color = QColor::fromRgbF(
          c0.redF() + localT * ( c1.redF() - c0.redF() ),
          c0.greenF() + localT * ( c1.greenF() - c0.greenF() ),
          c0.blueF() + localT * ( c1.blueF() - c0.blueF() ),
          c0.alphaF() + localT * ( c1.alphaF() - c0.alphaF() )
        );
      }
    }

    pixels[i * 4 + 0] = static_cast<unsigned char>( color.red() );
    pixels[i * 4 + 1] = static_cast<unsigned char>( color.green() );
    pixels[i * 4 + 2] = static_cast<unsigned char>( color.blue() );
    pixels[i * 4 + 3] = static_cast<unsigned char>( color.alpha() );
  }

  // Create OpenGL texture
  GLuint textureId = 0;
  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if ( !ctx )
    return 0;

  QOpenGLFunctions *gl = ctx->functions();
  gl->glGenTextures( 1, &textureId );
  gl->glBindTexture( GL_TEXTURE_2D, textureId );

  gl->glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
  gl->glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
  gl->glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
  gl->glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

  gl->glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, COLORMAP_WIDTH, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );

  gl->glBindTexture( GL_TEXTURE_2D, 0 );

  QgsDebugMsgLevel( QStringLiteral( "Created colormap texture ID %1" ).arg( textureId ), 3 );

  return textureId;
}
