/***************************************************************************
  qgsrastergpucachemanager.h - Persistent GPU resource cache
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

#ifndef QGSRASTERGPUCACHEMANAGER_H
#define QGSRASTERGPUCACHEMANAGER_H

#include <memory>
#include <unordered_map>

#include "qgis_gui.h"

#include <QHash>
#include <QMutex>
#include <QString>

class QgsRasterGPUTileUploader;
class QgsRasterTileReader;

/**
 * \ingroup gui
 * \class QgsRasterGPUCacheManager
 * \brief Manages persistent GPU resources for raster rendering.
 *
 * This singleton class maintains tile uploaders and their caches across
 * render calls, enabling texture reuse and avoiding expensive re-uploads
 * on every map redraw.
 *
 * Thread-safe for concurrent access.
 *
 * \since QGIS 3.40
 */
class GUI_EXPORT QgsRasterGPUCacheManager
{
  public:
    /**
     * \brief Get singleton instance
     */
    static QgsRasterGPUCacheManager *instance();

    /**
     * \brief Get or create a tile uploader for the given data source
     * \param dataSourceUri Data source URI (GDAL path)
     * \returns Shared tile uploader, or nullptr if initialization failed
     *
     * The uploader is cached and reused across render calls.
     * Returns nullptr if the dataset cannot be opened or is not tiled.
     */
    QgsRasterGPUTileUploader *getOrCreateUploader( const QString &dataSourceUri );

    /**
     * \brief Remove cached uploader for a data source
     * \param dataSourceUri Data source URI
     *
     * Call when a layer is removed or data source changes.
     */
    void removeUploader( const QString &dataSourceUri );

    /**
     * \brief Clear all cached resources
     *
     * Called during application shutdown.
     */
    void clearAll();

    /**
     * \brief Get number of cached uploaders
     */
    int cacheSize() const;

    /**
     * \brief Get total GPU memory used by all caches
     */
    qint64 totalGPUMemoryUsed() const;

  private:
    QgsRasterGPUCacheManager() = default;
    ~QgsRasterGPUCacheManager();

    // Non-copyable
    QgsRasterGPUCacheManager( const QgsRasterGPUCacheManager & ) = delete;
    QgsRasterGPUCacheManager &operator=( const QgsRasterGPUCacheManager & ) = delete;

    struct CacheEntry
    {
        std::unique_ptr<QgsRasterTileReader> reader;
        std::unique_ptr<QgsRasterGPUTileUploader> uploader;
    };

    struct QStringHash
    {
        std::size_t operator()( const QString &s ) const { return qHash( s ); }
    };

    mutable QMutex mMutex;
    std::unordered_map<QString, std::unique_ptr<CacheEntry>, QStringHash> mCache;

    static QgsRasterGPUCacheManager *sInstance;
};

#endif // QGSRASTERGPUCACHEMANAGER_H
