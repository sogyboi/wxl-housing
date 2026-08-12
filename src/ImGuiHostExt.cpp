// ImGui host implementation, ported from wxl-mini-noggit (v1.1 include paths).
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "ImGuiHostExt.hpp"

#include "Catalog.hpp"
#include "ExtensionApi.hpp"
#include "FreeBuildCamera.hpp"
#include "Placement.hpp"
#include "TerrainDeform.hpp"

#include <algorithm>
#include <cmath>
#include <windows.h>
#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "../third_party/imguizmo/ImGuizmo.h" // vendored; see shared.cmake

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace wxl_housing
{
    namespace
    {
        HWND    g_hwnd        = nullptr;
        WNDPROC g_origWndProc = nullptr;
        bool    g_worldLeftDown = false;
        ULONGLONG g_lastInsertToggleMs = 0;

        constexpr float kLauncherWindowSize = 58.0f;
        constexpr float kLauncherEdgeMargin = 12.0f;
        constexpr ULONGLONG kInsertToggleDebounceMs = 250;

        bool IsStockMovementKeyMessage(UINT message, WPARAM key)
        {
            if (message != WM_KEYDOWN && message != WM_KEYUP &&
                message != WM_SYSKEYDOWN && message != WM_SYSKEYUP)
                return false;
            switch (key)
            {
                case 'W': case 'A': case 'S': case 'D':
                case 'Q': case 'E': case VK_SPACE:
                case VK_CONTROL: case VK_SHIFT:
                    return true;
                default:
                    return false;
            }
        }

        float ClampLauncherAxis(float position, float displayExtent)
        {
            if (!std::isfinite(displayExtent) || displayExtent <= 0.0f)
                return position;

            // Keep the complete launcher plus a small edge gap when space permits.
            const float insetMinimum = kLauncherEdgeMargin;
            const float insetMaximum = displayExtent - kLauncherWindowSize - kLauncherEdgeMargin;
            if (insetMaximum >= insetMinimum)
                return (std::max)(insetMinimum, (std::min)(position, insetMaximum));

            // On small displays, prefer the whole launcher without an inset. If even
            // that cannot fit, retain a reachable strip rather than producing an
            // inverted clamp range or allowing the window to disappear completely.
            const float flushMaximum = displayExtent - kLauncherWindowSize;
            if (flushMaximum >= 0.0f)
                return (std::max)(0.0f, (std::min)(position, flushMaximum));

            const float reachable = (std::min)(kLauncherEdgeMargin, displayExtent);
            const float partialMinimum = reachable - kLauncherWindowSize;
            const float partialMaximum = displayExtent - reachable;
            return (std::max)(partialMinimum, (std::min)(position, partialMaximum));
        }

        ImVec2 ClampLauncherPosition(const ImVec2& position, const ImVec2& displaySize)
        {
            return ImVec2(ClampLauncherAxis(position.x, displaySize.x),
                          ClampLauncherAxis(position.y, displaySize.y));
        }

        bool ValidDisplaySize(const ImVec2& displaySize)
        {
            return std::isfinite(displaySize.x) && std::isfinite(displaySize.y) &&
                   displaySize.x > 0.0f && displaySize.y > 0.0f;
        }

        BOOL CALLBACK PickWindow(HWND h, LPARAM out)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId() && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h))
            {
                *reinterpret_cast<HWND*>(out) = h;
                return FALSE;
            }
            return TRUE;
        }

        HWND FindGameWindow()
        {
            HWND h = nullptr;
            EnumWindows(&PickWindow, reinterpret_cast<LPARAM>(&h));
            if (!h) h = FindWindowA("GxWindowClass", nullptr);
            return h;
        }

        // Window-input router. Insert toggles the editor; while visible, messages the
        // editor consumes are swallowed. Chains to any previous subclass (mini-noggit),
        // which in turn chains to the game - coexistence is a LIFO call chain.
        LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            auto& host = ImGuiHostExt::Instance();

            // Ignore key-repeat so holding Insert cannot flicker the editor open/closed.
            if ((m == WM_KEYDOWN || m == WM_KEYUP) && w == VK_INSERT)
            {
                if (m == WM_KEYDOWN && (l & (1u << 30)) == 0)
                {
                    const ULONGLONG now = GetTickCount64();
                    if (!g_lastInsertToggleMs ||
                        now - g_lastInsertToggleMs >= kInsertToggleDebounceMs)
                    {
                        g_lastInsertToggleMs = now;
                        host.ToggleVisible();
                        if (!host.Open()) g_worldLeftDown = false;
                    }
                }
                return 0;
            }
            if (host.Ready())
            {
                // Other WarcraftXL overlays may own a separate ImGui context. Always route
                // housing input through ours, then restore the previous context before chaining.
                ImGuiContext* previousContext = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(host.Context());
                ImGui_ImplWin32_WndProcHandler(h, m, w, l);
                const ImGuiIO& io = ImGui::GetIO();
                bool handled = FreeBuildCamera::Instance().HandleWindowMessage(
                    h, m, w, l, io.WantCaptureMouse, io.WantTextInput);

                // Terrain deformation owns world clicks while its brush is active.
                // A successful stamp is swallowed so WoW does not also move/target.
                if (!handled && host.Open() && TerrainDeform::Instance().Active() &&
                    m == WM_LBUTTONDOWN && !io.WantCaptureMouse)
                {
                    if (TerrainDeform::Instance().HandleScreenClick(
                            static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))))
                    {
                        g_worldLeftDown = true;
                        handled = true;
                    }
                }
                // Otherwise, left click in the world requests a housing doodad pick.
                else if (!handled && host.Open() && m == WM_LBUTTONDOWN && !io.WantCaptureMouse)
                {
                    if (!host.GizmoBusy())
                        host.RequestPick(static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)));
                    g_worldLeftDown = true;
                    handled = true;
                }
                // Do not give WoW an unmatched button-up after swallowing the world click.
                else if (m == WM_LBUTTONUP && g_worldLeftDown)
                {
                    g_worldLeftDown = false;
                    handled = true;
                }
                else if (m == WM_CAPTURECHANGED || m == WM_CANCELMODE)
                    g_worldLeftDown = false;

                // Swallow what the editor consumes (clicks, wheel, keys) but let
                // WM_MOUSEMOVE through, so WoW keeps driving its own native cursor.
                if (io.WantCaptureMouse && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEMOVE)
                    handled = true;
                // A focused ImGui window enables WantCaptureKeyboard for navigation,
                // even when no editor owns text. Swallowing that broad flag strands
                // stock WASD/jump while the free camera is Off. Only text/numeric
                // editors request WantTextInput; keep those safe while ordinary
                // dashboard focus passes movement keys back to WoW.
                if (host.Open() && io.WantTextInput &&
                    !IsStockMovementKeyMessage(m, w) &&
                    ((m >= WM_KEYFIRST && m <= WM_KEYLAST) || m == WM_CHAR))
                    handled = true;

                ImGui::SetCurrentContext(previousContext);
                if (handled) return 0;
            }
            return CallWindowProcA(g_origWndProc, h, m, w, l);
        }
    }

    ImGuiHostExt& ImGuiHostExt::Instance()
    {
        static ImGuiHostExt s;
        return s;
    }

    void ImGuiHostExt::SetVisible(bool v)
    {
        visible_ = v;
        if (!v)
        {
            // A hidden editor must never retain camera or movement-input ownership.
            // Deactivate is idempotent and performs its pointer restore at EndScene.
            FreeBuildCamera::Instance().Deactivate();
            gizmoBusy_ = false;
            pickPending_ = false;
            TerrainDeform::Instance().SetActive(false);
        }
    }

    bool ImGuiHostExt::ConsumePick(int& x, int& y)
    {
        if (!pickPending_) return false;
        pickPending_ = false;
        x = pickX_; y = pickY_;
        return true;
    }

    void ImGuiHostExt::DrawLauncher(void* device)
    {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 initialPosition = ClampLauncherPosition(
            ImVec2((std::max)(kLauncherEdgeMargin,
                              io.DisplaySize.x - kLauncherWindowSize - kLauncherEdgeMargin),
                   260.0f),
            io.DisplaySize);
        ImGui::SetNextWindowPos(initialPosition, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(kLauncherWindowSize, kLauncherWindowSize), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 29.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::Begin("##housing-launcher", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground);

        static ImVec2 previousDisplaySize(-1.0f, -1.0f);
        const bool displaySizeChanged =
            std::fabs(previousDisplaySize.x - io.DisplaySize.x) > 0.5f ||
            std::fabs(previousDisplaySize.y - io.DisplaySize.y) > 0.5f;
        if (displaySizeChanged)
        {
            if (ValidDisplaySize(io.DisplaySize))
                ImGui::SetWindowPos(ClampLauncherPosition(ImGui::GetWindowPos(), io.DisplaySize),
                                    ImGuiCond_Always);
            previousDisplaySize = io.DisplaySize;
        }

        const ImVec2 iconMin = ImGui::GetCursorScreenPos();
        constexpr float iconSize = 50.0f;
        ImGui::InvisibleButton("##housing-building-button", ImVec2(iconSize, iconSize));
        const bool hovered = ImGui::IsItemHovered();

        if (ImGui::IsItemActivated())
            launcherDragMoved_ = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
        {
            launcherDragMoved_ = true;
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 dragged(pos.x + io.MouseDelta.x, pos.y + io.MouseDelta.y);
            ImGui::SetWindowPos(ClampLauncherPosition(dragged, io.DisplaySize), ImGuiCond_Always);
        }
        if (ImGui::IsItemDeactivated())
        {
            if (!launcherDragMoved_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                ToggleVisible();
            if (ValidDisplaySize(io.DisplaySize))
                ImGui::SetWindowPos(ClampLauncherPosition(ImGui::GetWindowPos(), io.DisplaySize),
                                    ImGuiCond_Always);
            launcherDragMoved_ = false;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 center(iconMin.x + iconSize * 0.5f, iconMin.y + iconSize * 0.5f);
        draw->AddCircleFilled(center, 24.5f, IM_COL32(18, 14, 10, 235), 48);

        if (void* texture = Catalog::Instance().LauncherTexture(device))
        {
            // Retail HousingItemCategoryNavigation: structural/building icon.
            // Odd cells are the gold active state, even cells the muted state.
            constexpr float atlas = 1024.0f;
            constexpr float stride = 66.0f;
            constexpr float cell = 64.0f;
            const int cellX = (visible_ || hovered) ? 9 : 10;
            const int cellY = 4;
            const ImVec2 uv0(cellX * stride / atlas, cellY * stride / atlas);
            const ImVec2 uv1((cellX * stride + cell) / atlas,
                             (cellY * stride + cell) / atlas);
            draw->AddImage(reinterpret_cast<ImTextureID>(texture), iconMin,
                           ImVec2(iconMin.x + iconSize, iconMin.y + iconSize), uv0, uv1);
        }
        else
        {
            // Readable fallback if the optional Retail atlas is missing.
            const ImU32 gold = visible_ || hovered
                ? IM_COL32(244, 195, 45, 255) : IM_COL32(178, 151, 104, 255);
            draw->AddCircle(center, 23.0f, gold, 40, 2.0f);
            for (int row = -1; row <= 1; ++row)
            {
                const float y = center.y + row * 7.0f;
                draw->AddLine(ImVec2(center.x - 13.0f, y), ImVec2(center.x + 13.0f, y), gold, 2.0f);
            }
            draw->AddLine(ImVec2(center.x, center.y - 14.0f), ImVec2(center.x, center.y - 7.0f), gold, 2.0f);
            draw->AddLine(ImVec2(center.x - 7.0f, center.y - 7.0f), ImVec2(center.x - 7.0f, center.y), gold, 2.0f);
            draw->AddLine(ImVec2(center.x + 7.0f, center.y), ImVec2(center.x + 7.0f, center.y + 7.0f), gold, 2.0f);
            draw->AddLine(ImVec2(center.x, center.y + 7.0f), ImVec2(center.x, center.y + 14.0f), gold, 2.0f);
        }

        if (visible_ || hovered)
            draw->AddCircle(center, 24.0f, IM_COL32(246, 194, 37, 235), 48, visible_ ? 2.2f : 1.3f);
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Housing Build Menu\nClick to open/close - drag to move");
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    void ImGuiHostExt::EnsureInit(void* device)
    {
        if (ready_) return;
        g_hwnd = FindGameWindow();
        if (!g_hwnd || !device)
        {
            static bool warned = false;
            if (!warned) { warned = true; WLOG_WARN("imgui: init deferred (hwnd=%p device=%p)", g_hwnd, device); }
            return;
        }

        IMGUI_CHECKVERSION();
        ImGuiContext* previousContext = ImGui::GetCurrentContext();
        context_ = ImGui::CreateContext();
        if (!context_)
        {
            ImGui::SetCurrentContext(previousContext);
            WLOG_ERROR("imgui: CreateContext failed");
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(g_hwnd))
        {
            ImGui::DestroyContext(context_);
            context_ = nullptr;
            ImGui::SetCurrentContext(previousContext);
            WLOG_ERROR("imgui: Win32 backend init failed");
            return;
        }
        if (!ImGui_ImplDX9_Init(reinterpret_cast<IDirect3DDevice9*>(device)))
        {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(context_);
            context_ = nullptr;
            ImGui::SetCurrentContext(previousContext);
            WLOG_ERROR("imgui: DX9 backend init failed");
            return;
        }
        ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());

        SetLastError(0);
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
        const DWORD subclassError = GetLastError();
        if (!g_origWndProc)
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(context_);
            context_ = nullptr;
            ImGui::SetCurrentContext(previousContext);
            WLOG_ERROR("imgui: window subclass install failed (error=%lu)", subclassError);
            return;
        }
        ready_ = true;
        ImGui::SetCurrentContext(previousContext);
        WLOG_INFO("imgui: own context ready (hwnd=%p), Insert toggles the editor", g_hwnd);
    }

    void ImGuiHostExt::OnDeviceLost(const wxl::events::DeviceResetArgs&)
    {
        if (!ready_) return;
        ImGuiContext* previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context_);
        Catalog::Instance().OnDeviceLost();
        ImGui_ImplDX9_InvalidateDeviceObjects();
        ImGui::SetCurrentContext(previousContext);
    }

    void ImGuiHostExt::OnDeviceReset(const wxl::events::DeviceResetArgs&)
    {
        if (!ready_) return;
        ImGuiContext* previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context_);
        ImGui_ImplDX9_CreateDeviceObjects();
        ImGui::SetCurrentContext(previousContext);
    }

    void ImGuiHostExt::OnEndScene(void* device)
    {
        EnsureInit(device);
        FreeBuildCamera::Instance().Tick();
        Placement::Instance().TickPersistence();
        frameActive_ = false;
        if (!ready_)
        {
            gizmoBusy_ = false;
            return;
        }

        ImGuiContext* previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context_);

        // Do not draw ImGui's software cursor: WoW's own cursor stays the single visible one.
        ImGui::GetIO().MouseDrawCursor = false;
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        frameActive_ = true;

        // The launcher remains available when the large editor is closed.
        DrawLauncher(device);

        // WantCaptureMouse is finalized by NewFrame. A click can arrive just
        // after the cursor is teleported over an editor window while the
        // WndProc still sees the previous frame's capture flag; cancel that
        // stale world-pick request before the catalog button action runs.
        if (ImGui::GetIO().WantCaptureMouse)
            pickPending_ = false;

        // Own the complete housing frame so no ImGui calls escape NewFrame/Render.
        if (visible_)
        {
            Catalog::Instance().Frame(device);
            TerrainDeform::Instance().OnEndScene();
            Placement::Instance().OnEndScene();
        }
        else
            gizmoBusy_ = false;
        frameActive_ = false;

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        ImGui::SetCurrentContext(previousContext);
    }
}
