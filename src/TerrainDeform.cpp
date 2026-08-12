// Client-local terrain deformation brush for wxl-housing.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "TerrainDeform.hpp"

#include "ExtensionApi.hpp"

#include "game/Adt.hpp"
#include "game/Camera.hpp"
#include "game/Doodad.hpp"
#include "game/Loading.hpp"
#include "game/Pick.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

namespace wxl_housing
{
    namespace
    {
        namespace adt    = wxl::game::adt;
        namespace camera = wxl::game::camera;
        namespace detail = wxl::game::doodad::detail;
        namespace world  = wxl::game::world;

        constexpr char kSavePath[] = "Extensions\\wxl-housing\\terrain_deform.tsv";
        constexpr char kTempPath[] = "Extensions\\wxl-housing\\terrain_deform.tsv.tmp";
        constexpr char kBackupPath[] = "Extensions\\wxl-housing\\terrain_deform.tsv.bak";

        // ADT fourccs are byte-reversed in the served file buffer. These exact
        // constants are the values ProcessIffChunks compares in build 12340.
        constexpr uint32_t kMcnkTag = 0x4D434E4Bu; // file bytes "KNCM" (MCNK)
        constexpr uint32_t kMcvtTag = 0x4D435654u; // file bytes "TVCM" (MCVT)
        constexpr size_t kMcnkHeaderBytes = 0x80;
        constexpr size_t kVertexCount = 145;
        constexpr size_t kMcvtBytes = kVertexCount * sizeof(float);
        constexpr uint32_t kMaximumMcnkBytes = 4u * 1024u * 1024u;

        // Build-12340 MapChunk landmarks, byte-checked against WarcraftXL v1.1's
        // offsets/game/ADT.hpp and the stock ChunkBuild body at 0x007C64B0.
        constexpr size_t kChunkGlobalX = 0x34;
        constexpr size_t kChunkGlobalY = 0x38;
        constexpr size_t kChunkBboxMin = 0x4C; // float xyz
        constexpr size_t kChunkBboxMax = 0x58; // float xyz
        constexpr size_t kChunkMcvt = 0x11C;   // float[145]*

        using ChunkBuildFn = void(__fastcall*)(void* chunk, void* edx, void* rawMcnk, int param2);

        bool FiniteHeight(float value)
        {
            return std::isfinite(value) && value > -10000.0f && value < 10000.0f;
        }

        bool FiniteWorld(float value)
        {
            // Azeroth/Outland coordinates legitimately exceed 10,000 yards
            // (Eversong is around X=10,416). Keep height validation narrow, but
            // use the client's full coordinate envelope for world X/Y/Z.
            return std::isfinite(value) && value > -64000.0f && value < 64000.0f;
        }

        template <class T>
        bool ReadAt(const void* base, size_t offset, T& value)
        {
            const char* address = static_cast<const char*>(base) + offset;
            if (!detail::Readable(address, sizeof(T))) return false;
            std::memcpy(&value, address, sizeof(T));
            return true;
        }

        float Clamp(float value, float minimum, float maximum)
        {
            return (std::max)(minimum, (std::min)(value, maximum));
        }

        bool WorldToScreen(const float worldPosition[3], ImVec2& output)
        {
            const float* matrix = camera::GetViewProj();
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (!matrix || display.x <= 1.0f || display.y <= 1.0f) return false;

            float clip[4]{};
            for (int column = 0; column < 4; ++column)
                clip[column] = worldPosition[0] * matrix[column] +
                               worldPosition[1] * matrix[4 + column] +
                               worldPosition[2] * matrix[8 + column] + matrix[12 + column];
            if (!std::isfinite(clip[3]) || clip[3] <= 0.001f) return false;

            output.x = (clip[0] / clip[3] * 0.5f + 0.5f) * display.x;
            output.y = (0.5f - clip[1] / clip[3] * 0.5f) * display.y;
            return std::isfinite(output.x) && std::isfinite(output.y);
        }
    }

    struct TerrainDeform::Impl
    {
        struct Key
        {
            int mapId = -1;
            int globalX = -1;
            int globalY = -1;

            bool operator<(const Key& other) const
            {
                if (mapId != other.mapId) return mapId < other.mapId;
                if (globalX != other.globalX) return globalX < other.globalX;
                return globalY < other.globalY;
            }
            bool operator==(const Key& other) const
            {
                return mapId == other.mapId && globalX == other.globalX && globalY == other.globalY;
            }
        };

        struct Record
        {
            std::array<float, kVertexCount> targets{};
            std::bitset<kVertexCount> changed;
        };

