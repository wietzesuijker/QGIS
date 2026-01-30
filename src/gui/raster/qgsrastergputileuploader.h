/***************************************************************************
  qgsrastergputileuploader.h - GPU tile uploader for rasters (QRhi)
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

#ifndef QGSRASTERGPUTILEUPLOADER_H
#define QGSRASTERGPUTILEUPLOADER_H

#define SIP_NO_FILE

#include "qgis_gui.h"
#include "qgsrastertilereader.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QThread>

#ifdef HAVE_QRHI
#include <rhi/qrhi.h>
#endif

/**
 * \ingroup gui
 * \class QgsRasterGPUTileUploader
 * \brief GPU tile uploader with LRU texture caching using QRhi.
 *
 * Combines fast COG tile reading (via QgsRasterTileReader) with GPU
 * texture uploads for GPU-accelerated raster rendering.
 *
 * Data flow (not zero-copy):
 * 1. GDAL GDALReadBlock() → CPU buffer (QByteArray)
 * 2. QRhiResourceUpdateBatch::uploadTexture() → GPU texture memory
 *
 * Key features:
 *
 * - LRU texture caching (avoids re-upload on pan/zoom)
 * - Format-aware texture creation (maps GDAL types to QRhi formats)
 * - Automatic overview level selection
 * - Cache eviction for memory management
 * - Batched texture uploads for efficiency
 *
 * Thread safety:
 * This class must only be used from the thread that owns the QRhi context
 * (typically the main/GUI thread). Calling from other threads will fail
 * validation checks and return invalid tiles.
 *
 * \since QGIS 3.44
 */
class GUI_EXPORT QgsRasterGPUTileUploader
{
  public:
#ifdef HAVE_QRHI
    /**
     * \brief GPU tile structure
     */
    struct GPUTile
    {
        QRhiTexture *texture = nullptr; //!< QRhi texture (owned by uploader)
        int width = 0;                  //!< Tile width in pixels
        int height = 0;                 //!< Tile height in pixels
        quint64 lastUsedFrame = 0;      //!< Frame number when last used
        bool isValid = false;           //!< Whether texture is valid
        bool needsUpload = false;       //!< Whether texture data needs uploading
        QByteArray data;                //!< Pending upload data (cleared after upload)
    };

    /**
     * \brief Constructor
     * \param reader COG tile reader (ownership NOT transferred)
     * \param rhi QRhi instance (ownership NOT transferred)
     */
    explicit QgsRasterGPUTileUploader( QgsRasterTileReader *reader, QRhi *rhi );

    //! Destructor - releases GPU resources
    ~QgsRasterGPUTileUploader();

    /**
     * \brief Tile coordinate for batch operations
     */
    struct TileCoord
    {
        int level;
        int x;
        int y;
    };

    /**
     * \brief Get or create GPU tile (with caching)
     *
     * If the tile is not in cache, creates the texture and marks it for upload.
     * Call collectPendingUploads() after getting all tiles to batch uploads.
     *
     * \param overviewLevel Overview level
     * \param tileX Tile X index
     * \param tileY Tile Y index
     * \param bandNumber Band number
     * \param frameNumber Current frame number (for LRU)
     * \returns GPU tile from cache or newly created
     */
    GPUTile getTile( int overviewLevel, int tileX, int tileY, int bandNumber, quint64 frameNumber );

    /**
     * \brief Batch get multiple tiles (single mutex lock)
     *
     * More efficient than multiple getTile() calls when fetching many tiles.
     * \param coords List of tile coordinates
     * \param bandNumber Band number
     * \param frameNumber Current frame number (for LRU)
     * \returns Vector of GPU tiles (same order as coords)
     */
    QVector<GPUTile> getTiles( const QVector<TileCoord> &coords, int bandNumber, quint64 frameNumber );

