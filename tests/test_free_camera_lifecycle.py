"""Deterministic lifecycle/static gates for the build-12340 free camera.

The native camera pointer cannot be exercised outside Wow.exe, so this module models
the state transitions and also asserts that the production source retains each safety
guard. The release gate additionally requires an actual Win32 build and in-client soak.
"""

from __future__ import annotations

import json
import random
import unittest
from enum import Enum, auto
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src" / "FreeBuildCamera.cpp").read_text(encoding="utf-8")
HPP = (ROOT / "src" / "FreeBuildCamera.hpp").read_text(encoding="utf-8")
HOST = (ROOT / "src" / "ImGuiHostExt.cpp").read_text(encoding="utf-8")
CATALOG = (ROOT / "src" / "Catalog.cpp").read_text(encoding="utf-8")
MODULE = (ROOT / "src" / "Module.cpp").read_text(encoding="utf-8")
MANIFEST = json.loads((ROOT / "wxl.json").read_text(encoding="utf-8"))


class State(Enum):
    INACTIVE = auto()
    ACTIVE = auto()
    PENDING = auto()
    GRACE = auto()
    FAULTED = auto()


class Restore(Enum):
    RESTORED = auto()
    ALREADY_STOCK = auto()
    FRAME_DETACHED = auto()
    OWNERSHIP_LOST = auto()
    UNSAFE = auto()


class CameraModel:
    """Small executable specification of the production transition contract."""

    def __init__(self) -> None:
        self.state = State.INACTIVE
        self.grace = 0
        self.clone_generation = 0
        self.release_fence: set[str] = set()
        self.last_toggle_ms: int | None = None

    def activate(self) -> bool:
        if self.state is not State.INACTIVE or self.release_fence:
            return False
        self.clone_generation += 1
        self.state = State.ACTIVE
        return True

    def request_stop(self, held: set[str] | None = None) -> None:
        if self.state is not State.ACTIVE:
            return
        self.release_fence.update(held or set())
        self.state = State.PENDING

    def toggle(self, now_ms: int) -> bool:
        if self.last_toggle_ms is not None and now_ms - self.last_toggle_ms < 250:
            return False
        self.last_toggle_ms = now_ms
        if self.state is State.ACTIVE:
            self.request_stop()
            return True
        if self.state is State.INACTIVE:
            return self.activate()
        return False

    def end_scene(self, result: Restore = Restore.RESTORED) -> None:
        if self.state is State.PENDING:
            if result is Restore.UNSAFE:
                return
            self.state = State.GRACE
            self.grace = 2
            return
        if self.state is State.GRACE:
            self.grace -= 1
            if self.grace == 0:
                self.state = State.INACTIVE

    def world_leave(self, result: Restore = Restore.RESTORED) -> None:
        if self.state is State.ACTIVE:
            self.request_stop()
        if self.state is State.PENDING:
            self.state = State.FAULTED if result is Restore.UNSAFE else State.INACTIVE
        elif self.state is State.GRACE:
            self.state = State.INACTIVE

    def key_up(self, key: str) -> None:
        self.release_fence.discard(key)


class DeferredHookApiModel:
    """Executable model of WarcraftXL v1.1's stored original-slot contract."""

    def __init__(self) -> None:
        self.original_slot: list[object | None] | None = None

    def attach(self, original_slot: list[object | None]) -> bool:
        self.original_slot = original_slot
        # Registration succeeds before Patch supplies the trampoline.
        return True

    def patch(self, trampoline: object) -> None:
        assert self.original_slot is not None
        self.original_slot[0] = trampoline