        struct UndoEntry
        {
            Key key;
            bool existed = false;
            Record previous;
        };

        struct RuntimeChunk
        {
            void* object = nullptr;
            Key key;
            float* heights = nullptr;
            float minX = 0.0f;
            float minY = 0.0f;
            float maxX = 0.0f;
            float maxY = 0.0f;
        };

        static ChunkBuildFn originalChunkBuild;

        TerrainDeform* owner = nullptr;
        std::map<Key, Record> records;
        std::deque<std::vector<UndoEntry>> undo;
        std::set<Key> waitingKeys;
        bool waitingForReload = false;
        bool observedReload = false;
        int reloadTimeoutFrames = 0;
        size_t lastVertexCount = 0;
        uint64_t hookCalls = 0;
        uint64_t successfulPatches = 0;
        uint64_t rawValidationFailures = 0;
        uint64_t hookCallsAtReload = 0;
        uint64_t successfulPatchesAtReload = 0;
        uint64_t rawFailuresAtReload = 0;
        const char* lastRawFailure = "none";
        world::WorldHit hover{};
        bool hoverValid = false;
        int hoverHitType = 0;
        Key hoverKey{};
        bool hoverHasKey = false;
        std::string statusStorage = "Terrain deformation ready";

        void SetStatus(const std::string& value)
        {
            statusStorage = value;
            owner->status_ = statusStorage.c_str();
        }

        static void __fastcall ChunkBuildDetour(void* chunk, void* edx, void* rawMcnk, int param2)
        {
            TerrainDeform& service = TerrainDeform::Instance();
            if (service.impl_) service.impl_->PatchRawChunk(chunk, rawMcnk);
            if (originalChunkBuild) originalChunkBuild(chunk, edx, rawMcnk, param2);
        }

        bool ChunkKey(void* chunk, Key& key) const
        {
            int globalX = -1;
            int globalY = -1;
            if (!chunk || !ReadAt(chunk, kChunkGlobalX, globalX) ||
                !ReadAt(chunk, kChunkGlobalY, globalY))
                return false;
            if (globalX < 0 || globalX >= 1024 || globalY < 0 || globalY >= 1024)
                return false;
            const int mapId = world::MapId();
            if (mapId < 0 || mapId > 100000) return false;
            key = { mapId, globalX, globalY };
            return true;
        }

        static float* FindRawMcvt(void* rawMcnk, const char*& failure)
        {
            failure = "unknown";
            if (!rawMcnk || !detail::Readable(rawMcnk, 8 + kMcnkHeaderBytes))
            {
                failure = "MCNK header is unreadable";
                return nullptr;
            }
            uint32_t tag = 0;
            uint32_t outerSize = 0;
            std::memcpy(&tag, rawMcnk, sizeof(tag));
            std::memcpy(&outerSize, static_cast<char*>(rawMcnk) + 4, sizeof(outerSize));
            if (tag != kMcnkTag)
            {
                failure = "MCNK tag mismatch";
                return nullptr;
            }
            if (outerSize < kMcnkHeaderBytes + 8 + kMcvtBytes ||
                outerSize > kMaximumMcnkBytes)
            {
                failure = "MCNK size is outside the validated range";
                return nullptr;
            }

            char* cursor = static_cast<char*>(rawMcnk) + 8 + kMcnkHeaderBytes;
            char* end = static_cast<char*>(rawMcnk) + 8 + outerSize;
            if (end <= cursor || !detail::Readable(rawMcnk, 8 + outerSize))
            {
                failure = "MCNK body is unreadable";
                return nullptr;
            }

            for (int guard = 0; cursor + 8 <= end && guard < 64; ++guard)
            {
                uint32_t subTag = 0;
                uint32_t subSize = 0;
                std::memcpy(&subTag, cursor, sizeof(subTag));
                std::memcpy(&subSize, cursor + 4, sizeof(subSize));
                if (subSize > static_cast<uint32_t>(end - cursor - 8))
                {
                    failure = "MCNK subchunk size crosses the chunk boundary";
                    return nullptr;
                }
                if (subTag == kMcvtTag)
                {
                    if (subSize != kMcvtBytes)
                    {
                        failure = "MCVT is not exactly 145 floats";
                        return nullptr;
                    }
                    if (!detail::Writable(cursor + 8, kMcvtBytes))
                    {
                        failure = "MCVT payload is not writable";
                        return nullptr;
                    }
                    failure = "none";
                    return reinterpret_cast<float*>(cursor + 8);
                }
                cursor += 8 + subSize;
            }
            failure = "MCVT subchunk was not found";
            return nullptr;
        }

