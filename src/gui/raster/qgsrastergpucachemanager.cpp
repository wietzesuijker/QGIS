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
#include "qgsrastergputileuploader.h"
#include "qgsrastertilereader.h"

#include <QMutexLocker>
#include <QOpenGLContext>

QgsRasterGPUCacheManager *QgsRasterGPUCacheManager::sInstance = nullptr;

QgsRasterGPUCacheManager *QgsRasterGPUCacheManager::instance()
{
  if ( !sInstance )
  {
    sInstance = new QgsRasterGPUCacheManager();
  }
  return sInstance;
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

  // Check for OpenGL context
  if ( !QOpenGLContext::currentContext() )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: no OpenGL context available" ), 4 );
    return nullptr;
  }

  // Open GDAL dataset
  GDALDatasetH gdalDataset = GDALOpen( dataSourceUri.toUtf8().constData(), GA_ReadOnly );
  if ( !gdalDataset )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: cannot open GDAL dataset: %1" ).arg( dataSourceUri ), 4 );
    return nullptr;
  }

  // Create tile reader (takes ownership of dataset)
  auto reader = std::make_unique<QgsRasterTileReader>( gdalDataset );

  if ( !reader->isValid() )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: tile reader initialization failed" ), 4 );
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
    return nullptr;
  }

  if ( isZarrDataset )
  {
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: Zarr dataset detected" ), 3 );
  }

  // Create uploader
  auto uploader = std::make_unique<QgsRasterGPUTileUploader>( reader.get() );

  // Store in cache
  auto entry = std::make_unique<CacheEntry>();
  entry->reader = std::move( reader );
  entry->uploader = std::move( uploader );

  QgsRasterGPUTileUploader *result = entry->uploader.get();
  mCache.emplace( dataSourceUri, std::move( entry ) );

  QgsDebugMsgLevel( QStringLiteral( "GPU cache: created uploader for %1" ).arg( dataSourceUri ), 2 );

  return result;
}

void QgsRasterGPUCacheManager::removeUploader( const QString &dataSourceUri )
{
  QMutexLocker locker( &mMutex );

  auto it = mCache.find( dataSourceUri );
  if ( it != mCache.end() )
  {
    // Clear GPU resources before removing
    if ( it->second->uploader )
    {
      it->second->uploader->clearCache();
    }
    mCache.erase( it );
    QgsDebugMsgLevel( QStringLiteral( "GPU cache: removed uploader for %1" ).arg( dataSourceUri ), 2 );
  }
}

void QgsRasterGPUCacheManager::clearAll()
{
  QMutexLocker locker( &mMutex );

  // Clear GPU resources for each entry
  for ( auto it = mCache.begin(); it != mCache.end(); ++it )
  {
    if ( it->second->uploader )
    {
      it->second->uploader->clearCache();
    }
  }

  mCache.clear();
  QgsDebugMsgLevel( QStringLiteral( "GPU cache: cleared all uploaders" ), 2 );
}

int QgsRasterGPUCacheManager::cacheSize() const
{
  QMutexLocker locker( &mMutex );
  return mCache.size();
}

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
