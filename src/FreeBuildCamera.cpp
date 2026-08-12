// Detached, client-local build camera for wxl-housing.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "FreeBuildCamera.hpp"

#include "ExtensionApi.hpp"

#include "wxl/EventScript.hpp"

#include "game/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wxl_housing
{
    namespace
    {
        namespace camera = wxl::game::camera;
        namespace world = wxl::game::world;

        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kMouseSensitivity = 0.0032f;
        constexpr float kBaseSpeed = 8.0f;
        constexpr float kBoostMultiplier = 4.0f;
        // Do not move the engine's stream focus away from the player. Keeping the
        // editor camera within this radius also avoids entering unloaded terrain.
        constexpr float kMaximumRange = 180.0f;
        // Build-12340 CWorldFrame active-camera field. This extension-local offset is
        // pinned because the public v1.1 SDK exposes kWorldFrame but not this field.
        constexpr size_t kWorldFrameCamera = 0x7E20;

        // Concrete build-12340 camera preparation. This method is entered as
        // `thiscall(camera, sceneContext, flags)` and returns with `ret 8`.
        // The engine prepares/follows its stock target here after our EndScene tick;
        // reapplying the free pose after this method is the last camera-local point
        // before the caller continues into the world draw.
        constexpr uintptr_t kCameraPrepare = 0x00606F90;
        constexpr unsigned char kCameraPrepareSignature[] = {
            0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA4, 0x00, 0x00,
            0x00, 0x53, 0x8B, 0x5D, 0x08, 0x85, 0xDB, 0x56,
        };
        constexpr uintptr_t kCameraPrepareRet8 = 0x00606FBD;
        constexpr unsigned char kCameraPrepareRet8Signature[] = { 0xC2, 0x08, 0x00 };
        // Same-camera recursion is collapsed to the outer return. A nested clone
        // reached while an outer stock camera is preparing must still reapply, so a
        // process/global "outermost only" depth guard would be incorrect.
        thread_local void* g_cameraPrepareHookCamera = nullptr;

        bool Finite3(const float v[3])
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        float Length3(const float v[3])
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }

        bool Normalize3(float v[3])
        {
            const float length = Length3(v);
            if (!(length > 0.0001f) || !std::isfinite(length)) return false;
            v[0] /= length;
            v[1] /= length;
            v[2] /= length;
            return true;
        }

        float Clamp(float value, float minimum, float maximum)
        {
            return (std::max)(minimum, (std::min)(value, maximum));
        }

        bool MemoryAllows(const void* address, size_t bytes, bool write)
        {
            if (!address || bytes == 0) return false;
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                return false;

            const DWORD protection = info.Protect & 0xff;
            const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                                  protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                                  protection == PAGE_EXECUTE_READWRITE ||
                                  protection == PAGE_EXECUTE_WRITECOPY;
            const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                                  protection == PAGE_EXECUTE_READWRITE ||
                                  protection == PAGE_EXECUTE_WRITECOPY;
            if (!readable || (write && !writable)) return false;

            const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            return begin <= regionEnd && bytes <= regionEnd - begin;
        }
    }

    FreeBuildCamera::CameraPrepareFn FreeBuildCamera::cameraPrepareOriginal_ = nullptr;

    FreeBuildCamera::FreeBuildCamera()
    {
        InitializeCriticalSectionAndSpinCount(&stateLock_, 4000);
        ResetCameraCanary();
    }

    FreeBuildCamera::~FreeBuildCamera()
    {
        DeleteCriticalSection(&stateLock_);
    }

    FreeBuildCamera& FreeBuildCamera::Instance()
    {
        static FreeBuildCamera instance;
        return instance;
    }

    const char* FreeBuildCamera::DiagnosticText() const
    {
        StateGuard guard(stateLock_);
        bool slotOwned = false;
        if (lifecycle_ == Lifecycle::Active && worldFrame_ && CurrentWorldFrame() == worldFrame_)
        {
            void** slot = CameraSlot(worldFrame_);
            slotOwned = slot && *slot == CameraPointer();
        }
        const char* lifecycle = "inactive";
        if (lifecycle_ == Lifecycle::Active) lifecycle = "active";
        else if (lifecycle_ == Lifecycle::RestorePending) lifecycle = "restore-pending";
        else if (lifecycle_ == Lifecycle::RestoreGrace) lifecycle = "restore-grace";
        else if (lifecycle_ == Lifecycle::Faulted) lifecycle = "faulted";
        std::snprintf(diagnosticText_, sizeof(diagnosticText_),
                      "state=%s | slot=%s | canary=%s | prep=%s/%llu | "
                      "pos %.2f %.2f %.2f | look=%s | moves=%llu last=%.3f",
                      lifecycle, slotOwned ? "owned" : "no",
                      CameraCanaryIntact() ? "ok" : "BAD",
                      cameraPrepareHookReady_ ? "ready" : "off",
                      static_cast<unsigned long long>(cameraPoseApplyCount_),
                      position_[0], position_[1], position_[2],
                      mouseLooking_ ? "yes" : "no",
                      static_cast<unsigned long long>(movementTickCount_),
                      lastMovementDistance_);
        return diagnosticText_;
    }

    bool FreeBuildCamera::Active() const
    {
        StateGuard guard(stateLock_);
        return lifecycle_ == Lifecycle::Active;
    }

    bool FreeBuildCamera::Transitioning() const
    {
        StateGuard guard(stateLock_);
        return lifecycle_ == Lifecycle::RestorePending ||
               lifecycle_ == Lifecycle::RestoreGrace;
    }

    bool FreeBuildCamera::MouseLooking() const
    {
        StateGuard guard(stateLock_);
        return mouseLooking_;
    }

    const char* FreeBuildCamera::Status() const
    {
        StateGuard guard(stateLock_);
        return status_;
    }

    bool FreeBuildCamera::Initialize()
    {
        StateGuard guard(stateLock_);
        if (initialized_) return true;
        if (!g_api || !g_api->Subscribe || !g_api->HookAttach)
        {
            status_ = "Free camera unavailable (event/hook service missing)";
            return false;
        }

        if (!InstallCameraPrepareHook()) return false;

        if (!worldLeaveSubscribed_)
        {
            g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnWorldLeave),
                             &FreeBuildCamera::OnWorldLeave, this);
            worldLeaveSubscribed_ = true;
        }

        initialized_ = true;
        status_ = "Free camera ready";
        WLOG_INFO("free-camera: world-frame camera substitution + post-prepare pose hook ready "
                  "(WASD, Q/Space up, E/Ctrl down, RMB look, Shift boost)");
        return true;
    }

    bool FreeBuildCamera::InstallCameraPrepareHook()
    {
        if (cameraPrepareHookReady_) return true;
        if (!MemoryAllows(reinterpret_cast<const void*>(kCameraPrepare),
                          sizeof(kCameraPrepareSignature), false) ||
            std::memcmp(reinterpret_cast<const void*>(kCameraPrepare),
                        kCameraPrepareSignature, sizeof(kCameraPrepareSignature)) != 0 ||
            !MemoryAllows(reinterpret_cast<const void*>(kCameraPrepareRet8),
                          sizeof(kCameraPrepareRet8Signature), false) ||
            std::memcmp(reinterpret_cast<const void*>(kCameraPrepareRet8),
                        kCameraPrepareRet8Signature, sizeof(kCameraPrepareRet8Signature)) != 0)
        {
            status_ = "Free camera unavailable (camera-prep signature mismatch)";
            WLOG_ERROR("free-camera: build-12340 camera-prep signature mismatch at %p; "
                       "free camera disabled without attaching a hook",
                       reinterpret_cast<void*>(kCameraPrepare));
            return false;
        }

        if (!g_api->HookAttach("wxl-housing.camera-prepare", kCameraPrepare,
                               reinterpret_cast<void*>(&FreeBuildCamera::CameraPrepareHook),
                               reinterpret_cast<void**>(&cameraPrepareOriginal_),
                               WXL_HOOK_DEFAULT_PRIORITY))
        {
            status_ = "Free camera unavailable (camera-prep hook failed)";
            WLOG_ERROR("free-camera: camera-prep hook attach failed; free camera disabled");
            return false;
        }

        // WarcraftXL v1.1 stores the supplied original slot and fills it later when
        // the complete hook chain is patched/relinked. It is expected to remain null
        // during WXL_Load; the detour cannot run until Patch has populated this
        // process-lifetime static slot and armed the target.
        cameraPrepareHookReady_ = true;
        WLOG_INFO("free-camera: camera-prep hook registered at %p "
                  "(deferred trampoline, thiscall, two stack args)",
                  reinterpret_cast<void*>(kCameraPrepare));
        return true;
    }

    int __fastcall FreeBuildCamera::CameraPrepareHook(void* cameraObject, void*,
                                                       void* sceneContext, int flags)
    {
        CameraPrepareFn original = cameraPrepareOriginal_;
        if (!original) return 0;

        // Hook chains should not normally recurse, but the native camera method may
        // be re-entered by an engine callback. Collapse only same-camera recursion;
        // nested preparation of our clone beneath a stock camera still owns a pose
        // write. Every invocation always calls the next/original function.
        void* previousCamera = g_cameraPrepareHookCamera;
        g_cameraPrepareHookCamera = cameraObject;
        const int result = original(cameraObject, sceneContext, flags);
        if (previousCamera != cameraObject)
            FreeBuildCamera::Instance().ReapplyPreparedPose(cameraObject);
        g_cameraPrepareHookCamera = previousCamera;
        return result;
    }

    void FreeBuildCamera::ReapplyPreparedPose(void* cameraObject)
    {
        StateGuard guard(stateLock_);
        if (!cameraPrepareHookReady_ || lifecycle_ != Lifecycle::Active ||
            cameraObject != cameraStorage_)
            return;

        const DWORD currentThread = GetCurrentThreadId();
        if (!cameraPrepareThreadId_)
        {
            cameraPrepareThreadId_ = currentThread;
            WLOG_INFO("free-camera: camera-prep owner thread=%lu", currentThread);
        }
        if ((cameraPrepareThreadId_ != currentThread ||
             (renderThreadId_ && renderThreadId_ != currentThread)))
        {
            if (!cameraPrepareThreadMismatchLogged_)
            {
                cameraPrepareThreadMismatchLogged_ = true;
                WLOG_ERROR("free-camera: camera-prep thread mismatch "
                           "(prep=%lu render=%lu current=%lu); stopping fail-closed",
                           cameraPrepareThreadId_, renderThreadId_, currentThread);
            }
            faultAfterRestore_ = true;
            RequestDeactivate("Free camera disabled (camera-prep thread changed)",
                              "camera-prep-thread");
            return;
        }

        if (!CameraCanaryIntact())
        {
            faultAfterRestore_ = true;
            RequestDeactivate("Free camera disabled (camera clone boundary changed)",
                              "prepare-clone-canary");
            return;
        }

        void* liveFrame = CurrentWorldFrame();
        void** liveSlot = liveFrame == worldFrame_ ? CameraSlot(liveFrame) : nullptr;
        if (!liveSlot || *liveSlot != cameraStorage_)
        {
            RequestDeactivate("Free camera stopped (world camera changed)",
                              "prepare-world-camera-changed");
            return;
        }

        AimCamera();
        ++cameraPoseApplyCount_;
    }

    void FreeBuildCamera::OnWorldLeave(void* user, const void*)
    {
        FreeBuildCamera* self = static_cast<FreeBuildCamera*>(user);
        if (!self) return;
        StateGuard guard(self->stateLock_);
        const DWORD currentThread = GetCurrentThreadId();
        if (self->renderThreadId_ && currentThread != self->renderThreadId_)
            WLOG_WARN("free-camera: world-leave synchronized across threads (render=%lu event=%lu)",
                      self->renderThreadId_, currentThread);
        if (self->lifecycle_ == Lifecycle::RestoreGrace)
        {
            // The slot was already restored (or ownership was proven lost). Do not
            // leave saved teardown identities waiting for an EndScene that may never run.
            self->FinishRestoreGrace();
            self->cameraPrepareThreadId_ = 0;
            self->cameraPrepareThreadMismatchLogged_ = false;
            WLOG_INFO("free-camera: world-leave finalized restore grace before teardown");
            return;
        }
        if (self->lifecycle_ == Lifecycle::Faulted)
        {
            WLOG_WARN("free-camera: world-leave observed fail-closed camera state");
            return;
        }
        self->RequestDeactivate("Free camera stopped (world is leaving)", "world-leave");
        // OnWorldLeave is the final guaranteed pre-teardown callback. Finish the
        // compare/exchange here while the live-frame identity can still be checked;
        // never dereference the saved frame after this callback returns.
        self->ProcessPendingRestore(true);
        self->cameraPrepareThreadId_ = 0;
        self->cameraPrepareThreadMismatchLogged_ = false;
        WLOG_INFO("free-camera: world-leave guard completed before teardown");
    }

    void* FreeBuildCamera::CurrentWorldFrame() const
    {
        const void* holder = reinterpret_cast<const void*>(world::woff::kWorldFrame);
        if (!MemoryAllows(holder, sizeof(void*), false)) return nullptr;
        return *reinterpret_cast<void* const*>(holder);
    }

    void** FreeBuildCamera::CameraSlot(void* worldFrame) const
    {
        if (!worldFrame) return nullptr;
        void** slot = reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(worldFrame) + kWorldFrameCamera);
        return MemoryAllows(slot, sizeof(void*), true) ? slot : nullptr;
    }

    bool FreeBuildCamera::ValidWorldCamera(void* stockCamera) const
    {
        if (!stockCamera || !MemoryAllows(stockCamera, kCameraCloneBytes, false)) return false;
        float position[3]{};
        camera::GetPosition(position);
        if (!Finite3(position)) return false;

        const float* view = camera::GetView();
        const float* projection = camera::GetProjection();
        if (!view || !projection) return false;
        const float forward[3] = { view[2], view[6], view[10] };
        return Finite3(forward) && Length3(forward) > 0.5f &&
               std::isfinite(projection[5]) && std::fabs(projection[5]) > 0.01f;
    }

    bool FreeBuildCamera::Activate()
    {
        StateGuard guard(stateLock_);
        if (lifecycle_ == Lifecycle::Active) return true;
        if (lifecycle_ == Lifecycle::Faulted)
        {
            status_ = "Free camera disabled after an unsafe restore; restart the client";
            return false;
        }
        if (lifecycle_ != Lifecycle::Inactive)
        {
            status_ = "Free camera is still restoring the player camera";
            return false;
        }
        UpdateKeyReleaseFence();
        if (AnyKeyReleasePending())
        {
            status_ = "Release camera movement keys before enabling free camera";
            return false;
        }
        if (!initialized_)
        {
            status_ = "Free camera unavailable (not initialized)";
            return false;
        }

        void* worldFrame = CurrentWorldFrame();
        void** cameraSlot = CameraSlot(worldFrame);
        if (!worldFrame || !cameraSlot)
        {
            status_ = "Enter the world before enabling free camera";
            WLOG_WARN("free-camera: activation rejected (world frame/active-camera slot unavailable)");
            return false;
        }

        void* stock = *cameraSlot;
        if (!ValidWorldCamera(stock))
        {
            status_ = "Free camera could not validate the active world camera";
            WLOG_WARN("free-camera: activation rejected (stock camera=%p, camera globals invalid)", stock);
            return false;
        }

        camera::GetPosition(position_);
        std::memcpy(anchor_, position_, sizeof(anchor_));

        const float* view = camera::GetView();
        float forward[3] = { view[2], view[6], view[10] };
        if (!Normalize3(forward))
        {
            status_ = "Free camera could not read the current view";
            return false;
        }
        yaw_ = std::atan2(forward[1], forward[0]);
        pitch_ = std::asin(Clamp(forward[2], -1.0f, 1.0f));

        const float projectionY = camera::GetProjection()[5];
        fov_ = 2.0f * std::atan(1.0f / std::fabs(projectionY));
        if (!std::isfinite(fov_) || fov_ < 0.35f || fov_ > 2.6f)
            fov_ = 60.0f * kPi / 180.0f;

        worldFrame_ = worldFrame;
        stockCamera_ = stock;
        std::memcpy(cameraStorage_, stockCamera_, sizeof(cameraStorage_));
        ResetCameraCanary();
        cameraVtable_ = *reinterpret_cast<const void* const*>(stockCamera_);
        if (!cameraVtable_)
        {
            worldFrame_ = nullptr;
            stockCamera_ = nullptr;
            status_ = "Free camera could not clone the active camera";
            WLOG_WARN("free-camera: activation rejected (stock camera has no vtable)");
            return false;
        }
        ClearInput();
        movementTickCount_ = 0;
        lastMovementDistance_ = 0.0f;
        QueryPerformanceFrequency(&performanceFrequency_);
        QueryPerformanceCounter(&previousTick_);
        AimCamera();

        // GetActiveCamera is not the only consumer: several build-12340 paths inline
        // *(worldFrame + 0x7E20). Substitute that exact field so scene preparation,
        // culling, picking, and rendering all observe one camera. The compare/exchange
        // refuses to overwrite a camera the engine changed between our read and write.
        void* replaced = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(cameraSlot), cameraStorage_, stockCamera_);
        if (replaced != stockCamera_)
        {
            worldFrame_ = nullptr;
            stockCamera_ = nullptr;
            cameraVtable_ = nullptr;
            status_ = "Free camera activation raced a world-camera change";
            WLOG_WARN("free-camera: camera slot changed during activation (%p -> %p)", stock, replaced);
            return false;
        }

        lifecycle_ = Lifecycle::Active;
        cameraPrepareThreadId_ = 0;
        cameraPrepareThreadMismatchLogged_ = false;
        cameraPoseApplyCount_ = 0;
        restoreGraceFrames_ = 0;
        pendingFinalStatus_ = nullptr;
        pendingStopReason_ = nullptr;
        restoreBlockedLogged_ = false;
        faultAfterRestore_ = false;
        status_ = "Free camera active - Esc exits";
        WLOG_INFO("free-camera: enabled at %.2f %.2f %.2f (frame=%p stock=%p replacement=%p range=%.0f)",
                  position_[0], position_[1], position_[2], worldFrame_, stockCamera_, cameraStorage_,
                  kMaximumRange);
        return true;
    }

    FreeBuildCamera::RestoreResult FreeBuildCamera::RestoreStockCamera(void*& liveFrame)
    {
        liveFrame = CurrentWorldFrame();
        if (!worldFrame_ || !stockCamera_)
            return RestoreResult::UnsafeToRestore;

        // Classify ownership before dereferencing any saved frame/camera identity.
        // A different live frame proves this clone is no longer the active slot; it
        // is safe to retain the bytes for a grace period and then retire the state.
        if (liveFrame != worldFrame_)
            return RestoreResult::FrameDetached;
        void** slot = CameraSlot(worldFrame_);
        if (!slot) return RestoreResult::UnsafeToRestore;
        void* current = *slot;
        if (current == stockCamera_) return RestoreResult::AlreadyStock;
        if (current != cameraStorage_) return RestoreResult::OwnershipLost;

        // Only the dangerous case remains: the live frame still owns our clone. Do
        // not attempt a CAS unless the saved stock object and exact vtable remain valid.
        if (!ValidStockCameraForRestore()) return RestoreResult::UnsafeToRestore;
        void* replaced = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(slot), stockCamera_, cameraStorage_);
        if (replaced == cameraStorage_) return RestoreResult::Restored;
        if (replaced == stockCamera_) return RestoreResult::AlreadyStock;
        return RestoreResult::OwnershipLost;
    }

    bool FreeBuildCamera::ValidStockCameraForRestore() const
    {
        if (!stockCamera_ || !cameraVtable_ ||
            !MemoryAllows(stockCamera_, kCameraCloneBytes, false) ||
            !MemoryAllows(cameraVtable_, sizeof(void*), false))
            return false;
        return *reinterpret_cast<const void* const*>(stockCamera_) == cameraVtable_;
    }

    bool FreeBuildCamera::CameraCanaryIntact() const
    {
        for (uint32_t value : cameraCanary_)
            if (value != kCameraCanary) return false;
        return true;
    }

    void FreeBuildCamera::ResetCameraCanary()
    {
        for (uint32_t& value : cameraCanary_) value = kCameraCanary;
    }

    void FreeBuildCamera::RequestDeactivate(const char* finalStatus, const char* reason)
    {
        if (lifecycle_ == Lifecycle::Inactive || lifecycle_ == Lifecycle::RestoreGrace ||
            lifecycle_ == Lifecycle::Faulted)
            return;
        if (lifecycle_ == Lifecycle::RestorePending)
            return; // idempotent: the first stop reason owns the transition

        if (mouseLooking_) EndMouseLook(GetForegroundWindow() == hwnd_);
        CaptureKeyReleaseFence();
        ClearInput();
        lifecycle_ = Lifecycle::RestorePending;
        pendingFinalStatus_ = finalStatus;
        pendingStopReason_ = reason;
        restoreBlockedLogged_ = false;
        status_ = "Free camera stopping safely at the end of the scene";
        WLOG_INFO("free-camera: stop requested (%s); input released, camera restore deferred",
                  reason ? reason : "unspecified");
    }

    void FreeBuildCamera::ProcessPendingRestore(bool worldTeardown)
    {
        if (lifecycle_ != Lifecycle::RestorePending) return;

        const void* savedFrame = worldFrame_;
        const void* savedStock = stockCamera_;
        void* liveFrame = nullptr;
        const RestoreResult result = RestoreStockCamera(liveFrame);
        if (result == RestoreResult::UnsafeToRestore)
        {
            if (!restoreBlockedLogged_)
            {
                restoreBlockedLogged_ = true;
                WLOG_ERROR("free-camera: restore blocked (%s); live frame still owns an unsafe "
                           "clone or its slot cannot be validated (frame=%p live=%p stock=%p)",
                           pendingStopReason_ ? pendingStopReason_ : "unspecified", savedFrame,
                           liveFrame, savedStock);
            }
            status_ = "Free camera restore blocked; leave the world or restart the client";
            if (worldTeardown)
                EnterFaulted("unsafe restore at world teardown");
            return;
        }

        if (result == RestoreResult::Restored)
            WLOG_INFO("free-camera: deferred restore complete (%s); stock camera %p restored",
                      pendingStopReason_ ? pendingStopReason_ : "unspecified", savedStock);
        else if (result == RestoreResult::AlreadyStock)
            WLOG_INFO("free-camera: deferred restore already complete (%s); stock camera %p active",
                      pendingStopReason_ ? pendingStopReason_ : "unspecified", savedStock);
        else if (result == RestoreResult::FrameDetached)
            WLOG_WARN("free-camera: saved frame detached before restore (%s); clone retired safely",
                      pendingStopReason_ ? pendingStopReason_ : "unspecified");
        else
            WLOG_WARN("free-camera: camera-slot ownership changed before restore (%s); "
                      "replacement left untouched",
                      pendingStopReason_ ? pendingStopReason_ : "unspecified");

        // Keep the cloned camera and saved identities alive for two complete
        // end-of-scene calls after the swap. Engine code that captured the old
        // pointer earlier in a frame therefore never observes cleared lifecycle
        // state or a reused clone during the transition.
        lifecycle_ = Lifecycle::RestoreGrace;
        restoreGraceFrames_ = worldTeardown ? 0u : 2u;
        status_ = pendingFinalStatus_ ? pendingFinalStatus_ : "Free camera ready";
        if (worldTeardown) FinishRestoreGrace();
    }

    void FreeBuildCamera::EnterFaulted(const char* reason)
    {
        // Fail closed. Clear only saved identities, never the clone bytes. Disallowing
        // activation prevents a later world from overwriting storage an old teardown
        // path might still reference.
        worldFrame_ = nullptr;
        stockCamera_ = nullptr;
        cameraVtable_ = nullptr;
        restoreGraceFrames_ = 0;
        pendingFinalStatus_ = nullptr;
        pendingStopReason_ = nullptr;
        restoreBlockedLogged_ = false;
        faultAfterRestore_ = false;
        lifecycle_ = Lifecycle::Faulted;
        status_ = "Free camera disabled after an unsafe restore; restart the client";
        WLOG_ERROR("free-camera: fail-closed for this process (%s)",
                   reason ? reason : "unspecified");
    }

    void FreeBuildCamera::FinishRestoreGrace()
    {
        if (faultAfterRestore_)
        {
            EnterFaulted("camera clone boundary changed");
            return;
        }
        worldFrame_ = nullptr;
        stockCamera_ = nullptr;
        cameraVtable_ = nullptr;
        restoreGraceFrames_ = 0;
        pendingFinalStatus_ = nullptr;
        pendingStopReason_ = nullptr;
        restoreBlockedLogged_ = false;
        lifecycle_ = Lifecycle::Inactive;
        if (!initialized_) status_ = "Free camera not initialized";
    }

    void FreeBuildCamera::Deactivate()
    {
        StateGuard guard(stateLock_);
        RequestDeactivate("Free camera ready", "manual-exit");
    }

    void FreeBuildCamera::Toggle()
    {
        StateGuard guard(stateLock_);
        const ULONGLONG now = GetTickCount64();
        if (lastToggleTickMs_ && now - lastToggleTickMs_ < 250)
        {
            WLOG_WARN("free-camera: duplicate toggle ignored during 250ms transition debounce");
            return;
        }
        lastToggleTickMs_ = now;

        if (lifecycle_ == Lifecycle::Active) Deactivate();
        else if (lifecycle_ == Lifecycle::Inactive) Activate();
        else WLOG_WARN("free-camera: toggle ignored while deferred restore is in progress");
    }

    void FreeBuildCamera::ClearInput()
    {
        std::memset(keyDown_, 0, sizeof(keyDown_));
        mouseDeltaX_ = 0.0f;
        mouseDeltaY_ = 0.0f;
        recenteringMouse_ = false;
    }

    WPARAM FreeBuildCamera::ControlKey(int slot) const
    {
        switch (slot)
        {
            case Forward: return 'W';
            case Backward: return 'S';
            case Left: return 'A';
            case Right: return 'D';
            case Up: return 'Q';
            case Down: return 'E';
            case Boost: return VK_SHIFT;
            default: return 0;
        }
    }

    void FreeBuildCamera::CaptureKeyReleaseFence()
    {
        for (int slot = 0; slot < KeySlotCount; ++slot)
        {
            const WPARAM key = ControlKey(slot);
            const bool physicallyDown = key && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
            keyReleasePending_[slot] = keyReleasePending_[slot] || keyDown_[slot] || physicallyDown;
        }
        // Space and Ctrl alias Up/Down. Fence those slots when either alias is down.
        keyReleasePending_[Up] = keyReleasePending_[Up] ||
            (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        keyReleasePending_[Down] = keyReleasePending_[Down] ||
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    void FreeBuildCamera::UpdateKeyReleaseFence()
    {
        for (int slot = 0; slot < KeySlotCount; ++slot)
        {
            if (!keyReleasePending_[slot]) continue;
            bool down = false;
            if (slot == Up)
                down = (GetAsyncKeyState('Q') & 0x8000) != 0 ||
                       (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            else if (slot == Down)
                down = (GetAsyncKeyState('E') & 0x8000) != 0 ||
                       (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            else
                down = (GetAsyncKeyState(static_cast<int>(ControlKey(slot))) & 0x8000) != 0;
            if (!down) keyReleasePending_[slot] = false;
        }
    }

    bool FreeBuildCamera::AnyKeyReleasePending() const
    {
        for (int slot = 0; slot < KeySlotCount; ++slot)
            if (keyReleasePending_[slot]) return true;
        return false;
    }

    void FreeBuildCamera::AimCamera()
    {
        const float cosPitch = std::cos(pitch_);
        const float sinPitch = std::sin(pitch_);
        const float cosYaw = std::cos(yaw_);
        const float sinYaw = std::sin(yaw_);

        const float forward[3] = { cosPitch * cosYaw, cosPitch * sinYaw, sinPitch };
        // WoW's world basis is X/Y horizontal and +Z up. The camera builder
        // expects forward x right = up, so this is the +right vector rather
        // than its negation (which would mirror the rendered view).
        const float right[3] = { -sinYaw, cosYaw, 0.0f };
        const float up[3] = { -cosYaw * sinPitch, -sinYaw * sinPitch, cosPitch };
        camera::Camera* object = CameraObject();
        camera::Aim(*object, position_, forward, right, up, fov_);
        // Preserve the concrete stock camera vtable copied at activation. Its four
        // methods are the engine's own field readers; retaining it also avoids
        // claiming the cloned auxiliary state belongs to a different camera type.
        object->vtable = cameraVtable_;
    }

    camera::Camera* FreeBuildCamera::CameraObject()
    {
        return reinterpret_cast<camera::Camera*>(cameraStorage_);
    }

    void FreeBuildCamera::Tick()
    {
        StateGuard guard(stateLock_);
        const DWORD currentThread = GetCurrentThreadId();
        if (!renderThreadId_)
        {
            renderThreadId_ = currentThread;
            WLOG_INFO("free-camera: render lifecycle owner thread=%lu", renderThreadId_);
        }
        else if (renderThreadId_ != currentThread)
        {
            WLOG_WARN("free-camera: EndScene owner changed (old=%lu current=%lu); state synchronized",
                      renderThreadId_, currentThread);
            renderThreadId_ = currentThread;
            crossThreadInputLogged_ = false;
        }
        UpdateKeyReleaseFence();
        if (lifecycle_ == Lifecycle::RestorePending)
        {
            ProcessPendingRestore(false);
            return;
        }
        if (lifecycle_ == Lifecycle::RestoreGrace)
        {
            if (restoreGraceFrames_ > 0) --restoreGraceFrames_;
            if (restoreGraceFrames_ == 0) FinishRestoreGrace();
            return;
        }
        if (lifecycle_ != Lifecycle::Active) return;
        if (!CameraCanaryIntact())
        {
            WLOG_ERROR("free-camera: clone boundary canary changed; requesting guarded restore");
            faultAfterRestore_ = true;
            RequestDeactivate("Free camera disabled (camera clone boundary changed)",
                              "clone-canary");
            return;
        }
        if (!hwnd_ || GetForegroundWindow() != hwnd_)
        {
            RequestDeactivate("Free camera stopped (window focus lost)", "focus-loss-tick");
            return;
        }

        void* currentFrame = CurrentWorldFrame();
        void** currentSlot = currentFrame == worldFrame_ ? CameraSlot(currentFrame) : nullptr;
        if (currentFrame != worldFrame_ || !currentSlot || *currentSlot != cameraStorage_)
        {
            RequestDeactivate("Free camera stopped (world camera changed)", "world-camera-changed");
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        float dt = 0.0f;
        if (performanceFrequency_.QuadPart > 0 && previousTick_.QuadPart > 0)
            dt = static_cast<float>(
                static_cast<double>(now.QuadPart - previousTick_.QuadPart) /
                static_cast<double>(performanceFrequency_.QuadPart));
        previousTick_ = now;
        dt = Clamp(dt, 0.0f, 0.1f);

        yaw_ += mouseDeltaX_ * kMouseSensitivity;
        pitch_ -= mouseDeltaY_ * kMouseSensitivity;
        mouseDeltaX_ = 0.0f;
        mouseDeltaY_ = 0.0f;
        pitch_ = Clamp(pitch_, -89.0f * kPi / 180.0f, 89.0f * kPi / 180.0f);
        if (yaw_ > kPi) yaw_ -= 2.0f * kPi;
        else if (yaw_ < -kPi) yaw_ += 2.0f * kPi;

        const float cosPitch = std::cos(pitch_);
        const float forward[3] = {
            cosPitch * std::cos(yaw_), cosPitch * std::sin(yaw_), std::sin(pitch_)
        };
        const float right[3] = { -std::sin(yaw_), std::cos(yaw_), 0.0f };
        float move[3] = {
            (keyDown_[Forward] ? forward[0] : 0.0f) -
                (keyDown_[Backward] ? forward[0] : 0.0f) +
                (keyDown_[Left] ? right[0] : 0.0f) -
                (keyDown_[Right] ? right[0] : 0.0f),
            (keyDown_[Forward] ? forward[1] : 0.0f) -
                (keyDown_[Backward] ? forward[1] : 0.0f) +
                (keyDown_[Left] ? right[1] : 0.0f) -
                (keyDown_[Right] ? right[1] : 0.0f),
            (keyDown_[Forward] ? forward[2] : 0.0f) -
                (keyDown_[Backward] ? forward[2] : 0.0f) +
                (keyDown_[Up] ? 1.0f : 0.0f) -
                (keyDown_[Down] ? 1.0f : 0.0f),
        };
        if (Normalize3(move) && dt > 0.0f)
        {
            const float oldPosition[3] = { position_[0], position_[1], position_[2] };
            const float speed = kBaseSpeed * (keyDown_[Boost] ? kBoostMultiplier : 1.0f);
            position_[0] += move[0] * speed * dt;
            position_[1] += move[1] * speed * dt;
            position_[2] += move[2] * speed * dt;

            float offset[3] = {
                position_[0] - anchor_[0], position_[1] - anchor_[1], position_[2] - anchor_[2]
            };
            const float distance = Length3(offset);
            if (distance > kMaximumRange && Normalize3(offset))
            {
                position_[0] = anchor_[0] + offset[0] * kMaximumRange;
                position_[1] = anchor_[1] + offset[1] * kMaximumRange;
                position_[2] = anchor_[2] + offset[2] * kMaximumRange;
                status_ = "Free camera active - build-area range reached";
            }
            else
                status_ = "Free camera active - Esc exits";

            const float actualMove[3] = {
                position_[0] - oldPosition[0],
                position_[1] - oldPosition[1],
                position_[2] - oldPosition[2],
            };
            lastMovementDistance_ = Length3(actualMove);
            if (lastMovementDistance_ > 0.0001f) ++movementTickCount_;
        }
        else
            lastMovementDistance_ = 0.0f;

        AimCamera();
    }

    int FreeBuildCamera::ControlSlot(WPARAM key) const
    {
        switch (key)
        {
            case 'W': return Forward;
            case 'S': return Backward;
            case 'A': return Left;
            case 'D': return Right;
            // Matches wow.export's established free-camera convention.
            case 'Q': case VK_SPACE: return Up;
            case 'E': case VK_CONTROL: return Down;
            case VK_SHIFT: return Boost;
            default: return -1;
        }
    }

    void FreeBuildCamera::BeginMouseLook(HWND hwnd)
    {
        if (mouseLooking_ || !hwnd || (GetCapture() && GetCapture() != hwnd)) return;
        hwnd_ = hwnd;
        GetCursorPos(&savedCursor_);
        savedClipValid_ = GetClipCursor(&savedClip_) != FALSE;

        RECT client{};
        if (!GetClientRect(hwnd, &client)) return;
        POINT topLeft{ client.left, client.top };
        POINT bottomRight{ client.right, client.bottom };
        ClientToScreen(hwnd, &topLeft);
        ClientToScreen(hwnd, &bottomRight);
        RECT screenRect{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
        ClipCursor(&screenRect);
        SetCapture(hwnd);

        POINT center{ (client.left + client.right) / 2, (client.top + client.bottom) / 2 };
        ClientToScreen(hwnd, &center);
        recenteringMouse_ = true;
        SetCursorPos(center.x, center.y);
        mouseLooking_ = true;
    }

    void FreeBuildCamera::EndMouseLook(bool restoreCursor)
    {
        if (!mouseLooking_) return;
        mouseLooking_ = false;
        recenteringMouse_ = false;
        mouseDeltaX_ = 0.0f;
        mouseDeltaY_ = 0.0f;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (savedClipValid_) ClipCursor(&savedClip_);
        else ClipCursor(nullptr);
        if (restoreCursor) SetCursorPos(savedCursor_.x, savedCursor_.y);
    }

    bool FreeBuildCamera::HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                               bool uiWantsMouse, bool uiWantsTextInput)
    {
        StateGuard guard(stateLock_);
        const DWORD currentThread = GetCurrentThreadId();
        if (renderThreadId_ && currentThread != renderThreadId_ && !crossThreadInputLogged_)
        {
            crossThreadInputLogged_ = true;
            WLOG_WARN("free-camera: WndProc input synchronized across threads (render=%lu input=%lu)",
                      renderThreadId_, currentThread);
        }
        if (hwnd) hwnd_ = hwnd;
        if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam))
        {
            if (lifecycle_ == Lifecycle::Active)
                RequestDeactivate("Free camera stopped (window focus lost)",
                                  "focus-loss-message");
            return false;
        }

        const bool keyMessage = message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
                                message == WM_KEYUP || message == WM_SYSKEYUP;
        const bool keyPressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const int slot = keyMessage ? ControlSlot(wparam) : -1;

        // If a key was held at exit, retain only that key until Windows reports its
        // release so WoW never receives an unmatched key-up. Do not claim brand-new
        // downs during Pending/Grace: Off must restore player movement immediately.
        if (lifecycle_ != Lifecycle::Active)
        {
            if (slot >= 0 && keyReleasePending_[slot])
            {
                if (!keyPressed) UpdateKeyReleaseFence();
                return true;
            }
            if (Transitioning() && keyPressed && wparam == VK_ESCAPE)
                return true;
            return false;
        }

        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_ESCAPE)
        {
            RequestDeactivate("Free camera ready", "escape");
            return true;
        }

        if (keyMessage)
        {
            if (slot >= 0)
            {
                if (!keyPressed || !uiWantsTextInput)
                    keyDown_[slot] = keyPressed;
                // ImGui already received the message before this router is called, so
                // always swallow camera-control keys here. This remains true while a
                // numeric/text editor owns the key: letting that edge reach WoW could
                // start player movement and therefore emit movement packets.
                return true;
            }
        }

        if (message == WM_RBUTTONDOWN && !uiWantsMouse)
        {
            BeginMouseLook(hwnd);
            return mouseLooking_;
        }
        if (message == WM_RBUTTONUP && mouseLooking_)
        {
            EndMouseLook(true);
            return true;
        }
        if ((message == WM_CANCELMODE || message == WM_CAPTURECHANGED) && mouseLooking_)
        {
            EndMouseLook(false);
            return false;
        }
        if (message == WM_MOUSEMOVE && mouseLooking_)
        {
            RECT client{};
            if (!GetClientRect(hwnd, &client)) return true;
            const int centerX = (client.left + client.right) / 2;
            const int centerY = (client.top + client.bottom) / 2;
            const int x = static_cast<short>(LOWORD(lparam));
            const int y = static_cast<short>(HIWORD(lparam));
            if (recenteringMouse_ && x == centerX && y == centerY)
            {
                recenteringMouse_ = false;
                return true;
            }

            mouseDeltaX_ += static_cast<float>(x - centerX);
            mouseDeltaY_ += static_cast<float>(y - centerY);
            POINT center{ centerX, centerY };
            ClientToScreen(hwnd, &center);
            recenteringMouse_ = true;
            SetCursorPos(center.x, center.y);
            return true;
        }
        return false;
    }
}