        void PatchRawChunk(void* chunk, void* rawMcnk)
        {
            ++hookCalls;
            Key key;
            if (!ChunkKey(chunk, key)) return;
            const auto found = records.find(key);
            const bool hasRecord = found != records.end() && found->second.changed.any();
            const bool waitingForKey = waitingForReload && waitingKeys.find(key) != waitingKeys.end();
            if (!hasRecord && !waitingForKey) return;

            const char* failure = "unknown";
            float* heights = FindRawMcvt(rawMcnk, failure);
            if (!heights)
            {
                ++rawValidationFailures;
                lastRawFailure = failure;
                WLOG_WARN("terrain: rejected raw chunk map=%d chunk=%d,%d: %s",
                          key.mapId, key.globalX, key.globalY, failure);
                return;
            }
            if (hasRecord)
            {
                const Record& record = found->second;
                for (size_t index = 0; index < kVertexCount; ++index)
                    if (record.changed.test(index) && FiniteHeight(record.targets[index]))
                        heights[index] = record.targets[index];
                ++successfulPatches;
            }

            // Both edits and recordless Revert/Undo count as rebuilt only after
            // their raw MCVT was validated. A malformed/unwritable chunk must time
            // out instead of falsely reporting that the request is visible.
            if (waitingForKey)
            {
                waitingKeys.erase(key);
                if (waitingKeys.empty()) observedReload = true;
            }
        }

        bool ReadRuntimeChunk(void* object, RuntimeChunk& output) const
        {
            Key key;
            if (!ChunkKey(object, key)) return false;

            float* heights = nullptr;
            if (!ReadAt(object, kChunkMcvt, heights) || !detail::Readable(heights, kMcvtBytes))
                return false;

            float minimum[3]{};
            float maximum[3]{};
            if (!ReadAt(object, kChunkBboxMin, minimum) ||
                !ReadAt(object, kChunkBboxMax, maximum))
                return false;
            if (!FiniteWorld(minimum[0]) || !FiniteWorld(minimum[1]) ||
                !FiniteWorld(maximum[0]) || !FiniteWorld(maximum[1]))
                return false;

            output.object = object;
            output.key = key;
            output.heights = heights;
            output.minX = (std::min)(minimum[0], maximum[0]);
            output.minY = (std::min)(minimum[1], maximum[1]);
            output.maxX = (std::max)(minimum[0], maximum[0]);
            output.maxY = (std::max)(minimum[1], maximum[1]);
            const float width = output.maxX - output.minX;
            const float height = output.maxY - output.minY;
            return width > 20.0f && width < 50.0f && height > 20.0f && height < 50.0f;
        }

        std::vector<RuntimeChunk> ChunksAround(float x, float y, float z, float radius) const
        {
            std::vector<RuntimeChunk> result;
            std::set<void*> seen;
            const float offsets[] = { -radius, 0.0f, radius };
            for (float dx : offsets)
            for (float dy : offsets)
            {
                float query[3] = { x + dx, y + dy, z };
                void* object = adt::GetChunk(query);
                if (!object || !seen.insert(object).second) continue;
                RuntimeChunk chunk;
                if (ReadRuntimeChunk(object, chunk)) result.push_back(chunk);
            }
            return result;
        }

        static void VertexPosition(const RuntimeChunk& chunk, int row, int column,
                                   float& x, float& y, size_t& index)
        {
            if ((row & 1) == 0)
            {
                index = static_cast<size_t>(row / 2 * 17 + column);
                x = chunk.minX + (chunk.maxX - chunk.minX) * (static_cast<float>(column) / 8.0f);
            }
            else
            {
                index = static_cast<size_t>(row / 2 * 17 + 9 + column);
                x = chunk.minX + (chunk.maxX - chunk.minX) *
                    ((static_cast<float>(column) + 0.5f) / 8.0f);
            }
            y = chunk.minY + (chunk.maxY - chunk.minY) * (static_cast<float>(row) / 16.0f);
        }

        void PushUndo(std::vector<UndoEntry>&& entries)
        {
            if (entries.empty()) return;
            undo.push_back(std::move(entries));
            while (undo.size() > 32) undo.pop_front();
        }

