/***************************************************************************
  qgsrastergputileuploader.cpp - GPU tile uploader for rasters (QRhi)
  --------------------------------------
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

#include "qgsrastergputileuploader.h"

#include "qgslogger.h"

#include <limits>

#include <QMutexLocker>
#include <QString>

using namespace Qt::StringLiterals;

#ifdef HAVE_QRHI

QgsRasterGPUTileUploader::QgsRasterGPUTileUploader( QgsRasterTileReader *reader, QRhi *rhi )
  : mRhi( rhi )
  , mReader( reader )
  , mOwnerThread( QThread::currentThread() )
{
}

QgsRasterGPUTileUploader::~QgsRasterGPUTileUploader()
{
  clearCache();
}

quint64 QgsRasterGPUTileUploader::makeTileKey( int overview, int tileX, int tileY, int band )
{
  // Pack into 64-bit key: [16-bit overview][16-bit band][16-bit Y][16-bit X]
  return ( static_cast<quint64>( overview & 0xFFFF ) << 48 ) | ( static_cast<quint64>( band & 0xFFFF ) << 32 ) | ( static_cast<quint64>( tileY & 0xFFFF ) << 16 ) | ( static_cast<quint64>( tileX & 0xFFFF ) );
}

quint64 QgsRasterGPUTileUploader::makeRGBTileKey( int overview, int tileX, int tileY, int redBand, int greenBand, int blueBand )
{
  // Pack into 64-bit key with RGB marker (0xFF in high byte to distinguish from single-band)
  // Layout: [8-bit marker][4-bit overview][4-bit R][4-bit G][4-bit B][20-bit Y][20-bit X]
  // Supports: 16 overview levels, 16 bands, ~1M tiles per axis
  return ( static_cast<quint64>( 0xFF ) << 56 ) | ( static_cast<quint64>( overview & 0xF ) << 52 ) | ( static_cast<quint64>( redBand & 0xF ) << 48 ) | ( static_cast<quint64>( greenBand & 0xF ) << 44 ) | ( static_cast<quint64>( blueBand & 0xF ) << 40 ) | ( static_cast<quint64>( tileY & 0xFFFFF ) << 20 ) | ( static_cast<quint64>( tileX & 0xFFFFF ) );
}

QRhiTexture::Format QgsRasterGPUTileUploader::textureFormat( Qgis::DataType dataType, int bandCount ) const
{
  // Map GDAL data types to QRhi texture formats
  switch ( dataType )
  {
    case Qgis::DataType::Byte:
      switch ( bandCount )
      {
        case 1:
          return QRhiTexture::R8;
        case 3:
          return QRhiTexture::RGBA8; // RGB stored as RGBA
        case 4:
          return QRhiTexture::RGBA8;
        default:
          return QRhiTexture::RGBA8;
      }

    case Qgis::DataType::UInt16:
    case Qgis::DataType::Int16:
      return QRhiTexture::R16; // 16-bit single channel

    case Qgis::DataType::Float32:
      return QRhiTexture::R32F; // 32-bit float single channel

    default:
      // Fall back to RGBA8 for other types
      return QRhiTexture::RGBA8;
  }
}

QgsRasterGPUTileUploader::GPUTile QgsRasterGPUTileUploader::createTile( int overviewLevel, int tileX, int tileY, int bandNumber )
{
  GPUTile result;

  // Thread safety: QRhi resources can only be accessed from the owner thread
  if ( QThread::currentThread() != mOwnerThread )
  {
    QgsDebugError( QStringLiteral( "createTile called from wrong thread" ) );
    return result;
  }

  if ( !mReader || !mReader->isValid() || !mRhi )
  {
    QgsDebugError( u"GPU uploader not initialized"_s );
    return result;
  }

  // Read tile data using fast COG reader
  if ( !mReader->readTile( overviewLevel, tileX, tileY, bandNumber, mTileBuffer ) )
  {
    QgsDebugError( u"Failed to read tile %1,%2 overview %3"_s
                     .arg( tileX )
                     .arg( tileY )
                     .arg( overviewLevel ) );
    return result;
  }

  const auto tileInfo = mReader->tileInfo( overviewLevel );

  // Determine texture format
  const QRhiTexture::Format format = textureFormat(
    static_cast<Qgis::DataType>( tileInfo.dataType ),
    1 // single band
  );

  // Create QRhi texture
  QRhiTexture *texture = mRhi->newTexture(
    format,
    QSize( tileInfo.width, tileInfo.height ),
    1,                                // sample count
    QRhiTexture::UsedAsTransferSource // Needed for potential readback
  );

  if ( !texture->create() )
  {
    QgsDebugError( u"Failed to create QRhi texture"_s );
    delete texture;
    return result;
  }

  // Fill result - data will be uploaded via collectPendingUploads()
  result.texture = texture;
  result.width = tileInfo.width;
  result.height = tileInfo.height;
  result.lastUsedFrame = 0;
  result.isValid = true;
  result.needsUpload = true;
  result.data = mTileBuffer; // Copy data for deferred upload

  return result;
}

QgsRasterGPUTileUploader::GPUTile QgsRasterGPUTileUploader::createRGBTile( int overviewLevel, int tileX, int tileY, int redBand, int greenBand, int blueBand )
{
  GPUTile result;

  // Thread safety: QRhi resources can only be accessed from the owner thread
  if ( QThread::currentThread() != mOwnerThread )
  {
    QgsDebugError( QStringLiteral( "createRGBTile called from wrong thread" ) );
    return result;
  }

  if ( !mReader || !mReader->isValid() || !mRhi )
  {
    QgsDebugError( u"GPU uploader not initialized"_s );
    return result;
  }

  // Read multi-band tile data (interleaved RGB)
  const QList<int> bands = { redBand, greenBand, blueBand };
  if ( !mReader->readTileMultiBand( overviewLevel, tileX, tileY, bands, mTileBuffer ) )
  {
    QgsDebugError( u"Failed to read RGB tile %1,%2 overview %3"_s
                     .arg( tileX )
                     .arg( tileY )
                     .arg( overviewLevel ) );
    return result;
  }

  const auto tileInfo = mReader->tileInfo( overviewLevel );

  // Create QRhi texture (RGB as RGBA)
  QRhiTexture *texture = mRhi->newTexture(
    QRhiTexture::RGBA8,
    QSize( tileInfo.width, tileInfo.height ),
    1,
    QRhiTexture::UsedAsTransferSource
  );

  if ( !texture->create() )
  {
    QgsDebugError( u"Failed to create QRhi RGB texture"_s );
    delete texture;
    return result;
  }

  // Convert RGB to RGBA (QRhi requires RGBA)
  const int pixelCount = tileInfo.width * tileInfo.height;
  QByteArray rgbaData( pixelCount * 4, Qt::Uninitialized );
  const char *src = mTileBuffer.constData();
  char *dst = rgbaData.data();

  for ( int i = 0; i < pixelCount; ++i )
  {
    dst[i * 4 + 0] = src[i * 3 + 0];           // R
    dst[i * 4 + 1] = src[i * 3 + 1];           // G
    dst[i * 4 + 2] = src[i * 3 + 2];           // B
    dst[i * 4 + 3] = static_cast<char>( 255 ); // A
  }

  // Fill result
  result.texture = texture;
  result.width = tileInfo.width;
  result.height = tileInfo.height;
  result.lastUsedFrame = 0;
  result.isValid = true;
  result.needsUpload = true;
  result.data = std::move( rgbaData );

  return result;
}

QgsRasterGPUTileUploader::GPUTile QgsRasterGPUTileUploader::getTile(
  int overviewLevel, int tileX, int tileY, int bandNumber, quint64 frameNumber
)
{
  QMutexLocker locker( &mMutex );

  const quint64 key = makeTileKey( overviewLevel, tileX, tileY, bandNumber );

  // Check cache
  if ( mTileCache.contains( key ) )
  {
    GPUTile &tile = mTileCache[key];
    tile.lastUsedFrame = frameNumber;
    return tile;
  }

  // Create new tile
  GPUTile tile = createTile( overviewLevel, tileX, tileY, bandNumber );
  if ( tile.isValid )
  {
    tile.lastUsedFrame = frameNumber;
    mTileCache[key] = tile;
    if ( tile.needsUpload )
    {
      mPendingUploads.append( key );
    }
  }

  return tile;
}

QVector<QgsRasterGPUTileUploader::GPUTile> QgsRasterGPUTileUploader::getTiles(
  const QVector<TileCoord> &coords, int bandNumber, quint64 frameNumber
)
{
  QMutexLocker locker( &mMutex );

  QVector<GPUTile> results;
  results.reserve( coords.size() );

  for ( const TileCoord &coord : coords )
  {
    const quint64 key = makeTileKey( coord.level, coord.x, coord.y, bandNumber );

    // Check cache first
    if ( mTileCache.contains( key ) )
    {
      GPUTile &tile = mTileCache[key];
      tile.lastUsedFrame = frameNumber;
      results.append( tile );
      continue;
    }

    // Create new tile
    GPUTile tile = createTile( coord.level, coord.x, coord.y, bandNumber );
    if ( tile.isValid )
    {
      tile.lastUsedFrame = frameNumber;
      mTileCache[key] = tile;
      if ( tile.needsUpload )
      {
        mPendingUploads.append( key );
      }
    }
    results.append( tile );
  }

  return results;
}

QgsRasterGPUTileUploader::GPUTile QgsRasterGPUTileUploader::getRGBTile(
  int overviewLevel, int tileX, int tileY, int redBand, int greenBand, int blueBand, quint64 frameNumber
)
{
  QMutexLocker locker( &mMutex );

  const quint64 key = makeRGBTileKey( overviewLevel, tileX, tileY, redBand, greenBand, blueBand );

  // Check cache
  if ( mTileCache.contains( key ) )
  {
    GPUTile &tile = mTileCache[key];
    tile.lastUsedFrame = frameNumber;
    return tile;
  }

  // Create new RGB tile
  GPUTile tile = createRGBTile( overviewLevel, tileX, tileY, redBand, greenBand, blueBand );
  if ( tile.isValid )
  {
    tile.lastUsedFrame = frameNumber;
    mTileCache[key] = tile;
    if ( tile.needsUpload )
    {
      mPendingUploads.append( key );
    }
  }

  return tile;
}

int QgsRasterGPUTileUploader::collectPendingUploads( QRhiResourceUpdateBatch *batch )
{
  QMutexLocker locker( &mMutex );

  if ( !batch || mPendingUploads.isEmpty() )
    return 0;

  int count = 0;

  for ( const quint64 key : mPendingUploads )
  {
    if ( !mTileCache.contains( key ) )
      continue;

    GPUTile &tile = mTileCache[key];
    if ( !tile.needsUpload || !tile.texture || tile.data.isEmpty() )
      continue;

    // Queue texture upload
    QRhiTextureSubresourceUploadDescription subresDesc( tile.data.constData(), tile.data.size() );
    QRhiTextureUploadEntry entry( 0, 0, subresDesc );
    QRhiTextureUploadDescription uploadDesc( entry );
    batch->uploadTexture( tile.texture, uploadDesc );

    // Clear pending data
    tile.needsUpload = false;
    tile.data.clear();
    ++count;
  }

  mPendingUploads.clear();

  if ( count > 0 )
  {
    QgsDebugMsgLevel( u"Queued %1 texture uploads"_s.arg( count ), 3 );
  }

  return count;
}

void QgsRasterGPUTileUploader::clearCache()
{
  QMutexLocker locker( &mMutex );

  // Delete all textures
  for ( auto it = mTileCache.begin(); it != mTileCache.end(); ++it )
  {
    if ( it.value().texture )
    {
      delete it.value().texture;
    }
  }

  mTileCache.clear();
  mPendingUploads.clear();

  QgsDebugMsgLevel( u"GPU tile cache cleared"_s, 2 );
}

void QgsRasterGPUTileUploader::evictOldTiles( quint64 currentFrame, int maxAge, qint64 maxMemoryBytes )
{
  QMutexLocker locker( &mMutex );

  int evictedCount = 0;

  // Phase 1: Remove tiles older than maxAge frames
  QList<quint64> toRemove;
  for ( auto it = mTileCache.begin(); it != mTileCache.end(); ++it )
  {
    const quint64 age = currentFrame - it.value().lastUsedFrame;
    if ( age > static_cast<quint64>( maxAge ) )
    {
      toRemove.append( it.key() );
    }
  }

  for ( const quint64 key : toRemove )
  {
    GPUTile &tile = mTileCache[key];
    if ( tile.texture )
    {
      delete tile.texture;
    }
    mTileCache.remove( key );
    ++evictedCount;
  }

  // Phase 2: If over memory budget, evict oldest tiles until under budget
  if ( maxMemoryBytes > 0 )
  {
    // Calculate current memory usage
    qint64 currentMemory = 0;
    for ( auto it = mTileCache.begin(); it != mTileCache.end(); ++it )
    {
      const GPUTile &tile = it.value();
      if ( tile.isValid && tile.texture )
      {
        int bytesPerPixel = 4;
        switch ( tile.texture->format() )
        {
          case QRhiTexture::R8:
            bytesPerPixel = 1;
            break;
          case QRhiTexture::R16:
            bytesPerPixel = 2;
            break;
          case QRhiTexture::R32F:
            bytesPerPixel = 4;
            break;
          default:
            bytesPerPixel = 4;
        }
        currentMemory += tile.width * tile.height * bytesPerPixel;
      }
    }

    // Evict oldest tiles if over budget
    while ( currentMemory > maxMemoryBytes && !mTileCache.isEmpty() )
    {
      // Find oldest tile
      quint64 oldestKey = 0;
      quint64 oldestFrame = std::numeric_limits<quint64>::max();

      for ( auto it = mTileCache.begin(); it != mTileCache.end(); ++it )
      {
        if ( it.value().lastUsedFrame < oldestFrame )
        {
          oldestFrame = it.value().lastUsedFrame;
          oldestKey = it.key();
        }
      }

      // Remove oldest tile
      GPUTile &tile = mTileCache[oldestKey];
      if ( tile.texture )
      {
        int bytesPerPixel = 4;
        switch ( tile.texture->format() )
        {
          case QRhiTexture::R8:
            bytesPerPixel = 1;
            break;
          case QRhiTexture::R16:
            bytesPerPixel = 2;
            break;
          case QRhiTexture::R32F:
            bytesPerPixel = 4;
            break;
          default:
            bytesPerPixel = 4;
        }
        currentMemory -= tile.width * tile.height * bytesPerPixel;
        delete tile.texture;
      }
      mTileCache.remove( oldestKey );
      ++evictedCount;
    }
  }

  if ( evictedCount > 0 )
  {
    QgsDebugMsgLevel( u"Evicted %1 tiles from GPU cache (%.1f MB remaining)"_s.arg( evictedCount ).arg( static_cast<double>( maxMemoryBytes > 0 ? maxMemoryBytes : 0 ) / ( 1024.0 * 1024.0 ) ), 2 );
  }
}

void QgsRasterGPUTileUploader::getCacheStats( int &cachedCount, qint64 &memoryBytes ) const
{
  QMutexLocker locker( &mMutex );

  cachedCount = mTileCache.size();
  memoryBytes = 0;

  for ( auto it = mTileCache.begin(); it != mTileCache.end(); ++it )
  {
    const GPUTile &tile = it.value();
    if ( tile.isValid && tile.texture )
    {
      // Approximate memory based on texture format
      int bytesPerPixel = 4; // Default RGBA8
      switch ( tile.texture->format() )
      {
        case QRhiTexture::R8:
          bytesPerPixel = 1;
          break;
        case QRhiTexture::R16:
          bytesPerPixel = 2;
          break;
        case QRhiTexture::R32F:
          bytesPerPixel = 4;
          break;
        case QRhiTexture::RGBA8:
          bytesPerPixel = 4;
          break;
        default:
          bytesPerPixel = 4;
      }
      memoryBytes += tile.width * tile.height * bytesPerPixel;
    }
  }
}

#endif // HAVE_QRHI

// Non-QRhi-specific functions (always available)

QgsRasterTileReader::TileInfo QgsRasterGPUTileUploader::tileInfo( int overviewLevel ) const
{
  if ( mReader && mReader->isValid() )
  {
    return mReader->tileInfo( overviewLevel );
  }
  return QgsRasterTileReader::TileInfo();
}

QgsRectangle QgsRasterGPUTileUploader::rasterExtent() const
{
  if ( mReader && mReader->isValid() )
  {
    return mReader->extent();
  }
  return QgsRectangle();
}

int QgsRasterGPUTileUploader::selectBestOverview( double targetMupp ) const
{
  if ( mReader && mReader->isValid() )
  {
    return mReader->selectBestOverview( targetMupp );
  }
  return 0;
}

QgsRectangle QgsRasterGPUTileUploader::tileExtent( int overviewLevel, int tileX, int tileY ) const
{
  if ( !mReader || !mReader->isValid() )
  {
    return QgsRectangle();
  }

  // Get tile info
  const auto info = mReader->tileInfo( overviewLevel );
  if ( !info.isTiled )
  {
    return QgsRectangle();
  }

  // Get full raster extent
  const QgsRectangle extent = mReader->extent();

  // Calculate tile size in georeferenced units
  const double tileWidth = extent.width() / info.tilesX;
  const double tileHeight = extent.height() / info.tilesY;

  // Calculate tile extent
  // Note: Y axis is inverted (tile 0 is at top)
  const double minX = extent.xMinimum() + ( tileX * tileWidth );
  const double maxX = minX + tileWidth;
  const double maxY = extent.yMaximum() - ( tileY * tileHeight );
  const double minY = maxY - tileHeight;

  return QgsRectangle( minX, minY, maxX, maxY );
}
