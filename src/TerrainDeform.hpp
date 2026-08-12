// Client-local terrain deformation brush for wxl-housing.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include <cstdint>

namespace wxl_housing
{
    class TerrainDeform
    {
    public:
        static TerrainDeform& Instance();

        // Arms the build-12340 Adt.ChunkBuild named hook and loads saved edits.
        bool Initialize();

        // Builder integration. DrawControls is called inside the housing panel;
        // OnEndScene draws the world-space brush and commits queued world clicks.
        void DrawControls();
        void OnEndScene();

        // Called by the housing WndProc before its decor-selection path. While the
        // terrain tool is active, the click is queued for the next safe render frame
        // and swallowed so WoW does not also move/target.
        bool HandleScreenClick(int clientX, int clientY);

        bool Active() const { return active_; }
        void SetActive(bool active);
        const char* Status() const { return status_; }

    private:
        TerrainDeform() = default;

        enum class Mode : uint8_t { Raise, Lower };

        struct Impl;
        Impl* impl_ = nullptr;
        bool initialized_ = false;
        bool active_ = false;
        bool clickPending_ = false;
        int clickX_ = 0;
        int clickY_ = 0;
        Mode mode_ = Mode::Raise;
        float radius_ = 8.0f;
        float strength_ = 1.0f;
        const char* status_ = "Terrain deformation has not initialized";
    };
}