        bool ApplyBrush(const world::WorldHit& hit)
        {
            if (waitingForReload)
            {
                SetStatus("Terrain is still rebuilding; wait for the brush to turn ready");
                return false;
            }

            std::vector<RuntimeChunk> chunks = ChunksAround(
                hit.pos.x, hit.pos.y, hit.pos.z, owner->radius_);
            if (chunks.empty())
            {
                SetStatus("No writable terrain chunk was found under the brush");
                return false;
            }

            std::vector<UndoEntry> before;
            std::set<Key> captured;
            std::set<Key> changedKeys;
            size_t changedVertices = 0;
            const float direction = owner->mode_ == Mode::Raise ? 1.0f : -1.0f;

            for (RuntimeChunk& chunk : chunks)
            {
                bool touchedChunk = false;
                for (int row = 0; row <= 16; ++row)
                {
                    const int columns = (row & 1) == 0 ? 9 : 8;
                    for (int column = 0; column < columns; ++column)
                    {
                        float vertexX = 0.0f;
                        float vertexY = 0.0f;
                        size_t index = 0;
                        VertexPosition(chunk, row, column, vertexX, vertexY, index);
                        if (index >= kVertexCount) continue;
                        const float dx = vertexX - hit.pos.x;
                        const float dy = vertexY - hit.pos.y;
                        const float distance = std::sqrt(dx * dx + dy * dy);
                        if (distance > owner->radius_) continue;

                        if (!captured.count(chunk.key))
                        {
                            const auto existing = records.find(chunk.key);
                            UndoEntry entry;
                            entry.key = chunk.key;
                            entry.existed = existing != records.end();
                            if (entry.existed) entry.previous = existing->second;
                            before.push_back(entry);
                            captured.insert(chunk.key);
                        }

                        const float current = chunk.heights[index];
                        if (!FiniteHeight(current)) continue;
                        const float normalized = 1.0f - distance / owner->radius_;
                        const float falloff = normalized * normalized * (3.0f - 2.0f * normalized);
                        const float next = Clamp(current + direction * owner->strength_ * falloff,
                                                 -9999.0f, 9999.0f);
                        Record& record = records[chunk.key];
                        record.targets[index] = next;
                        record.changed.set(index);
                        touchedChunk = true;
                        ++changedVertices;
                    }
                }
                if (touchedChunk) changedKeys.insert(chunk.key);
            }

            if (changedVertices == 0)
            {
                SetStatus("The brush did not cover a terrain vertex");
                return false;
            }

            PushUndo(std::move(before));
            lastVertexCount = changedVertices;
            Save();
            RequestReload(changedKeys);

            std::ostringstream message;
            message << (owner->mode_ == Mode::Raise ? "Raised " : "Lowered ")
                    << changedVertices << " terrain vertices; rebuilding terrain";
            SetStatus(message.str());
            WLOG_INFO("terrain: %s %zu vertices across %zu chunks at (%.2f %.2f %.2f)",
                      owner->mode_ == Mode::Raise ? "raised" : "lowered", changedVertices,
                      changedKeys.size(), hit.pos.x, hit.pos.y, hit.pos.z);
            return true;
        }

        void RequestReload(const std::set<Key>& keys)
        {
            waitingKeys = keys;
            waitingForReload = true;
            observedReload = false;
            reloadTimeoutFrames = 300;
            hookCallsAtReload = hookCalls;
            successfulPatchesAtReload = successfulPatches;
            rawFailuresAtReload = rawValidationFailures;
            world::RequestTerrainReload();
        }

        void FinishReloadIfReady()
        {
            if (!waitingForReload) return;
            if (observedReload)
            {
                waitingForReload = false;
                waitingKeys.clear();
                observedReload = false;
                reloadTimeoutFrames = 0;
                std::ostringstream message;
                message << "Terrain ready (" << lastVertexCount << " vertices changed)";
                SetStatus(message.str());
                return;
            }
            if (--reloadTimeoutFrames <= 0)
            {
                waitingForReload = false;
                waitingKeys.clear();
                std::ostringstream message;
                if (hookCalls == hookCallsAtReload)
                    message << "Terrain reload did not invoke ChunkBuild; edits are saved for relog";
                else if (rawValidationFailures > rawFailuresAtReload)
                    message << "Terrain reload rejected raw MCVT: " << lastRawFailure;
                else if (successfulPatches == successfulPatchesAtReload)
                    message << "Terrain reload rebuilt other chunks but not the edited chunk";
                else
                    message << "Terrain reload timed out waiting for every edited chunk";
                SetStatus(message.str());
                WLOG_WARN("terrain: rebuild timeout hooks=%llu patches=%llu rawFailures=%llu reason=%s",
                          static_cast<unsigned long long>(hookCalls - hookCallsAtReload),
                          static_cast<unsigned long long>(successfulPatches - successfulPatchesAtReload),
                          static_cast<unsigned long long>(rawValidationFailures - rawFailuresAtReload),
                          lastRawFailure);
            }
        }

