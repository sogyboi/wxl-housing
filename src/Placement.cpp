// Decor placement implementation. The pick, gizmo, and selection-box routines are ported
// from wxl-mini-noggit (same core event surfaces, v1.1 include paths).
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "Placement.hpp"

#include "ExtensionApi.hpp"
#include "ImGuiHostExt.hpp"

#include "game/Binding.hpp"
#include "game/Camera.hpp"
#include "game/Doodad.hpp"
#include "game/Io.hpp"
#include "game/World.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "imgui.h"

#include "../third_party/imguizmo/ImGuizmo.h" // vendored; see shared.cmake

namespace wxl_housing
{
    namespace
    {
        namespace doodad = wxl::game::doodad;
        namespace camera = wxl::game::camera;
        namespace io     = wxl::game::io;
        namespace world  = wxl::game::world;
        namespace ooddad = wxl::game::doodad::off;

        // MDDF record layout beyond the offsets header's curated landmarks (0x24 bytes,
        // standard MDDF, see offsets/game/Doodad.hpp): nameId/uniqueId u32 at +0x00/+0x04,
        // pos +0x08/+0x0C/+0x10 (header constants), rot floats at +0x14/+0x18/+0x1C,
        // scale u16 (1024 = 100%) at +0x20, flags u16 at +0x22.
        constexpr size_t kMddfNameId   = 0x00;
        constexpr size_t kMddfUniqueId = 0x04;
        constexpr size_t kMddfRotX     = 0x14;
        constexpr size_t kMddfRotY     = 0x18;
        constexpr size_t kMddfRotZ     = 0x1C;
        constexpr size_t kMddfScale    = 0x20;
        constexpr size_t kMddfFlags    = 0x22;

        // SpawnFromMDDF constructs and globally registers the doodad, but its only
        // retail call site immediately creates a reference node and links that node
        // into the owning chunk. Skipping this step leaves a stale global-list entry
        // when the map unloads (the client then faults while tearing the list down).
        // 0x007C0750 is the reference-node allocator used directly after
        // kSpawnFromMDDF at 0x007C63D4 in build 12340.
        constexpr uintptr_t kCreateDoodadRef = 0x007C0750;
        using CreateDoodadRefFn = void*(__cdecl*)(void* doodad);
        constexpr size_t kRefOwnerChunk = 0x08;
        constexpr size_t kDoodadVisibilityScale = 0x8C;

        // ImGuizmo operation bitmasks.
        constexpr int kOpTranslate = ImGuizmo::TRANSLATE;
        constexpr int kOpRotate    = ImGuizmo::ROTATE;
        constexpr int kOpScale     = ImGuizmo::SCALEU; // uniform size, matching housing decor

        constexpr const char* kPlacementPath = "Extensions\\wxl-housing\\placements.tsv";
        constexpr const char* kPlacementTemp = "Extensions\\wxl-housing\\placements.tsv.tmp";
        constexpr const char* kPlacementBackup = "Extensions\\wxl-housing\\placements.tsv.bak";

        uint32_t g_uniqueId = 0x50000000u; // synthetic unique ids, session-local

        bool IsM2Path(const char* path)
        {
            if (!path) return false;
            const char* ext = strrchr(path, '.');
            return ext && _stricmp(ext, ".m2") == 0;
        }

        bool NormalizeCustomM2Path(const std::string& input, std::string& output)
        {
            constexpr char kCustomRoot[] = "World\\wxl_housing\\custom\\";
            constexpr size_t kCustomRootLength = sizeof(kCustomRoot) - 1;
            output = input;
            std::replace(output.begin(), output.end(), '/', '\\');
            if (output.empty() || output.size() >= 512 || output.front() == '\\' ||
                output.find(':') != std::string::npos || output.find("..") != std::string::npos ||
                !IsM2Path(output.c_str()))
                return false;
            return output.size() > kCustomRootLength &&
                   _strnicmp(output.c_str(), kCustomRoot, kCustomRootLength) == 0;
        }

        bool ClientCanOpen(const std::string& path)
        {
            void* handle = nullptr;
            if (!io::FileOpen(path.c_str(), 0, &handle) || !handle) return false;
            io::FileClose(handle);
            return true;
        }

        bool CanAttachToChunk(void* chunk)
        {
            if (!doodad::detail::Readable(
                    chunk, ooddad::kChunkDoodadHead + sizeof(void*)))
                return false;

            const uint32_t linkOffset = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const char*>(chunk) + ooddad::kChunkDoodadLinkOff);
            if (linkOffset > 0x400) return false;

            // The list sentinel starts one pointer before kChunkDoodadHead. Its
            // first word is the previous node; that node's +4 slot is writable.
            void* previousNode = *reinterpret_cast<void**>(
                reinterpret_cast<char*>(chunk) + ooddad::kChunkDoodadHead - sizeof(void*));
            return doodad::detail::Writable(previousNode, 2 * sizeof(void*));
        }

        bool AttachToChunk(void* doodadObject, void* chunk)
        {
            if (!doodadObject || !CanAttachToChunk(chunk)) return false;

            const uint32_t linkOffset = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const char*>(chunk) + ooddad::kChunkDoodadLinkOff);
            void* previousNode = *reinterpret_cast<void**>(
                reinterpret_cast<char*>(chunk) + ooddad::kChunkDoodadHead - sizeof(void*));

