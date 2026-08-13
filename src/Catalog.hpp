// HouseDecor.db2 catalog: schema, load, search, and the Place action.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "wxl/PluginApi.h"

namespace wxl_housing
{
    /// One HouseDecor row. Retail names are joined from the generated sidecar because
    /// wxl.db2 v1 intentionally exposes numeric cells only.
    struct DecorRow
    {
        uint32_t rowId;
        uint32_t modelFdid;
        uint32_t thumbFdid;
        float    initialScale;
        uint32_t type;
        uint32_t modelType;
        uint32_t itemId;
        uint32_t flags;
        uint32_t orderIndex;
        std::string name;
        std::string modelPath; // resolved once via wxl.fdid (may be empty)
        // Composite custom decor uses modelPath as the primary/fallback model and
        // spawns every entry in modelParts at one shared transform.
        std::vector<std::string> modelParts;
        std::string thumbPath; // resolved Retail BLP or validated custom thumbnail path (may be empty)
        std::string customCategory; // manifest label; custom rows still share one UI rail category
        std::string attribution;    // optional author/license text from decor.json
        float spawnDistance = 4.0f;
        bool assetInstalled = false;
        bool placeable = false;
        bool dnt = false;
        bool custom = false;
        bool discovered = false; // true only for rows loaded from decor.json
    };

    class Catalog
    {
    public:
        static Catalog& Instance();

        /// Lazy table load; returns false if the DB2 definition could not be opened.
        bool EnsureLoaded();

        /// Called every OnEndScene: draws the panel when the host is visible.
        void Frame(void* device);

        /// Returns the Retail category atlas used by the always-visible launcher.
        /// This also supplies the D3D device while the full catalog is closed.
        void* LauncherTexture(void* device);

        /// Release the lazily-created preview textures before a D3D9 reset.
        void OnDeviceLost();

        /// Row list for the panel.
        const std::vector<DecorRow>& Rows() const { return rows_; }

        /// Look up a row by HouseDecor row id (0 when absent).
        const DecorRow* Find(uint32_t rowId) const;

    private:
        Catalog() = default;

        void DrawPanel();
        void DrawCategoryRail(float height);
        void DrawGrid(float height);
        void DrawCard(const DecorRow& row, float size);
        void DrawDetails(float height);
        void DrawCompactTable(float height);
        void RebuildFiltered();
        void RescanCustomProps();
        void* LoadTextureFile(uint32_t cacheKey, const char* path);
        void* LoadThumbnail(uint32_t fdid);
        void* LoadRowThumbnail(const DecorRow& row);
        void Place(const DecorRow& row);

        bool loaded_ = false;
        void* table_ = nullptr; // opaque handle from wxl.db2 Load(); owned by wxl-db2
        std::vector<DecorRow> rows_;
        std::vector<const DecorRow*> filtered_;

        char filter_[128] = {};
        // Keep the opening catalog focused on props that are usable in the
        // local editor.  Technical WMO/DNT entries remain available through
        // the Filters popup for inspection.
        bool includeDnt_ = false;
        bool placeableOnly_ = true;
        bool compactView_ = false;
        int categoryFilter_ = 1;
        int typeFilter_ = -1;
        uint32_t selectedRow_ = 0;
        int placeCount_ = 0;
        size_t customManifestCount_ = 0;
        std::vector<std::string> customScanErrors_;
        void* device_ = nullptr;
        int thumbnailLoadsRemaining_ = 0;
        std::unordered_map<uint32_t, void*> thumbnails_;
        std::unordered_map<std::string, void*> customThumbnails_;
    };
}