        bool NormalizeSurfaceHit(world::WorldHit& hit, int hitType, const char*& failure) const
        {
            failure = "unknown";
            if (hitType == 0)
            {
                failure = "the cursor ray missed the world";
                return false;
            }
            if (!FiniteWorld(hit.pos.x) || !FiniteWorld(hit.pos.y) || !FiniteWorld(hit.pos.z))
            {
                failure = "the cursor hit returned invalid world coordinates";
                return false;
            }

            // Native picking can stop on an M2 (type 2). Terrain deformation still
            // needs the terrain/WMO surface below it, so resolve a vertical surface
            // at the same X/Y instead of rejecting every click over foliage/decor.
            if (hitType == 2)
            {
                float surfaceZ = hit.pos.z;
                if (world::GroundZ(hit.pos.x, hit.pos.y, hit.pos.z, surfaceZ))
                    hit.pos.z = surfaceZ;
            }

            float query[3] = { hit.pos.x, hit.pos.y, hit.pos.z };
            if (!adt::GetChunk(query))
            {
                failure = "no loaded terrain chunk exists at the cursor hit";
                return false;
            }
            failure = "none";
            return true;
        }

        bool SurfaceAtScreen(float x, float y, world::WorldHit& hit, int& hitType,
                             const char*& failure) const
        {
            hitType = world::Pick(x, y, hit);
            return NormalizeSurfaceHit(hit, hitType, failure);
        }

        bool PickAtScreen(int x, int y, world::WorldHit& hit, const char*& failure) const
        {
            int hitType = 0;
            return SurfaceAtScreen(static_cast<float>(x), static_cast<float>(y),
                                   hit, hitType, failure);
        }

        void UpdateHover()
        {
            const char* failure = "unknown";
            // Use the engine's normalized live cursor path for the preview. This
            // avoids client-pixel/DDC scaling differences and makes the cached hit
            // authoritative for the next queued click.
            hoverHitType = world::PickCursor(hover);
            hoverValid = NormalizeSurfaceHit(hover, hoverHitType, failure);
            hoverHasKey = false;
            if (!hoverValid)
            {
                hoverHitType = 0;
                return;
            }
            float query[3] = { hover.pos.x, hover.pos.y, hover.pos.z };
            void* chunk = adt::GetChunk(query);
            hoverHasKey = ChunkKey(chunk, hoverKey);
        }