    /**
     * \brief Get or create RGB GPU tile (with caching)
     * \param overviewLevel Overview level
     * \param tileX Tile X index
     * \param tileY Tile Y index
     * \param redBand Red band number (1-based)
     * \param greenBand Green band number (1-based)
     * \param blueBand Blue band number (1-based)
     * \param frameNumber Current frame number (for LRU)
     * \returns GPU tile from cache or newly created
     */
    GPUTile getRGBTile( int overviewLevel, int tileX, int tileY, int redBand, int greenBand, int blueBand, quint64 frameNumber );

    /**
     * \brief Queue pending texture uploads to a resource batch
     *
     * Call this after getTile()/getTiles() to add pending uploads to the batch.
     * The batch should be applied before beginning the render pass.
     *
     * \param batch Resource update batch to add uploads to
     * \returns Number of textures queued for upload
     */
    int collectPendingUploads( QRhiResourceUpdateBatch *batch );

    /**
     * \brief Clear tile cache and release GPU resources
     */
    void clearCache();

    /**
     * \brief Evict old tiles from cache
     * \param currentFrame Current frame number
     * \param maxAge Max age in frames before eviction
     * \param maxMemoryBytes Maximum VRAM budget in bytes (0 = no limit)
     *
     * Eviction happens in two phases:
     * 1. Remove tiles older than maxAge frames
     * 2. If still over budget, remove oldest tiles until under maxMemoryBytes
     */
    void evictOldTiles( quint64 currentFrame, int maxAge = 120, qint64 maxMemoryBytes = 512 * 1024 * 1024 );

    /**
     * \brief Get cache statistics
     * \param[out] cachedCount Number of cached tiles
     * \param[out] memoryBytes Approximate GPU memory used
     */
    void getCacheStats( int &cachedCount, qint64 &memoryBytes ) const;

#endif // HAVE_QRHI

    /**
     * \brief Get tile info for overview level (delegates to reader)
     */
    QgsRasterTileReader::TileInfo tileInfo( int overviewLevel = 0 ) const;

    /**
     * \brief Get raster extent (delegates to reader)
     */
    QgsRectangle rasterExtent() const;

    /**
     * \brief Select best overview based on map units per pixel
     */
    int selectBestOverview( double targetMupp ) const;

    /**
     * \brief Calculate georeferenced extent for a specific tile
     * \param overviewLevel Overview level
     * \param tileX Tile X index
     * \param tileY Tile Y index
     * \returns Tile extent in raster CRS
     */
    QgsRectangle tileExtent( int overviewLevel, int tileX, int tileY ) const;

  private:
#ifdef HAVE_QRHI
    /**
     * \brief Create tile cache key for single-band
     */
    static quint64 makeTileKey( int overview, int tileX, int tileY, int band );

    /**
     * \brief Create tile cache key for RGB (multi-band)
     */
    static quint64 makeRGBTileKey( int overview, int tileX, int tileY, int redBand, int greenBand, int blueBand );

    /**
     * \brief Create a new GPU tile (texture + pending upload data)
     */
    GPUTile createTile( int overviewLevel, int tileX, int tileY, int bandNumber );

    /**
     * \brief Create a new RGB GPU tile (texture + pending upload data)
     */
    GPUTile createRGBTile( int overviewLevel, int tileX, int tileY, int redBand, int greenBand, int blueBand );

    /**
     * \brief Get QRhi texture format for data type
     */
    QRhiTexture::Format textureFormat( Qgis::DataType dataType, int bandCount ) const;

    //! QRhi instance
    QRhi *mRhi = nullptr;

    //! Tile cache
    QHash<quint64, GPUTile> mTileCache;

    //! Keys of tiles with pending uploads
    QVector<quint64> mPendingUploads;

#endif // HAVE_QRHI

    //! COG tile reader
    QgsRasterTileReader *mReader = nullptr;

    //! Temporary buffer for tile data
    QByteArray mTileBuffer;

    //! Thread that created this uploader
    QThread *mOwnerThread = nullptr;

    //! Mutex for thread-safe cache access
    mutable QMutex mMutex;
};

#endif // QGSRASTERGPUTILEUPLOADER_H
