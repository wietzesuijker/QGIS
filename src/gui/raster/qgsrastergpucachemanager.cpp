/***************************************************************************
  qgsrastergpucachemanager.cpp - Persistent GPU resource cache
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

#include "qgsrastergpucachemanager.h"

#include <gdal.h>

#include "qgslogger.h"
#include "qgsrastergpufactory.h"
#include "qgsrastergputileuploader.h"
#include "qgsrastertilereader.h"

#include <QMutexLocker>

QgsRasterGPUCacheManager *QgsRasterGPUCacheManager::sInstance = nullptr;

QgsRasterGPUCacheManager::CacheEntry::~CacheEntry()
{
  // Release GPU resources first (must happen before GDAL close)
  uploader.reset();
  reader.reset();

  // Close GDAL dataset (we own it)
  if ( gdalDataset )
  {
    GDALClose( gdalDataset );
    gdalDataset = nullptr;
  }
}

QgsRasterGPUCacheManager *QgsRasterGPUCacheManager::instance()
{
  // Thread-safe singleton initialization using static local (C++11 magic statics)
  static QgsRasterGPUCacheManager instance;
  sInstance = &instance;
  return sInstance;
}

void QgsRasterGPUCacheManager::cleanup()
{
  // With static local singleton, we don't delete - just clear resources
  // The static instance is destroyed automatically at program exit
  if ( sInstance )
  {
    sInstance->clearAll();
    sInstance = nullptr;
    QgsDebugMsgLevel( QStringLiteral( "GPU cache manager: singleton cleaned up" ), 2 );
  }
}

QgsRasterGPUCacheManager::~QgsRasterGPUCacheManager()
{
  clearAll();
}

QgsRasterGPUTileUploader *QgsRasterGPUCacheManager::getOrCreateUploader( const QString &dataSourceUri )
{
  QMutexLocker locker( &mMutex );

  // Check cache first
  auto it = mCache.find( dataSourceUri );
  if ( it != mCache.end() )
  {
    return it->second->uploader.get();
  }

#ifdef HAVE_QRHI
  // Check for QRhi context
  QRhi *rhi = QgsRasterGPUFactory::rhi();
  if ( !rhi )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: no QRhi context available" ), 4 );
    return nullptr;
  }

  // Open GDAL dataset
  // Note: This opens a separate handle from the one used by QgsRasterDataProvider.
  // While this duplicates the file handle, it's necessary because:
  // 1. The cache needs a persistent handle that outlives individual render calls
  // 2. QgsRasterDataProvider doesn't expose its GDALDatasetH (abstraction layer)
  // 3. Sharing would require complex lifecycle management between core/gui
  // Future optimization: hook into layer lifecycle to share handles.
  GDALDatasetH gdalDataset = GDALOpen( dataSourceUri.toUtf8().constData(), GA_ReadOnly );
  if ( !gdalDataset )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: cannot open GDAL dataset: %1" ).arg( dataSourceUri ), 4 );
    return nullptr;
  }

  // Create tile reader (does NOT take ownership of dataset)
  auto reader = std::make_unique<QgsRasterTileReader>( gdalDataset );

  if ( !reader->isValid() )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: tile reader initialization failed" ), 4 );
    GDALClose( gdalDataset );
    return nullptr;
  }

  // Check if dataset is tiled
  const auto tileInfo = reader->tileInfo( 0 );

  // Zarr datasets are inherently chunked even if isTiled heuristic fails
  bool isZarrDataset = false;
  GDALDriverH driver = GDALGetDatasetDriver( gdalDataset );
  if ( driver )
  {
    const char *driverName = GDALGetDriverShortName( driver );
    isZarrDataset = driverName && ( strcmp( driverName, "Zarr" ) == 0 );
  }

  if ( !tileInfo.isTiled && !isZarrDataset )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: dataset is not tiled" ), 4 );
    GDALClose( gdalDataset );
    return nullptr;
  }

  if ( isZarrDataset )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: Zarr dataset detected" ), 3 );
  }

  // Create uploader with QRhi
  auto uploader = std::make_unique<QgsRasterGPUTileUploader>( reader.get(), rhi );

  // Store in cache (entry owns the GDAL dataset)
  auto entry = std::make_unique<CacheEntry>();
  entry->gdalDataset = gdalDataset;
  entry->reader = std::move( reader );
  entry->uploader = std::move( uploader );

  QgsRasterGPUTileUploader *result = entry->uploader.get();
  mCache.emplace( dataSourceUri, std::move( entry ) );

  QgsDebugMsgLevel( QStringLiteral( "GPU cache: created uploader for %1" ).arg( dataSourceUri ), 2 );

  return result;
#else
  Q_UNUSED( dataSourceUri )
  return nullptr;
#endif
}

void QgsRasterGPUCacheManager::removeUploader( const QString &dataSourceUri )
{
  QMutexLocker locker( &mMutex );

  auto it = mCache.find( dataSourceUri );
  if ( it != mCache.end() )
  {
#ifdef HAVE_QRHI
    // Clear GPU resources before removing
    if ( it->second->uploader )
    {
      it->second->uploader->clearCache();
    }
#endif
    mCache.erase( it );
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: removed uploader for %1" ).arg( dataSourceUri ), 2 );
  }
}

void QgsRasterGPUCacheManager::clearAll()
{
  QMutexLocker locker( &mMutex );

#ifdef HAVE_QRHI
  // Clear GPU resources for each entry
  for ( auto it = mCache.begin(); it != mCache.end(); ++it )
  {
    if ( it->second->uploader )
    {
      it->second->uploader->clearCache();
    }
  }
#endif

  mCache.clear();
  QgsDebugMsgLevel( QStringLiteral( "GPU cache: cleared all uploaders" ), 2 );
}

int QgsRasterGPUCacheManager::cacheSize() const
{
  QMutexLocker locker( &mMutex );
  return static_cast<int>( mCache.size() );
}

#ifdef HAVE_QRHI
qint64 QgsRasterGPUCacheManager::totalGPUMemoryUsed() const
{
  QMutexLocker locker( &mMutex );

  qint64 total = 0;
  for ( auto it = mCache.begin(); it != mCache.end(); ++it )
  {
    if ( it->second->uploader )
    {
      int count;
      qint64 bytes;
      it->second->uploader->getCacheStats( count, bytes );
      total += bytes;
    }
  }
  return total;
}
#endif