        void DrawPreview() const
        {
            if (!owner->active_) return;
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImU32 color = owner->mode_ == Mode::Raise
                ? IM_COL32(70, 255, 115, 255) : IM_COL32(255, 82, 58, 255);
            const ImU32 softColor = owner->mode_ == Mode::Raise
                ? IM_COL32(45, 220, 90, 52) : IM_COL32(255, 70, 45, 52);

            // Always show an unmistakable armed-brush cursor. If the world ray is
            // invalid, the amber crossed circle explains that a stroke cannot land.
            if (!hoverValid)
            {
                if (std::isfinite(mouse.x) && std::isfinite(mouse.y) &&
                    mouse.x >= 0.0f && mouse.y >= 0.0f)
                {
                    const ImU32 amber = IM_COL32(255, 190, 55, 255);
                    draw->AddCircleFilled(mouse, 30.0f, IM_COL32(80, 50, 0, 80), 40);
                    draw->AddCircle(mouse, 30.0f, amber, 40, 3.0f);
                    draw->AddLine(ImVec2(mouse.x - 18.0f, mouse.y - 18.0f),
                                  ImVec2(mouse.x + 18.0f, mouse.y + 18.0f), amber, 3.0f);
                    draw->AddLine(ImVec2(mouse.x - 18.0f, mouse.y + 18.0f),
                                  ImVec2(mouse.x + 18.0f, mouse.y - 18.0f), amber, 3.0f);
                    draw->AddText(ImVec2(mouse.x + 38.0f, mouse.y - 9.0f), amber,
                                  "NO TERRAIN HIT");
                }
                return;
            }

            constexpr int segments = 64;
            auto projectedRing = [&](float radius, std::vector<ImVec2>& points) {
                points.clear();
                points.reserve(segments);
                for (int i = 0; i < segments; ++i)
                {
                    const float angle = static_cast<float>(i) * 6.28318530718f /
                                        static_cast<float>(segments);
                    const float point[3] = {
                        hover.pos.x + std::cos(angle) * radius,
                        hover.pos.y + std::sin(angle) * radius,
                        hover.pos.z + 0.18f,
                    };
                    ImVec2 screen{};
                    if (!WorldToScreen(point, screen)) return false;
                    points.push_back(screen);
                }
                return points.size() == static_cast<size_t>(segments);
            };

            std::vector<ImVec2> outer;
            if (projectedRing(owner->radius_, outer))
            {
                draw->AddConvexPolyFilled(outer.data(), static_cast<int>(outer.size()), softColor);
                draw->AddPolyline(outer.data(), static_cast<int>(outer.size()), color, true, 3.5f);

                std::vector<ImVec2> middle;
                std::vector<ImVec2> inner;
                if (projectedRing(owner->radius_ * 0.66f, middle))
                    draw->AddPolyline(middle.data(), static_cast<int>(middle.size()),
                                      IM_COL32(255, 255, 255, 175), true, 1.5f);
                if (projectedRing(owner->radius_ * 0.33f, inner))
                    draw->AddPolyline(inner.data(), static_cast<int>(inner.size()),
                                      color, true, 1.5f);
            }

            float centerWorld[3] = { hover.pos.x, hover.pos.y, hover.pos.z + 0.22f };
            ImVec2 center{};
            if (WorldToScreen(centerWorld, center))
            {
                draw->AddCircleFilled(center, 8.0f, IM_COL32(0, 0, 0, 210), 20);
                draw->AddCircle(center, 11.0f, color, 24, 3.0f);
                draw->AddLine(ImVec2(center.x - 24.0f, center.y),
                              ImVec2(center.x + 24.0f, center.y), color, 2.5f);
                draw->AddLine(ImVec2(center.x, center.y - 24.0f),
                              ImVec2(center.x, center.y + 24.0f), color, 2.5f);
                const char* label = owner->mode_ == Mode::Raise ? "RAISE" : "LOWER";
                char detailText[128]{};
                std::snprintf(detailText, sizeof(detailText),
                              "%s  %.1f yd  strength %.2f  |  CLICK TO APPLY",
                              label, owner->radius_, owner->strength_);
                const ImVec2 textSize = ImGui::CalcTextSize(detailText);
                const ImVec2 textMin(center.x - textSize.x * 0.5f - 8.0f, center.y + 30.0f);
                const ImVec2 textMax(textMin.x + textSize.x + 16.0f, textMin.y + textSize.y + 8.0f);
                draw->AddRectFilled(textMin, textMax, IM_COL32(0, 0, 0, 205), 4.0f);
                draw->AddRect(textMin, textMax, color, 4.0f, 0, 2.0f);
                draw->AddText(ImVec2(textMin.x + 8.0f, textMin.y + 4.0f), color, detailText);
            }
        }

        bool RevertHovered()
        {
            if (!hoverHasKey || waitingForReload) return false;
            const auto found = records.find(hoverKey);
            if (found == records.end())
            {
                SetStatus("This terrain chunk has no saved deformation");
                return false;
            }

            std::vector<UndoEntry> entries(1);
            entries[0].key = hoverKey;
            entries[0].existed = true;
            entries[0].previous = found->second;
            records.erase(found);
            PushUndo(std::move(entries));
            Save();
            RequestReload(std::set<Key>{ hoverKey });
            SetStatus("Reverting the terrain chunk under the brush");
            WLOG_INFO("terrain: reverted map=%d chunk=%d,%d", hoverKey.mapId,
                      hoverKey.globalX, hoverKey.globalY);
            return true;
        }

        bool UndoLast()
        {
            if (undo.empty() || waitingForReload) return false;
            std::vector<UndoEntry> entries = std::move(undo.back());
            undo.pop_back();
            std::set<Key> changedKeys;
            for (const UndoEntry& entry : entries)
            {
                if (entry.existed) records[entry.key] = entry.previous;
                else records.erase(entry.key);
                changedKeys.insert(entry.key);
            }
            Save();
            RequestReload(changedKeys);
            SetStatus("Undoing the previous terrain brush stroke");
            WLOG_INFO("terrain: undo restored %zu chunks", changedKeys.size());
            return true;
        }

