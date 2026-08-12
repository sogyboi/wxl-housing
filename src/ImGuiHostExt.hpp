// Own ImGui context + D3D9/Win32 backends + window-input router for wxl-housing.
// The core's panel API cannot draw textures or a 3D gizmo, so the extension runs its own
// context (the mini-noggit approach). Insert toggles the editor; while visible, messages
// the editor consumes are swallowed so the game does not also react.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"

struct ImGuiContext;

namespace wxl_housing
{
    class ImGuiHostExt
    {
    public:
        static ImGuiHostExt& Instance();

        void OnEndScene(void* device);
        void OnDeviceLost(const wxl::events::DeviceResetArgs& a);
        void OnDeviceReset(const wxl::events::DeviceResetArgs& a);

        // Visible() deliberately means "safe to issue ImGui draw calls now".
        bool Visible() const { return visible_ && frameActive_; }
        bool Open() const { return visible_; }
        bool Ready() const { return ready_; }
        ImGuiContext* Context() const { return context_; }

        void SetVisible(bool v);
        void ToggleVisible() { SetVisible(!visible_); }
        bool GizmoBusy() const { return gizmoBusy_; }
        void SetGizmoBusy(bool b) { gizmoBusy_ = b; }

        /// Requests a world pick at a client-area point on the next frame (called by the
        /// input router; executed by Placement once DisplaySize is valid).
        void RequestPick(int x, int y) { pickX_ = x; pickY_ = y; pickPending_ = true; }
        bool ConsumePick(int& x, int& y);

    private:
        ImGuiHostExt() = default;

        void EnsureInit(void* device);
        void DrawLauncher(void* device);

        bool ready_ = false;
        bool visible_ = false;
        bool frameActive_ = false;
        bool gizmoBusy_ = false; // cursor over / dragging the gizmo (set each frame)
        bool launcherDragMoved_ = false;
        bool pickPending_ = false;
        int  pickX_ = 0, pickY_ = 0;
        ImGuiContext* context_ = nullptr;
    };
}
