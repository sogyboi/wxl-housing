// Detached, client-local build camera for wxl-housing.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "game/Camera.hpp"

namespace wxl_housing
{
    /// A bounded free-fly camera used while decorating. This class never writes the
    /// player object or calls movement APIs; the world frame is lent a cloned camera
    /// object only while Active() is true.
    class FreeBuildCamera
    {
        enum class Lifecycle
        {
            Inactive,
            Active,
            RestorePending,
            RestoreGrace,
            Faulted,
        };

        enum class RestoreResult
        {
            Restored,
            AlreadyStock,
            FrameDetached,
            OwnershipLost,
            UnsafeToRestore,
        };

    public:
        static FreeBuildCamera& Instance();

        /// Subscribes to the WarcraftXL world lifecycle. Safe to call more than once.
        bool Initialize();

        bool Activate();
        /// Requests a stop. Camera-slot restoration is deferred to the safe
        /// end-of-scene lifecycle tick instead of mutating the slot from WndProc.
        void Deactivate();
        void Toggle();
        bool Active() const;
        bool Transitioning() const;
        bool MouseLooking() const;
        const char* Status() const;
        /// One-line runtime proof for the catalog: activation, slot ownership,
        /// position, mouse-look state, and movement observed this session.
        const char* DiagnosticText() const;

        /// Advances keyboard/mouse movement and refreshes the SimpleCamera. Call once
        /// per rendered frame, including while the large catalog window is hidden.
        void Tick();

        /// Routes only the controls owned by the free camera. Returns true when the
        /// message must not reach WoW. ImGui capture is supplied by the host so typing
        /// in editor controls never flies the camera.
        bool HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 bool uiWantsMouse, bool uiWantsTextInput);

    private:
        FreeBuildCamera();
        ~FreeBuildCamera();

        class StateGuard
        {
        public:
            explicit StateGuard(CRITICAL_SECTION& lock) : lock_(&lock)
            {
                EnterCriticalSection(lock_);
            }
            ~StateGuard() { LeaveCriticalSection(lock_); }
            StateGuard(const StateGuard&) = delete;
            StateGuard& operator=(const StateGuard&) = delete;
        private:
            CRITICAL_SECTION* lock_;
        };

        enum KeySlot
        {
            Forward,
            Backward,
            Left,
            Right,
            Up,
            Down,
            Boost,
            KeySlotCount,
        };

        static void __cdecl OnWorldLeave(void* user, const void* args);
        // Build-12340 concrete-camera preparation. The native method is __thiscall
        // with two stack arguments (callee `ret 8`); the detour uses __fastcall so
        // ECX remains the camera and EDX is the required shim register.
        using CameraPrepareFn = int(__thiscall*)(void* camera, void* sceneContext, int flags);
        static int __fastcall CameraPrepareHook(void* camera, void* edx,
                                                void* sceneContext, int flags);
        bool InstallCameraPrepareHook();
        void ReapplyPreparedPose(void* camera);
        void AimCamera();
        void ClearInput();
        void CaptureKeyReleaseFence();
        void UpdateKeyReleaseFence();
        bool AnyKeyReleasePending() const;
        void BeginMouseLook(HWND hwnd);
        void EndMouseLook(bool restoreCursor);
        int ControlSlot(WPARAM key) const;
        WPARAM ControlKey(int slot) const;
        bool ValidWorldCamera(void* stockCamera) const;
        bool ValidStockCameraForRestore() const;
        bool CameraCanaryIntact() const;
        void ResetCameraCanary();
        void* CurrentWorldFrame() const;
        void** CameraSlot(void* worldFrame) const;
        RestoreResult RestoreStockCamera(void*& liveFrame);
        void RequestDeactivate(const char* finalStatus, const char* reason);
        void ProcessPendingRestore(bool worldTeardown);
        void FinishRestoreGrace();
        void EnterFaulted(const char* reason);
        wxl::game::camera::Camera* CameraObject();
        const void* CameraPointer() const { return cameraStorage_; }

        // The minimal SDK Camera maps only the fields exposed by four camera virtuals.
        // Build-12340 scene preparation also reads/writes the concrete camera through
        // +0x31C (Wow.exe 0x00600976: mov ecx,[esi+31Ch]). The world-frame constructor
        // allocates exactly 0x320 bytes at 0x004FADDA, constructs it at 0x00606B30,
        // then stores it at worldFrame+0x7E20. A 0x100 clone caused the reproducible
        // 0x0075AF47 crash; 0x320 is the proven complete concrete allocation.
        static constexpr size_t kCameraCloneBytes = 0x320;
        static constexpr uint32_t kCameraCanary = 0xC335CAFEu;
        alignas(16) unsigned char cameraStorage_[kCameraCloneBytes]{};
        uint32_t cameraCanary_[4]{};
        const void* cameraVtable_ = nullptr;
        void* worldFrame_ = nullptr;
        void* stockCamera_ = nullptr;
        HWND hwnd_ = nullptr;

        bool initialized_ = false;
        bool worldLeaveSubscribed_ = false;
        bool cameraPrepareHookReady_ = false;
        Lifecycle lifecycle_ = Lifecycle::Inactive;
        bool mouseLooking_ = false;
        bool recenteringMouse_ = false;
        bool keyDown_[KeySlotCount]{};
        bool keyReleasePending_[KeySlotCount]{};
        uint32_t restoreGraceFrames_ = 0;
        ULONGLONG lastToggleTickMs_ = 0;
        const char* pendingFinalStatus_ = nullptr;
        const char* pendingStopReason_ = nullptr;
        mutable CRITICAL_SECTION stateLock_{};
        DWORD renderThreadId_ = 0;
        DWORD cameraPrepareThreadId_ = 0;
        bool crossThreadInputLogged_ = false;
        bool cameraPrepareThreadMismatchLogged_ = false;
        bool restoreBlockedLogged_ = false;
        bool faultAfterRestore_ = false;
        uint64_t cameraPoseApplyCount_ = 0;

        float position_[3]{};
        float anchor_[3]{};
        float yaw_ = 0.0f;
        float pitch_ = 0.0f;
        float fov_ = 1.04719755f;
        float mouseDeltaX_ = 0.0f;
        float mouseDeltaY_ = 0.0f;
        float lastMovementDistance_ = 0.0f;
        uint64_t movementTickCount_ = 0;

        LARGE_INTEGER performanceFrequency_{};
        LARGE_INTEGER previousTick_{};
        POINT savedCursor_{};
        RECT savedClip_{};
        bool savedClipValid_ = false;

        const char* status_ = "Free camera not initialized";
        mutable char diagnosticText_[256]{};

        static CameraPrepareFn cameraPrepareOriginal_;
    };
}