        void Load()
        {
            std::ifstream input(kSavePath, std::ios::binary);
            if (!input) return;
            input.seekg(0, std::ios::end);
            const std::streamoff size = input.tellg();
            if (size < 0 || size > 8 * 1024 * 1024)
            {
                WLOG_WARN("terrain: ignored oversized deformation file");
                return;
            }
            input.seekg(0, std::ios::beg);

            std::string line;
            size_t loaded = 0;
            size_t rejected = 0;
            while (std::getline(input, line))
            {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream row(line);
                Key key;
                int index = -1;
                float target = 0.0f;
                std::string extra;
                if (!(row >> key.mapId >> key.globalX >> key.globalY >> index >> target) ||
                    (row >> extra) || key.mapId < 0 || key.mapId > 100000 ||
                    key.globalX < 0 || key.globalX >= 1024 ||
                    key.globalY < 0 || key.globalY >= 1024 ||
                    index < 0 || index >= static_cast<int>(kVertexCount) || !FiniteHeight(target))
                {
                    ++rejected;
                    continue;
                }
                Record& record = records[key];
                record.targets[static_cast<size_t>(index)] = target;
                record.changed.set(static_cast<size_t>(index));
                ++loaded;
            }
            WLOG_INFO("terrain: loaded %zu vertex edits across %zu chunks (%zu rejected)",
                      loaded, records.size(), rejected);
        }

        bool Save()
        {
            std::ofstream output(kTempPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                SetStatus("Could not write the terrain deformation file");
                WLOG_ERROR("terrain: could not open %s", kTempPath);
                return false;
            }
            output << "# wxl-housing terrain deformation v1\n";
            output << "# mapId\tglobalChunkX\tglobalChunkY\tvertexIndex\trelativeHeight\n";
            output << std::setprecision(9);
            for (const auto& pair : records)
            {
                for (size_t index = 0; index < kVertexCount; ++index)
                {
                    if (!pair.second.changed.test(index)) continue;
                    output << pair.first.mapId << '\t' << pair.first.globalX << '\t'
                           << pair.first.globalY << '\t' << index << '\t'
                           << pair.second.targets[index] << '\n';
                }
            }
            output.flush();
            if (!output.good())
            {
                output.close();
                DeleteFileA(kTempPath);
                SetStatus("Terrain deformation save failed before replace");
                return false;
            }
            output.close();
            CopyFileA(kSavePath, kBackupPath, FALSE);
            if (!MoveFileExA(kTempPath, kSavePath,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                SetStatus("Terrain deformation atomic save failed");
                WLOG_ERROR("terrain: atomic replace failed error=%lu", GetLastError());
                return false;
            }
            return true;
        }
    };

    ChunkBuildFn TerrainDeform::Impl::originalChunkBuild = nullptr;

    TerrainDeform& TerrainDeform::Instance()
    {
        static TerrainDeform instance;
        return instance;
    }

    bool TerrainDeform::Initialize()
    {
        if (initialized_) return true;
        if (!g_api || !g_api->HookAttachByName)
        {
            status_ = "Terrain deformation unavailable (named hook service missing)";
            return false;
        }

        if (!impl_)
        {
            impl_ = new Impl();
            impl_->owner = this;
            impl_->Load();
        }
        const int attached = g_api->HookAttachByName(
            "Adt.ChunkBuild", reinterpret_cast<void*>(&Impl::ChunkBuildDetour),
            reinterpret_cast<void**>(&Impl::originalChunkBuild), WXL_HOOK_DEFAULT_PRIORITY);
        // HookAttachByName registers the chain during WXL_Load. The core fills the
        // trampoline when it enables all registered hooks after extension loading,
        // so originalChunkBuild is expected to still be null right here.
        if (!attached)
        {
            impl_->SetStatus("Terrain deformation unavailable (Adt.ChunkBuild hook failed)");
            WLOG_ERROR("terrain: Adt.ChunkBuild named hook failed");
            return false;
        }

        initialized_ = true;
        impl_->SetStatus("Terrain deformation ready");
        WLOG_INFO("terrain: brush ready; saved edits=%zu chunks", impl_->records.size());
        return true;
    }

    void TerrainDeform::SetActive(bool active)
    {
        if (active && !initialized_)
        {
            active_ = false;
            return;
        }
        active_ = active;
        if (!active_)
        {
            clickPending_ = false;
            if (impl_) impl_->SetStatus("Terrain deformation paused");
        }
        else if (impl_)
            impl_->SetStatus("Terrain brush active - click the world to deform");
    }

    bool TerrainDeform::HandleScreenClick(int clientX, int clientY)
    {
        if (!active_ || !initialized_) return false;
        if (clientX < 0 || clientY < 0) return true;
        clickX_ = clientX;
        clickY_ = clientY;
        clickPending_ = true;
        return true;
    }