class LifecycleModelTests(unittest.TestCase):
    def test_deferred_hook_fill_uses_process_lifetime_slot(self) -> None:
        api = DeferredHookApiModel()
        persistent_original: list[object | None] = [None]
        self.assertTrue(api.attach(persistent_original))
        self.assertIsNone(persistent_original[0])
        trampoline = object()
        api.patch(trampoline)
        self.assertIs(persistent_original[0], trampoline)

    def test_ui_button_stop_defers_until_next_end_scene(self) -> None:
        camera = CameraModel()
        self.assertTrue(camera.activate())
        # OnEndScene calls Tick first; the Catalog button runs later in that callback.
        camera.request_stop()
        self.assertEqual(camera.state, State.PENDING)
        camera.end_scene()
        self.assertEqual(camera.state, State.GRACE)

    def test_escape_insert_focus_share_idempotent_request(self) -> None:
        for reason in ("escape", "insert", "focus-loss"):
            with self.subTest(reason=reason):
                camera = CameraModel()
                camera.activate()
                camera.request_stop({"W"})
                camera.request_stop({"A"})
                self.assertEqual(camera.state, State.PENDING)
                self.assertEqual(camera.release_fence, {"W"})

    def test_two_frame_grace_prevents_immediate_clone_reuse(self) -> None:
        camera = CameraModel()
        camera.activate()
        generation = camera.clone_generation
        camera.request_stop()
        camera.end_scene()
        self.assertFalse(camera.activate())
        camera.end_scene()
        self.assertFalse(camera.activate())
        camera.end_scene()
        self.assertEqual(camera.state, State.INACTIVE)
        self.assertTrue(camera.activate())
        self.assertEqual(camera.clone_generation, generation + 1)

    def test_world_leave_finalizes_pending_and_grace(self) -> None:
        pending = CameraModel()
        pending.activate()
        pending.request_stop()
        pending.world_leave()
        self.assertEqual(pending.state, State.INACTIVE)

        grace = CameraModel()
        grace.activate()
        grace.request_stop()
        grace.end_scene()
        grace.world_leave()
        self.assertEqual(grace.state, State.INACTIVE)

    def test_slot_or_frame_ownership_loss_is_retired_not_overwritten(self) -> None:
        for result in (Restore.FRAME_DETACHED, Restore.OWNERSHIP_LOST,
                       Restore.ALREADY_STOCK):
            with self.subTest(result=result):
                camera = CameraModel()
                camera.activate()
                camera.request_stop()
                camera.end_scene(result)
                self.assertEqual(camera.state, State.GRACE)

    def test_unsafe_live_clone_is_never_retired_or_reused(self) -> None:
        camera = CameraModel()
        camera.activate()
        generation = camera.clone_generation
        camera.request_stop()
        for _ in range(20):
            camera.end_scene(Restore.UNSAFE)
        self.assertEqual(camera.state, State.PENDING)
        self.assertFalse(camera.activate())
        self.assertEqual(camera.clone_generation, generation)
        camera.world_leave(Restore.UNSAFE)
        self.assertEqual(camera.state, State.FAULTED)
        self.assertFalse(camera.activate())
        self.assertEqual(camera.clone_generation, generation)

    def test_key_release_fence_blocks_reactivation(self) -> None:
        camera = CameraModel()
        camera.activate()
        camera.request_stop({"W", "SHIFT"})
        camera.end_scene()
        camera.end_scene()
        camera.end_scene()
        self.assertFalse(camera.activate())
        camera.key_up("W")
        self.assertFalse(camera.activate())
        camera.key_up("SHIFT")
        self.assertTrue(camera.activate())

    def test_inactive_dashboard_does_not_own_stock_movement(self) -> None:
        def handled(*, active: bool, control: bool, text_editing: bool) -> bool:
            if active and control:
                return True
            return text_editing

        # ImGui may broadly request keyboard capture merely because the dashboard
        # has navigation focus. Inactive WASD/jump must still reach the game.
        for key in ("W", "A", "S", "D", "SPACE"):
            with self.subTest(key=key):
                self.assertFalse(handled(active=False, control=True, text_editing=False))
        # Active free-camera controls remain consumed, while an actual editor owns
        # all of its key edges independently of the camera state.
        self.assertTrue(handled(active=True, control=True, text_editing=False))
        self.assertTrue(handled(active=False, control=True, text_editing=True))

    def test_thousand_seeded_toggle_attempts_preserve_invariants(self) -> None:
        rng = random.Random(0x335A)
        camera = CameraModel()
        now = 0
        for _ in range(1000):
            now += rng.randrange(0, 401)
            before_generation = camera.clone_generation
            camera.toggle(now)
            if camera.state is State.PENDING:
                camera.end_scene(rng.choice(list(Restore)[:-1]))
            if camera.state is State.GRACE:
                camera.end_scene()
                camera.end_scene()
            self.assertNotEqual(camera.state, State.FAULTED)
            self.assertGreaterEqual(camera.clone_generation, before_generation)


