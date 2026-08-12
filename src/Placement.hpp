// Decor placement: SpawnFromMDDF call, world pick, ImGuizmo gizmo, selection box.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"
#include "Catalog.hpp"

namespace wxl_housing
{
    class Placement
    {
    public:
        static Placement& Instance();

        /// Place a catalog row in front of the player. Returns true when the spawn call
        /// was issued (the model resolves asynchronously; selection follows once loaded).
        bool SpawnRow(const DecorRow& row);

        /// Per-frame: queued pick, pending-selection promotion, selection box and gizmo.
        /// ImGuiHostExt calls this inside its active NewFrame/Render scope.
        void OnEndScene();

        /// Lightweight world tick that restores saved objects even while the editor is hidden.
        void TickPersistence();

        /// OnDoodadSpawn observer. Direct native return values are authoritative because
        /// one logical placement can emit several synchronous part events.
        void OnDoodadSpawn(const wxl::events::DoodadSpawnArgs& a);

    private:
        Placement();

        void DrawToolbar();
        void DrawPlacedList();
        void DrawGizmo();
        void DrawSelectionBox();
        void DoPick(int sx, int sy);
        void PrunePlaced();
        void PromotePending();
        bool HasPending() const { return !pendingObjects_.empty(); }
        bool SpawnAt(const DecorRow& row, const float pos[3], bool restoring,
                     uint64_t savedId, const float* desiredMatrix);
        void LoadPlacements();
        void SavePlacements();
        void PersistSelectedTransform();
        void ForgetSelectedOnReload();
        void DuplicateObject(void* object);
        void RequestDelete(void* object);
        void DrawDeleteConfirmation();
        void DeleteObject(void* object);
        bool IsSavedLive(uint64_t id) const;

        struct SavedPlacement
        {
            uint64_t id = 0;
            uint32_t rowId = 0;
            int mapId = -1;
            float matrix[16] = {};
        };

        struct RuntimePlacement
        {
            // One logical catalog placement can contain several M2s sharing the same
            // Local origin used when spawning a composite custom prop.
            // Retail decor continues to have exactly one entry here.
            std::vector<void*> objects;
            uint64_t savedId = 0;
            uint32_t rowId = 0;
        };

        RuntimePlacement* FindRuntime(void* object);
        const RuntimePlacement* FindRuntime(void* object) const;
        static void* PrimaryObject(const RuntimePlacement& runtime);
        static bool ContainsObject(const RuntimePlacement& runtime, void* object);
        static void ApplyGroupMatrix(RuntimePlacement& runtime, const float matrix[16]);
        static void HideGroup(RuntimePlacement& runtime);
        void ClearPending();

        void* selected_ = nullptr;
        void* deleteCandidate_ = nullptr;
        std::vector<void*> pendingObjects_; // one logical spawn, awaiting every part's matrix
        bool  pendingRestore_ = false;
        bool  pendingHasMatrix_ = false;
        uint64_t pendingSavedId_ = 0;
        uint32_t pendingRowId_ = 0;
        int pendingMapId_ = -1;
        float pendingMatrix_[16] = {};
        uint32_t pendingFrames_ = 0;
        int   gizmoOp_ = 0;             // initialized to ImGuizmo::TRANSLATE in the ctor
        bool  gizmoWasUsing_ = false;
        bool  persistenceLoaded_ = false;
        int observedMap_ = -9999;
        uint32_t restoreDelay_ = 0;
        size_t restoreCursor_ = 0;
        uint64_t nextSavedId_ = 1;
        std::vector<RuntimePlacement> placed_; // housing objects only; never mutate map scenery
        std::vector<SavedPlacement> saved_;
    };
}