    void TerrainDeform::OnEndScene()
    {
        if (!impl_ || !initialized_) return;
        impl_->FinishReloadIfReady();
        if (!active_) return;

        impl_->UpdateHover();
        impl_->DrawPreview();
        if (!clickPending_) return;

        const int x = clickX_;
        const int y = clickY_;
        clickPending_ = false;
        world::WorldHit hit;
        const char* failure = "unknown";
        bool picked = false;
        if (impl_->hoverValid)
        {
            // The preview is resolved from the engine/ImGui cursor during this
            // exact frame. Reuse it so the visible disc is precisely the stroke
            // target and client-pixel vs DDC scaling cannot move the click.
            hit = impl_->hover;
            picked = true;
        }
        else
            picked = impl_->PickAtScreen(x, y, hit, failure);
        if (!picked)
        {
            std::ostringstream message;
            message << "Terrain stroke rejected: " << failure;
            impl_->SetStatus(message.str());
            WLOG_WARN("terrain: click %d,%d rejected: %s", x, y, failure);
            return;
        }
        impl_->ApplyBrush(hit);
    }

    void TerrainDeform::DrawControls()
    {
        if (!impl_)
        {
            ImGui::TextWrapped("Terrain deformation did not initialize.");
            return;
        }

        ImGui::BeginDisabled(!initialized_);
        if (active_)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.18f, 0.12f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.47f, 0.24f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.62f, 0.31f, 1.0f));
        }
        if (ImGui::Button(active_ ? "STOP TERRAIN DEFORM" : "START TERRAIN DEFORM",
                          ImVec2(210.0f, 32.0f)))
            SetActive(!active_);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (active_)
            ImGui::TextColored(ImVec4(0.40f, 0.86f, 0.48f, 1.0f), "ACTIVE - client-local");
        else
            ImGui::TextDisabled("client-local");

        if (active_)
        {
            if (impl_->hoverValid && impl_->hoverHasKey)
            {
                ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.48f, 1.0f),
                    "BRUSH LOCKED: %.1f, %.1f, %.1f  |  chunk %d,%d",
                    impl_->hover.pos.x, impl_->hover.pos.y, impl_->hover.pos.z,
                    impl_->hoverKey.globalX, impl_->hoverKey.globalY);
            }
            else
                ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.16f, 1.0f),
                                   "NO TERRAIN UNDER CURSOR - move the cursor over loaded ground");

            ImGui::TextDisabled(
                "diag map=%d pick=%d chunk=%s | ChunkBuild=%llu patched=%llu rawfail=%llu | rebuild=%s",
                world::MapId(), impl_->hoverHitType, impl_->hoverHasKey ? "yes" : "no",
                static_cast<unsigned long long>(impl_->hookCalls),
                static_cast<unsigned long long>(impl_->successfulPatches),
                static_cast<unsigned long long>(impl_->rawValidationFailures),
                impl_->waitingForReload ? "waiting" : "ready");
        }

        ImGui::BeginDisabled(!active_ || impl_->waitingForReload);
        int selectedMode = mode_ == Mode::Raise ? 0 : 1;
        if (ImGui::RadioButton("Raise", selectedMode == 0)) mode_ = Mode::Raise;
        ImGui::SameLine();
        if (ImGui::RadioButton("Lower", selectedMode == 1)) mode_ = Mode::Lower;
        ImGui::SliderFloat("Brush radius", &radius_, 2.0f, 24.0f, "%.1f yd");
        ImGui::SliderFloat("Stroke strength", &strength_, 0.10f, 4.0f, "%.2f yd");
        ImGui::EndDisabled();

        ImGui::BeginDisabled(impl_->undo.empty() || impl_->waitingForReload);
        if (ImGui::Button("Undo last stroke")) impl_->UndoLast();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!impl_->hoverHasKey || impl_->waitingForReload ||
                             impl_->records.find(impl_->hoverKey) == impl_->records.end());
        if (ImGui::Button("Revert chunk...")) ImGui::OpenPopup("Revert terrain chunk?");
        ImGui::EndDisabled();

        if (ImGui::BeginPopupModal("Revert terrain chunk?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Remove every saved terrain change from the chunk under the brush?");
            ImGui::TextDisabled("This can be undone once with Undo last stroke.");
            if (ImGui::Button("REVERT", ImVec2(110.0f, 28.0f)))
            {
                impl_->RevertHovered();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Separator();
        ImGui::TextWrapped("%s", Status());
        ImGui::TextDisabled("Green raises, red lowers. Click loaded terrain to apply one smooth stroke. "
                            "Edits persist in terrain_deform.tsv and reapply after restart.");
    }
}