class ProductionSourceTests(unittest.TestCase):
    def test_manifest_and_fail_closed_lifecycle_are_pinned(self) -> None:
        self.assertEqual(MANIFEST["extension"]["version"], "0.8.2")
        self.assertIn('"wxl-housing 0.8.2:', MODULE)
        self.assertIn('"wxl-housing",\n        706,', MODULE)
        for token in (
            "RestoreResult::UnsafeToRestore",
            "Lifecycle::Faulted",
            "EnterFaulted(\"unsafe restore at world teardown\")",
            "restoreGraceFrames_ = worldTeardown ? 0u : 2u",
            "clone boundary canary changed",
        ):
            self.assertIn(token, CPP)

    def test_camera_restore_runs_from_tick_not_wndproc(self) -> None:
        handler = CPP.split("bool FreeBuildCamera::HandleWindowMessage", 1)[1]
        self.assertNotIn("RestoreStockCamera", handler)
        self.assertIn("ProcessPendingRestore(false)", CPP)
        self.assertIn("RequestDeactivate(\"Free camera ready\", \"escape\")", handler)

    def test_horizontal_strafe_matches_live_visual_orientation(self) -> None:
        self.assertIn("(keyDown_[Left] ? right[0] : 0.0f) -\n                (keyDown_[Right] ? right[0] : 0.0f)", CPP)
        self.assertIn("(keyDown_[Left] ? right[1] : 0.0f) -\n                (keyDown_[Right] ? right[1] : 0.0f)", CPP)
        self.assertNotIn("(keyDown_[Right] ? right[0] : 0.0f) -\n                (keyDown_[Left] ? right[0] : 0.0f)", CPP)

    def test_thread_and_toctou_guards_are_present(self) -> None:
        self.assertIn("mutable CRITICAL_SECTION stateLock_", HPP)
        self.assertIn("StateGuard guard(stateLock_)", CPP)
        self.assertIn("RestoreStockCamera(void*& liveFrame)", CPP)
        self.assertIn("liveFrame = CurrentWorldFrame()", CPP)
        self.assertIn("WndProc input synchronized across threads", CPP)
        self.assertIn("kCameraCanary = 0xC335CAFEu", HPP)
        self.assertIn("kCameraCloneBytes = 0x320", HPP)
        self.assertIn("0x00600976: mov ecx,[esi+31Ch]", HPP)
        self.assertIn("allocates exactly 0x320 bytes at 0x004FADDA", HPP)

    def test_camera_prepare_hook_is_signature_gated_and_post_original(self) -> None:
        for token in (
            "kCameraPrepare = 0x00606F90",
            "kCameraPrepareSignature[]",
            "kCameraPrepareRet8 = 0x00606FBD",
            "kCameraPrepareRet8Signature[] = { 0xC2, 0x08, 0x00 }",
            'HookAttach("wxl-housing.camera-prepare"',
            "cameraPrepareThreadId_",
            "cameraObject != cameraStorage_",
            "*liveSlot != cameraStorage_",
            "CameraCanaryIntact()",
        ):
            self.assertIn(token, CPP + HPP)
        hook = CPP.split("FreeBuildCamera::CameraPrepareHook", 1)[1]
        self.assertLess(
            hook.index("original(cameraObject, sceneContext, flags)"),
            hook.index("ReapplyPreparedPose(cameraObject)"),
        )
        self.assertIn("previousCamera != cameraObject", hook)
        self.assertIn("g_cameraPrepareHookCamera = previousCamera", hook)
        self.assertNotIn("g_cameraPrepareHookDepth", hook)
        activation = CPP.split("lifecycle_ = Lifecycle::Active;", 1)[1].split(
            "return true;", 1
        )[0]
        self.assertIn("cameraPrepareThreadId_ = 0;", activation)
        self.assertIn("cameraPrepareThreadMismatchLogged_ = false;", activation)
        self.assertIn("int(__thiscall*)(void* camera, void* sceneContext, int flags)", HPP)
        self.assertIn("static int __fastcall CameraPrepareHook", HPP)
        install = CPP.split("bool FreeBuildCamera::InstallCameraPrepareHook", 1)[1].split(
            "int __fastcall FreeBuildCamera::CameraPrepareHook", 1
        )[0]
        self.assertIn("reinterpret_cast<void**>(&cameraPrepareOriginal_)", install)
        self.assertNotIn("void* original = nullptr", install)
        self.assertNotIn("|| !original", install)
        self.assertIn("expected to remain null", install)

    def test_input_release_and_dashboard_close_fences(self) -> None:
        self.assertIn("CaptureKeyReleaseFence();", CPP)
        self.assertIn("AnyKeyReleasePending()", CPP)
        self.assertIn("FreeBuildCamera::Instance().Deactivate();", HOST)
        self.assertIn("kInsertToggleDebounceMs = 250", HOST)
        inactive = CPP.split("if (lifecycle_ != Lifecycle::Active)", 1)[1].split(
            "if ((message == WM_KEYDOWN", 1
        )[0]
        self.assertIn("slot >= 0 && keyReleasePending_[slot]", inactive)
        self.assertNotIn("slot >= 0 && (Transitioning()", inactive)

    def test_dashboard_keyboard_capture_is_text_only(self) -> None:
        router = HOST.split("LRESULT CALLBACK WndProc", 1)[1].split(
            "ImGuiHostExt& ImGuiHostExt::Instance", 1
        )[0]
        self.assertIn("io.WantTextInput", router)
        self.assertNotIn("host.Open() && io.WantCaptureKeyboard", router)
        self.assertIn("h, m, w, l, io.WantCaptureMouse, io.WantTextInput", router)
        self.assertIn("!IsStockMovementKeyMessage(m, w)", router)

    def test_transition_ui_is_explicit_and_non_clickable(self) -> None:
        self.assertIn('"CAMERA RESTORING..."', CATALOG)
        self.assertIn("ImGui::BeginDisabled()", CATALOG)
        self.assertIn("cameraTransitioning", CATALOG)

    def test_sdk_core_is_not_required_to_define_private_camera_offset(self) -> None:
        self.assertIn("constexpr size_t kWorldFrameCamera = 0x7E20", CPP)
        self.assertNotIn("world::woff::kWorldFrameCamera", CPP)


if __name__ == "__main__":
    unittest.main(verbosity=2)