            static const auto s_createRef =
                wxl::game::Native<CreateDoodadRefFn>(kCreateDoodadRef);
            void* reference = s_createRef(doodadObject);
            if (!reference) return false;

            char* node = reinterpret_cast<char*>(reference) + linkOffset;
            // kCreateDoodadRef zero-initializes both supported intrusive-link
            // pairs. The native allocator has already returned committed pool
            // storage here; no failure path remains after it increments the
            // doodad reference count.

            *reinterpret_cast<void**>(reinterpret_cast<char*>(reference) + kRefOwnerChunk) = chunk;

            // Exact push-back sequence from the native placement caller. The
            // intrusive list stores a previous-node address and a next object base,
            // hence the deliberately asymmetric node/reference assignments.
            *reinterpret_cast<void**>(node) = previousNode;
            *reinterpret_cast<void**>(node + sizeof(void*)) =
                *reinterpret_cast<void**>(reinterpret_cast<char*>(previousNode) + sizeof(void*));
            *reinterpret_cast<void**>(reinterpret_cast<char*>(previousNode) + sizeof(void*)) = reference;
            *reinterpret_cast<void**>(
                reinterpret_cast<char*>(chunk) + ooddad::kChunkDoodadHead - sizeof(void*)) = node;

            *reinterpret_cast<float*>(
                reinterpret_cast<char*>(doodadObject) + kDoodadVisibilityScale) = 1.0f;
            *reinterpret_cast<uint32_t*>(
                reinterpret_cast<char*>(doodadObject) + ooddad::kFlags) |= 4u;
            return true;
        }
    }

    Placement::Placement() : gizmoOp_(kOpTranslate) {}

    Placement& Placement::Instance()
    {
        static Placement s;
        return s;
    }

    bool Placement::SpawnRow(const DecorRow& row)
    {
        if (!row.custom && !g_fdid) return false;

        if (world::CurrentMapId() < 0)
        {
            WLOG_WARN("placement: enter the world before placing decor");
            return false;
        }

        // Retail rows resolve their FileDataID through wxl-db2. Custom rows carry
        // explicit, tightly-scoped archive paths and are validated in SpawnAt.
        const char* path = row.custom ? row.modelPath.c_str() : g_fdid->ResolveModel(row.modelFdid);
        if (!path || !*path)
        {
            WLOG_WARN("placement: fdid %u does not resolve to a model path", row.modelFdid);
            return false;
        }
        if (!IsM2Path(path))
        {
            // kSpawnFromMDDF is the M2 placement constructor. Feeding it a WMO path is
            // unsafe; WMO placement needs a populated MODF/name-table context instead.
            WLOG_WARN("placement: fdid %u resolves to unsupported non-M2 model: %s",
                      row.modelFdid, path);
            return false;
        }

        // Place four yards beyond the player/focus point, along the camera-to-player
        // horizontal direction. This works in third person; first person falls back to
        // the D3D row-vector view matrix's forward column.
        float camPos[3];
        camera::GetPosition(camPos);
        const unsigned long long playerGuid = world::ActivePlayerGuid();
        void* player = playerGuid ? world::ResolveObject(playerGuid, world::kTypeMaskPlayer) : nullptr;
        if (!player)
        {
            WLOG_WARN("placement: active player is not available");
            return false;
        }
        float focus[3];
        world::UnitPosition(player, focus);
        const float* view = camera::GetView();
        float forward[2] = { focus[0] - camPos[0], focus[1] - camPos[1] };
        float forwardLen = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        if (!(forwardLen > 0.001f) || !std::isfinite(forwardLen))
        {
            forward[0] = view[2];
            forward[1] = view[6];
            forwardLen = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        }
        if (!(forwardLen > 0.001f) || !std::isfinite(forwardLen))
        {
            forward[0] = 1.0f;
            forward[1] = 0.0f;
            forwardLen = 1.0f;
        }
        forward[0] /= forwardLen;
        forward[1] /= forwardLen;
        float distance = row.spawnDistance;
        if (!std::isfinite(distance) || distance < 1.0f) distance = 4.0f;
        distance = (std::min)(distance, 60.0f);
        float pos[3] = {
            focus[0] + forward[0] * distance,
            focus[1] + forward[1] * distance,
            focus[2],
        };
        return SpawnAt(row, pos, false, 0, nullptr);
    }

    bool Placement::SpawnAt(const DecorRow& row, const float pos[3], bool restoring,
                            uint64_t savedId, const float* desiredMatrix)
    {
        if (world::CurrentMapId() < 0 || (!row.custom && !g_fdid) || HasPending()) return false;

        std::vector<std::string> paths;
        if (row.custom)
        {
            const std::vector<std::string>& requested = row.modelParts.empty()
                ? std::vector<std::string>{ row.modelPath } : row.modelParts;
            paths.reserve(requested.size());
            for (const std::string& requestedPath : requested)
            {
                std::string normalized;
                if (!NormalizeCustomM2Path(requestedPath, normalized))
                {
                    WLOG_WARN("placement: rejected unsafe custom M2 path: %s", requestedPath.c_str());
                    return false;
                }
                if (!ClientCanOpen(normalized))
                {
                    WLOG_WARN("placement: custom M2 is not visible to client storage: %s", normalized.c_str());
                    return false;
                }
                paths.push_back(std::move(normalized));
            }
        }
        else
        {
            const char* resolved = g_fdid->ResolveModel(row.modelFdid);
            if (!resolved || !IsM2Path(resolved)) return false;
            paths.emplace_back(resolved);
        }
        if (paths.empty()) return false;

        float chunkPosition[3] = { pos[0], pos[1], pos[2] };
        void* ownerChunk = doodad::ChunkAt(chunkPosition);
        if (!CanAttachToChunk(ownerChunk))
        {
            if (!restoring)
                WLOG_WARN("placement: no writable terrain chunk at target position");
            return false;
        }

        float initialScale = row.initialScale;
        if (!std::isfinite(initialScale) || initialScale <= 0.0f) initialScale = 1.0f;
        initialScale = (std::max)(1.0f / 1024.0f, (std::min)(initialScale, 65535.0f / 1024.0f));
        const uint32_t rawScale = (std::min)(
            static_cast<uint32_t>(initialScale * 1024.0f + 0.5f), 65535u);

        static const auto s_spawn =
            wxl::game::Native<ooddad::SpawnFromMDDFFn>(ooddad::kSpawnFromMDDF);
        static const auto s_purge =
            wxl::game::Native<ooddad::DoodadPurgeFn>(ooddad::kDoodadPurge);

        std::vector<void*> spawnedObjects;
        spawnedObjects.reserve(paths.size());
        float origin[3] = { ooddad::kMddfTileOriginX, ooddad::kMddfTileOriginY, 0.0f };
        for (size_t part = 0; part < paths.size(); ++part)
        {
            // Every part needs a distinct native unique id even though their transform
            // is identical. They are grouped only in housing's editor/persistence state.
            alignas(4) unsigned char mddf[0x24] = {};
            const uint32_t uniqueId = g_uniqueId++;
            *reinterpret_cast<uint32_t*>(mddf + kMddfNameId)   = 0;
            *reinterpret_cast<uint32_t*>(mddf + kMddfUniqueId) = uniqueId;
            *reinterpret_cast<float*>(mddf + ooddad::kMddfPosX) = origin[1] - pos[1];
            *reinterpret_cast<float*>(mddf + ooddad::kMddfPosY) = pos[2] - origin[2];
            *reinterpret_cast<float*>(mddf + ooddad::kMddfPosZ) = origin[0] - pos[0];
            *reinterpret_cast<float*>(mddf + kMddfRotX) = 0.0f;
            *reinterpret_cast<float*>(mddf + kMddfRotY) = 0.0f;
            *reinterpret_cast<float*>(mddf + kMddfRotZ) = 0.0f;
            *reinterpret_cast<uint16_t*>(mddf + kMddfScale) = static_cast<uint16_t>(rawScale);
            *reinterpret_cast<uint16_t*>(mddf + kMddfFlags) = 1;

            void* spawned = s_spawn(paths[part].c_str(), mddf, origin);

            if (!spawned)
            {
                WLOG_WARN("placement: native spawn failed row=%u part=%zu path=%s",
                          row.rowId, part + 1, paths[part].c_str());
                RuntimePlacement partial{ spawnedObjects, 0, row.rowId };
                HideGroup(partial); // attached parts are reclaimed by normal chunk unload
                return false;
            }
            if (!AttachToChunk(spawned, ownerChunk))
            {
                // This object has no reference yet, so its zero-ref purge is safe.
                s_purge(spawned);
                RuntimePlacement partial{ spawnedObjects, 0, row.rowId };
                HideGroup(partial);
                WLOG_WARN("placement: chunk attachment failed row=%u part=%zu",
                          row.rowId, part + 1);
                return false;
            }
            spawnedObjects.push_back(spawned);
            WLOG_INFO("placement: spawned row=%u part=%zu/%zu uniqueId=%u path=%s -> obj=%p",
                      row.rowId, part + 1, paths.size(), uniqueId, paths[part].c_str(), spawned);
        }

        placed_.push_back({ spawnedObjects, savedId, row.rowId });
        pendingObjects_ = std::move(spawnedObjects);
        pendingFrames_ = 0;
        pendingRestore_ = restoring;
        pendingSavedId_ = savedId;
        pendingRowId_ = row.rowId;
        pendingMapId_ = world::CurrentMapId();
        pendingHasMatrix_ = desiredMatrix != nullptr;
        if (desiredMatrix) std::memcpy(pendingMatrix_, desiredMatrix, sizeof pendingMatrix_);
        WLOG_INFO("placement: logical row=%u parts=%zu at (%.2f %.2f %.2f)",
                  row.rowId, pendingObjects_.size(), pos[0], pos[1], pos[2]);
        return true;
    }

    void Placement::OnDoodadSpawn(const wxl::events::DoodadSpawnArgs& a)
    {
        // SpawnAt owns the native return values directly. The observer fires inside
        // each call and is intentionally not used as group state: doing so would lose
        // all but the final part of a composite placement.
        (void)a;
    }

    bool Placement::ContainsObject(const RuntimePlacement& runtime, void* object)
    {
        return object && std::find(runtime.objects.begin(), runtime.objects.end(), object) !=
                         runtime.objects.end();
    }

    void* Placement::PrimaryObject(const RuntimePlacement& runtime)
    {
        const auto found = std::find_if(runtime.objects.begin(), runtime.objects.end(),
            [](void* object) { return doodad::IsValid(object); });
        return found != runtime.objects.end() ? *found : nullptr;
    }

    Placement::RuntimePlacement* Placement::FindRuntime(void* object)
    {
        const auto found = std::find_if(placed_.begin(), placed_.end(),
            [object](const RuntimePlacement& runtime) { return ContainsObject(runtime, object); });
        return found != placed_.end() ? &*found : nullptr;
    }

    const Placement::RuntimePlacement* Placement::FindRuntime(void* object) const
    {
        const auto found = std::find_if(placed_.begin(), placed_.end(),
            [object](const RuntimePlacement& runtime) { return ContainsObject(runtime, object); });
        return found != placed_.end() ? &*found : nullptr;
    }

    void Placement::ApplyGroupMatrix(RuntimePlacement& runtime, const float matrix[16])
    {
        for (void* object : runtime.objects)
            if (doodad::IsValid(object)) doodad::SetWorldMatrix(object, matrix);
    }

    void Placement::HideGroup(RuntimePlacement& runtime)
    {
        for (void* object : runtime.objects)
        {
            float matrix[16];
            if (!doodad::IsValid(object) || !doodad::WorldMatrix(object, matrix)) continue;
            for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                matrix[row * 4 + column] *= 0.001f;
            matrix[14] = -10000.0f;
            doodad::SetWorldMatrix(object, matrix);
        }
    }

    void Placement::ClearPending()
    {
        pendingObjects_.clear();
        pendingFrames_ = 0;
        pendingRestore_ = false;
        pendingHasMatrix_ = false;
        pendingSavedId_ = 0;
        pendingRowId_ = 0;
        pendingMapId_ = -1;
    }

    void Placement::PrunePlaced()
    {
        for (RuntimePlacement& runtime : placed_)
        {
            const bool selectedHere = ContainsObject(runtime, selected_);
            const bool deleteHere = ContainsObject(runtime, deleteCandidate_);
            runtime.objects.erase(std::remove_if(runtime.objects.begin(), runtime.objects.end(),
                [](void* object) { return !doodad::IsValid(object); }), runtime.objects.end());
            if (selectedHere) selected_ = PrimaryObject(runtime);
            if (deleteHere) deleteCandidate_ = PrimaryObject(runtime);
        }
        placed_.erase(std::remove_if(placed_.begin(), placed_.end(),
            [](const RuntimePlacement& runtime) { return runtime.objects.empty(); }), placed_.end());
    }

    void Placement::LoadPlacements()
    {
        if (persistenceLoaded_) return;
        persistenceLoaded_ = true;
        std::ifstream input(kPlacementPath, std::ios::binary);
        if (!input)
        {
            WLOG_INFO("persistence: no saved placement file yet");
            return;
        }

        std::string line;
        std::getline(input, line); // WXLHOUSING 1 header
        while (std::getline(input, line))
        {
            if (line.empty()) continue;
            std::istringstream stream(line);
            char kind = 0;
            SavedPlacement saved;
            stream >> kind >> saved.id >> saved.mapId >> saved.rowId;
            for (float& value : saved.matrix) stream >> value;
            if (kind != 'P' || !stream || !saved.id || !saved.rowId) continue;
            // Persistence owns stable row ids, not catalog definitions. A custom
            // manifest can be temporarily missing, disabled, or loaded after this
            // file. Keep the saved record in all of those cases: TickPersistence
            // resolves the current row when it can and otherwise leaves the record
            // untouched so a later SavePlacements cannot erase unavailable props.
            saved_.push_back(saved);
            nextSavedId_ = (std::max)(nextSavedId_, saved.id + 1);
        }
        WLOG_INFO("persistence: loaded %zu saved placements", saved_.size());
    }

    void Placement::SavePlacements()
    {
        std::ofstream output(kPlacementTemp, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            WLOG_ERROR("persistence: cannot write %s", kPlacementTemp);
            return;
        }
        output << "WXLHOUSING\t1\n" << std::setprecision(9);
        for (const SavedPlacement& saved : saved_)
        {
            output << 'P' << '\t' << saved.id << '\t' << saved.mapId << '\t' << saved.rowId;
            for (float value : saved.matrix) output << '\t' << value;
            output << '\n';
        }
        output.flush();
        if (!output)
        {
            WLOG_ERROR("persistence: write failed");
            return;
        }
        output.close();
        CopyFileA(kPlacementPath, kPlacementBackup, FALSE); // best-effort previous revision
        if (!MoveFileExA(kPlacementTemp, kPlacementPath,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            WLOG_ERROR("persistence: atomic replace failed error=%lu", GetLastError());
            return;
        }
        WLOG_INFO("persistence: saved %zu placements", saved_.size());
    }

    bool Placement::IsSavedLive(uint64_t id) const
    {
        return std::any_of(placed_.begin(), placed_.end(),
            [id](const RuntimePlacement& runtime) { return runtime.savedId == id; });
    }

    void Placement::PersistSelectedTransform()
    {
        if (!selected_ || !doodad::IsValid(selected_)) return;
        RuntimePlacement* runtime = FindRuntime(selected_);
        if (!runtime) return;
        void* primary = PrimaryObject(*runtime);
        if (!primary) return;
        float matrix[16];
        if (!doodad::WorldMatrix(primary, matrix)) return;
        ApplyGroupMatrix(*runtime, matrix);

        SavedPlacement* saved = nullptr;
        if (runtime->savedId)
        {
            const auto found = std::find_if(saved_.begin(), saved_.end(),
                [runtime](const SavedPlacement& value) { return value.id == runtime->savedId; });
            if (found != saved_.end()) saved = &*found;
        }
        if (!saved)
        {
            SavedPlacement created;
            created.id = nextSavedId_++;
            created.rowId = runtime->rowId;
            created.mapId = world::CurrentMapId();
            saved_.push_back(created);
            saved = &saved_.back();
            runtime->savedId = saved->id;
        }
        saved->mapId = world::CurrentMapId();
        saved->rowId = runtime->rowId;
        std::memcpy(saved->matrix, matrix, sizeof saved->matrix);
        SavePlacements();
    }

    void Placement::ForgetSelectedOnReload()
    {
        RuntimePlacement* runtime = FindRuntime(selected_);
        if (!runtime || !runtime->savedId) return;
        const uint64_t id = runtime->savedId;
        runtime->savedId = 0;
        saved_.erase(std::remove_if(saved_.begin(), saved_.end(),
            [id](const SavedPlacement& value) { return value.id == id; }), saved_.end());
        SavePlacements();
    }

    void Placement::DuplicateObject(void* object)
    {
        if (!object || !doodad::IsValid(object) || HasPending()) return;
        RuntimePlacement* runtime = FindRuntime(object);
        if (!runtime) return;
        const DecorRow* row = Catalog::Instance().Find(runtime->rowId);
        if (!row || !row->placeable) return;

        void* primary = PrimaryObject(*runtime);
        if (!primary) return;
        float matrix[16];
        if (!doodad::WorldMatrix(primary, matrix)) return;
        matrix[12] += row->custom ? 6.0f : 1.25f;
        const float pos[3] = { matrix[12], matrix[13], matrix[14] };
        if (!SpawnAt(*row, pos, false, 0, matrix))
            WLOG_WARN("placement: duplicate failed row=%u", row->rowId);
    }

    void Placement::RequestDelete(void* object)
    {
        RuntimePlacement* runtime = FindRuntime(object);
        if (!runtime) return;
        deleteCandidate_ = PrimaryObject(*runtime);
        if (!deleteCandidate_) return;
        ImGui::OpenPopup("Delete placed decor?");
    }

    void Placement::DeleteObject(void* object)
    {
        const auto runtime = std::find_if(placed_.begin(), placed_.end(),
            [object](const RuntimePlacement& value) { return ContainsObject(value, object); });
        if (runtime == placed_.end()) return;

        const uint64_t savedId = runtime->savedId;
        const uint32_t rowId = runtime->rowId;
        if (savedId)
        {
            saved_.erase(std::remove_if(saved_.begin(), saved_.end(),
                [savedId](const SavedPlacement& value) { return value.id == savedId; }), saved_.end());
            SavePlacements();
        }

        // Attached doodads have no safe public destructor. Hide every part now;
        // normal terrain-chunk unload reclaims their reference nodes and objects.
        HideGroup(*runtime);

        if (ContainsObject(*runtime, selected_)) selected_ = nullptr;
        if (ContainsObject(*runtime, deleteCandidate_)) deleteCandidate_ = nullptr;
        placed_.erase(runtime);
        WLOG_INFO("placement: deleted row=%u savedId=%llu group hidden pending chunk unload",
                  rowId, static_cast<unsigned long long>(savedId));
    }

    void Placement::DrawDeleteConfirmation()
    {
        if (!ImGui::BeginPopupModal("Delete placed decor?", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;

        const auto runtime = std::find_if(placed_.begin(), placed_.end(),
            [this](const RuntimePlacement& value) { return ContainsObject(value, deleteCandidate_); });
        const DecorRow* row = runtime != placed_.end()
            ? Catalog::Instance().Find(runtime->rowId) : nullptr;
        ImGui::Text("Delete %s?", row ? row->name.c_str() : "this decor");
        ImGui::Separator();
        ImGui::TextWrapped("It will disappear now and its saved placement will be removed. "
                           "It will not return after relog or restart.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.08f, 0.04f, 1.0f));
        if (ImGui::Button("DELETE", ImVec2(110.0f, 30.0f)))
        {
            void* target = deleteCandidate_;
            ImGui::CloseCurrentPopup();
            DeleteObject(target);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 30.0f)))
        {
            deleteCandidate_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void Placement::PromotePending()
    {
        if (!HasPending()) return;
        RuntimePlacement* runtime = nullptr;
        for (void* object : pendingObjects_)
        {
            runtime = FindRuntime(object);
            if (runtime) break;
        }
        if (!runtime)
        {
            ClearPending();
            return;
        }

        // PrunePlaced may have noticed one native object disappear before this
        // promotion pass. A composite is all-or-nothing: do not leave the surviving
        // fragments as a smaller, silently corrupted logical placement.
        if (runtime->objects.size() != pendingObjects_.size())
        {
            const uint32_t rowId = runtime->rowId;
            HideGroup(*runtime);
            if (ContainsObject(*runtime, selected_)) selected_ = nullptr;
            const auto remove = std::find_if(placed_.begin(), placed_.end(),
                [runtime](const RuntimePlacement& value) { return &value == runtime; });
            if (remove != placed_.end()) placed_.erase(remove);
            ClearPending();
            WLOG_WARN("placement: logical row=%u lost a part before render-ready; group discarded", rowId);
            return;
        }

        bool allReady = true;
        for (void* object : pendingObjects_)
        {
            float ignored[16];
            if (!doodad::IsValid(object))
            {
                WLOG_WARN("placement: pending row=%u lost a composite part", pendingRowId_);
                allReady = false;
                pendingFrames_ = 901;
                break;
            }
            if (!doodad::WorldMatrix(object, ignored)) allReady = false;
        }

        if (allReady)
        {
            void* primary = PrimaryObject(*runtime);
            float matrix[16];
            if (!primary || !doodad::WorldMatrix(primary, matrix)) return;
            if (pendingHasMatrix_) std::memcpy(matrix, pendingMatrix_, sizeof matrix);
            ApplyGroupMatrix(*runtime, matrix);
            const bool restoring = pendingRestore_;
            const size_t partCount = runtime->objects.size();
            ClearPending();
            if (!restoring)
            {
                selected_ = primary;
                PersistSelectedTransform();
            }
            WLOG_INFO("placement: logical row=%u render-ready parts=%zu", runtime->rowId, partCount);
            return;
        }

        if (++pendingFrames_ > 900)
        {
            const uint32_t rowId = runtime->rowId;
            HideGroup(*runtime);
            if (ContainsObject(*runtime, selected_)) selected_ = nullptr;
            const auto remove = std::find_if(placed_.begin(), placed_.end(),
                [runtime](const RuntimePlacement& value) { return &value == runtime; });
            if (remove != placed_.end()) placed_.erase(remove);
            ClearPending();
            WLOG_WARN("placement: logical row=%u did not become fully render-ready; group discarded", rowId);
        }
    }

    void Placement::TickPersistence()
    {
        LoadPlacements();
        PrunePlaced();
        PromotePending();

        const int map = world::CurrentMapId();
        if (map != observedMap_)
        {
            observedMap_ = map;
            restoreCursor_ = 0;
            restoreDelay_ = 90;
        }
        if (map < 0 || HasPending() || saved_.empty()) return;
        if (restoreDelay_ > 0)
        {
            --restoreDelay_;
            return;
        }

        for (size_t attempts = 0; attempts < saved_.size(); ++attempts)
        {
            const SavedPlacement& saved = saved_[restoreCursor_++ % saved_.size()];
            if (saved.mapId != map || IsSavedLive(saved.id)) continue;
            const DecorRow* row = Catalog::Instance().Find(saved.rowId);
            if (!row || !row->placeable) continue;
            const float pos[3] = { saved.matrix[12], saved.matrix[13], saved.matrix[14] };
            const bool spawned = SpawnAt(*row, pos, true, saved.id, saved.matrix);
            restoreDelay_ = spawned ? 20 : 30;
            break;
        }
    }

    void Placement::OnEndScene()
    {
        namespace doodad = wxl::game::doodad;

        auto& host = ImGuiHostExt::Instance();
        if (!host.Visible()) return;
        host.SetGizmoBusy(false);

        // The input router queues client-area clicks between frames. Resolve them only
        // now, after NewFrame refreshed DisplaySize and before any selection UI is drawn.
        int pickX = 0, pickY = 0;
        if (host.ConsumePick(pickX, pickY)) DoPick(pickX, pickY);

        PrunePlaced();

        // Drop a selection whose doodad was freed (chunk unloaded, etc.).
        if (selected_ && !doodad::IsValid(selected_)) selected_ = nullptr;

        PromotePending();

        if (selected_) DrawToolbar();
        else DrawPlacedList();
        DrawSelectionBox(); // wireframe first, gizmo on top
        DrawGizmo();
    }

    void Placement::DrawPlacedList()
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2((std::max)(12.0f, io.DisplaySize.x - 410.0f), 24.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin("Housing - Placed Decor");
        ImGui::TextDisabled("%zu loaded nearby  |  %zu saved total", placed_.size(), saved_.size());
        if (placed_.empty())
            ImGui::TextWrapped("Place an item from the catalog, or move near a saved item so its terrain chunk can load.");
        const size_t visibleCount = placed_.size();
        for (size_t index = 0; index < visibleCount; ++index)
        {
            RuntimePlacement& runtime = placed_[index];
            void* object = PrimaryObject(runtime);
            if (!object) continue;
            const DecorRow* row = Catalog::Instance().Find(runtime.rowId);
            ImGui::PushID(static_cast<int>(index));
            ImGui::TextUnformatted(row ? row->name.c_str() : "Unknown decor");
            if (runtime.objects.size() > 1)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu parts)", runtime.objects.size());
            }
            if (runtime.savedId)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.35f, 0.86f, 0.45f, 1.0f), "saved");
            }
            if (ImGui::Button("Edit", ImVec2(62.0f, 0.0f))) selected_ = object;
            ImGui::SameLine();
            if (ImGui::Button("Duplicate", ImVec2(82.0f, 0.0f))) DuplicateObject(object);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.43f, 0.07f, 0.04f, 1.0f));
            if (ImGui::Button("Delete...", ImVec2(75.0f, 0.0f))) RequestDelete(object);
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::TextDisabled("You can also left-click a placed housing object in the world.");
        DrawDeleteConfirmation();
        ImGui::End();
    }

    void Placement::DrawToolbar()
    {
        namespace doodad = wxl::game::doodad;

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(550, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2((std::max)(12.0f, io.DisplaySize.x - 570.0f), 24.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin("Housing - Selected Decor");

        float p[3];
        doodad::Position(selected_, p);
        char name[260];
        const bool named = doodad::ModelName(selected_, name, sizeof name);
        ImGui::Text("Model: %s", named ? name : "(loading...)");
        float matrix[16];
        float displayScale = doodad::Scale(selected_);
        if (doodad::WorldMatrix(selected_, matrix))
            displayScale = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
        ImGui::TextDisabled("Position %.2f  %.2f  %.2f  |  Scale %.2f",
                            p[0], p[1], p[2], displayScale);
        RuntimePlacement* runtime = FindRuntime(selected_);
        const bool persisted = runtime && runtime->savedId != 0;
        if (runtime && runtime->objects.size() > 1)
            ImGui::TextDisabled("Composite placement: %zu synchronized parts", runtime->objects.size());
        ImGui::TextColored(persisted ? ImVec4(0.35f, 0.86f, 0.45f, 1.0f)
                                     : ImVec4(0.86f, 0.68f, 0.30f, 1.0f),
                           persisted ? "Saved - restores automatically" : "Session only - not saved yet");
        ImGui::Separator();
        ImGui::TextDisabled("EDIT MODE");
        if (ImGui::RadioButton("Move",        gizmoOp_ == kOpTranslate)) gizmoOp_ = kOpTranslate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate",      gizmoOp_ == kOpRotate))    gizmoOp_ = kOpRotate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale",       gizmoOp_ == kOpScale))     gizmoOp_ = kOpScale;

        float translation[3] = {}, rotation[3] = {}, scale[3] = { 1, 1, 1 };
        bool matrixReady = doodad::WorldMatrix(selected_, matrix);
        if (matrixReady)
        {
            ImGuizmo::DecomposeMatrixToComponents(matrix, translation, rotation, scale);
            bool changed = false;
            bool saveAfterEdit = false;
            changed |= ImGui::DragFloat3("Position", translation, 0.10f, -20000.0f, 20000.0f, "%.2f");
            saveAfterEdit |= ImGui::IsItemDeactivatedAfterEdit();
            changed |= ImGui::DragFloat3("Rotation", rotation, 0.50f, -360.0f, 360.0f, "%.1f deg");
            saveAfterEdit |= ImGui::IsItemDeactivatedAfterEdit();
            float uniformScale = (scale[0] + scale[1] + scale[2]) / 3.0f;
            if (ImGui::DragFloat("Scale", &uniformScale, 0.01f, 0.01f, 50.0f, "%.2f"))
            {
                scale[0] = scale[1] = scale[2] = uniformScale;
                changed = true;
            }
            saveAfterEdit |= ImGui::IsItemDeactivatedAfterEdit();
            if (changed)
            {
                ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, matrix);
                if (runtime) ApplyGroupMatrix(*runtime, matrix);
            }
            if (saveAfterEdit) PersistSelectedTransform();
        }

        if (ImGui::Button("Save now")) PersistSelectedTransform();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) DuplicateObject(selected_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!persisted);
        if (ImGui::Button("Forget on reload")) ForgetSelectedOnReload();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Deselect")) selected_ = nullptr;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.43f, 0.07f, 0.04f, 1.0f));
        if (ImGui::Button("Delete...")) RequestDelete(selected_);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Drag the gizmo or edit the numeric fields. Changes save when released.");
        DrawDeleteConfirmation();
        ImGui::End();
    }

    // Project a world point through the camera's view-projection (row-vector convention).
    static bool WorldToScreen(const float* vp, const float w[3], float W, float H, ImVec2& out)
    {
        float clip[4];
        for (int j = 0; j < 4; ++j)
            clip[j] = w[0] * vp[j] + w[1] * vp[4 + j] + w[2] * vp[8 + j] + vp[12 + j];
        if (clip[3] <= 0.001f) return false;
        out.x = (clip[0] / clip[3] * 0.5f + 0.5f) * W;
        out.y = (0.5f - clip[1] / clip[3] * 0.5f) * H;
        return true;
    }

    // Transform a model-local point by a row-major (row-vector) instance matrix.
    static void LocalToWorld(const float m[16], const float l[3], float w[3])
    {
        w[0] = l[0] * m[0] + l[1] * m[4] + l[2] * m[8]  + m[12];
        w[1] = l[0] * m[1] + l[1] * m[5] + l[2] * m[9]  + m[13];
        w[2] = l[0] * m[2] + l[1] * m[6] + l[2] * m[10] + m[14];
    }

    // Wireframe box around the selected doodad from its real local AABB, transformed by the
    // live instance matrix. Falls back to a small origin marker when bounds are unreadable.
    void Placement::DrawSelectionBox()
    {
        if (!selected_) return;
        RuntimePlacement* runtime = FindRuntime(selected_);
        if (!runtime) return;
        void* primary = PrimaryObject(*runtime);
        if (!primary) return;

        float m[16];
        if (!doodad::WorldMatrix(primary, m)) return;

        float lo[3] = {}, hi[3] = {};
        bool haveBounds = false;
        for (void* object : runtime->objects)
        {
            float partLo[3], partHi[3];
            if (!doodad::LocalBounds(object, partLo, partHi)) continue;
            if (!haveBounds)
            {
                std::memcpy(lo, partLo, sizeof lo);
                std::memcpy(hi, partHi, sizeof hi);
                haveBounds = true;
            }
            else
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    lo[axis] = (std::min)(lo[axis], partLo[axis]);
                    hi[axis] = (std::max)(hi[axis], partHi[axis]);
                }
            }
        }
        if (!haveBounds)
        {
            lo[0] = -2.0f; lo[1] = -2.0f; lo[2] = 0.0f;
            hi[0] =  2.0f; hi[1] =  2.0f; hi[2] = 6.0f;
        }

        const float* vp = camera::GetViewProj();
        const ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;

        ImVec2 s[8];
        bool ok[8];
        for (int k = 0; k < 8; ++k)
        {
            const float l[3] = { (k & 1) ? hi[0] : lo[0],
                                 (k & 2) ? hi[1] : lo[1],
                                 (k & 4) ? hi[2] : lo[2] };
            float w[3];
            LocalToWorld(m, l, w);
            ok[k] = WorldToScreen(vp, w, W, H, s[k]);
        }

        static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const ImU32 col = IM_COL32(255, 210, 40, 230);
        for (auto& e : edges)
            if (ok[e[0]] && ok[e[1]]) dl->AddLine(s[e[0]], s[e[1]], col, 1.6f);
    }

    // ImGuizmo over the selected doodad. The engine's row-major view/projection and the
    // doodad's world matrix feed ImGuizmo directly (same row-vector convention); the
    // result is written back live.
    void Placement::DrawGizmo()
    {
        auto& host = ImGuiHostExt::Instance();
        host.SetGizmoBusy(false);
        if (!selected_)
        {
            gizmoWasUsing_ = false;
            return;
        }

        RuntimePlacement* runtime = FindRuntime(selected_);
        if (!runtime) return;
        void* primary = PrimaryObject(*runtime);
        if (!primary) return;

        float model[16];
        if (!doodad::WorldMatrix(primary, model)) return;

        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

        ImGuizmo::Manipulate(camera::GetView(), camera::GetProjection(),
                             static_cast<ImGuizmo::OPERATION>(gizmoOp_), ImGuizmo::WORLD, model);

        // Feed the input router: while the cursor is over / dragging the gizmo, world
        // clicks must not be turned into picks.
        const bool usingNow = ImGuizmo::IsUsing();
        host.SetGizmoBusy(ImGuizmo::IsOver() || usingNow);
        if (usingNow) ApplyGroupMatrix(*runtime, model);
        else if (gizmoWasUsing_) PersistSelectedTransform();
        gizmoWasUsing_ = usingNow;
    }

    // Resolve a screen click into a doodad: for each near-camera doodad, transform its
    // real local AABB by the live instance matrix, project the 8 world corners, and select
    // the nearest whose projected rectangle contains the cursor.
    void Placement::DoPick(int sx, int sy)
    {
        PrunePlaced();
        const int groupCount = static_cast<int>(placed_.size());
        int partCount = 0;
        for (const RuntimePlacement& runtime : placed_)
            partCount += static_cast<int>(runtime.objects.size());

        const float* vp = camera::GetViewProj();
        const ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;
        const float cx = static_cast<float>(sx), cy = static_cast<float>(sy);

        void* best = nullptr;
        float bestDepth = 1e18f, bestSpanX = 0.0f, bestSpanY = 0.0f;
        int boxed = 0;
        for (RuntimePlacement& runtime : placed_)
        {
            void* groupPrimary = PrimaryObject(runtime);
            if (!groupPrimary) continue;
            for (void* candidate : runtime.objects)
            {
                float m[16], lo[3], hi[3];
                if (!doodad::WorldMatrix(candidate, m)) continue;
                if (!doodad::LocalBounds(candidate, lo, hi)) continue;
                ++boxed;

                float rminx = 1e9f, rminy = 1e9f, rmaxx = -1e9f, rmaxy = -1e9f, depth = 1e18f;
                int frontCorners = 0;
                for (int k = 0; k < 8; ++k)
                {
                    const float l[3] = { (k & 1) ? hi[0] : lo[0],
                                         (k & 2) ? hi[1] : lo[1],
                                         (k & 4) ? hi[2] : lo[2] };
                    float c[3];
                    LocalToWorld(m, l, c);
                    float clip[4];
                    for (int j = 0; j < 4; ++j)
                        clip[j] = c[0] * vp[j] + c[1] * vp[4 + j] + c[2] * vp[8 + j] + vp[12 + j];
                    if (clip[3] <= 0.001f) continue;
                    ++frontCorners;
                    const float px = (clip[0] / clip[3] * 0.5f + 0.5f) * W;
                    const float py = (0.5f - clip[1] / clip[3] * 0.5f) * H;
                    rminx = (px < rminx) ? px : rminx;
                    rmaxx = (px > rmaxx) ? px : rmaxx;
                    rminy = (py < rminy) ? py : rminy;
                    rmaxy = (py > rmaxy) ? py : rmaxy;
                    depth = (clip[3] < depth) ? clip[3] : depth;
                }
                if (frontCorners < 4) continue;
                if (cx >= rminx && cx <= rmaxx && cy >= rminy && cy <= rmaxy && depth < bestDepth)
                {
                    bestDepth = depth; best = groupPrimary;
                    bestSpanX = rmaxx - rminx; bestSpanY = rmaxy - rminy;
                }
            }
        }
        WLOG_INFO("pick: cursor=%d,%d groups=%d parts=%d boxed=%d selected=%s span=%.0fx%.0f",
                  sx, sy, groupCount, partCount, boxed, best ? "yes" : "no", bestSpanX, bestSpanY);
        selected_ = best; // clicking empty world deselects
    }
}
