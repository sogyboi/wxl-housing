"""Hash-bound build-12340 disassembly/crash gates for the concrete camera extent."""

from __future__ import annotations

import hashlib
import os
import struct
import unittest
from pathlib import Path


WOW_SHA256 = "57DD8955FD7238B00969F6011CDAA13DCA14DAA5849D1F9BE64152BD4C7FE5DA"
CRASH_SHA256 = "A7003A66030B15ED3FEE6C9721E2E5377E896151BB3AB4B1234397321590A5BE"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def bytes_at_virtual_address(image: bytes, address: int, size: int) -> bytes:
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    section_count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    image_base = struct.unpack_from("<I", image, pe + 24 + 28)[0]
    rva = address - image_base
    section_table = pe + 24 + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            file_offset = raw_pointer + rva - virtual_address
            return image[file_offset : file_offset + size]
    raise AssertionError(f"virtual address 0x{address:08X} is outside PE sections")


class CameraExtentBinaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        wow = os.environ.get("WXL_WOW_EXE")
        if not wow:
            raise unittest.SkipTest("set WXL_WOW_EXE for hash-bound build-12340 gates")
        cls.wow = Path(wow)
        cls.image = cls.wow.read_bytes()

    def test_exact_client_reads_concrete_camera_plus_31c(self) -> None:
        self.assertEqual(sha256(self.wow), WOW_SHA256)
        # 00600976 8B 8E 1C 03 00 00    mov ecx,dword ptr [esi+31Ch]
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x00600976, 6),
            bytes.fromhex("8B8E1C030000"),
        )
        # The resulting object is immediately passed as ECX to the crashing function.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x00600984, 5),
            bytes.fromhex("E8B7A51500"),
        )
        # 0075AF47 83 BE A4...          cmp dword ptr [esi+0A4h],0
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x0075AF47, 7),
            bytes.fromhex("83BEA400000000"),
        )

    def test_world_frame_allocates_exact_320_byte_camera(self) -> None:
        # 004FADDA push 320h (allocator size), then constructor call 00606B30.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x004FADDA, 5),
            bytes.fromhex("6820030000"),
        )
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x004FADEA, 5),
            bytes.fromhex("E841BD1000"),
        )
        # 004FADF8 stores the new object at worldFrame+7E20h.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x004FADF8, 6),
            bytes.fromhex("8986207E0000"),
        )

    def test_camera_prepare_hook_target_and_thiscall_shape(self) -> None:
        # 00606F90 is the concrete camera preparation entry gated by production.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x00606F90, 16),
            bytes.fromhex("558BEC81ECA4000000538B5D0885DB56"),
        )
        # Its null-context path proves two callee-popped stack arguments.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x00606FBD, 3),
            bytes.fromhex("C20800"),
        )
        # Representative caller: push flags, push context, ECX=camera, call 606F90.
        self.assertEqual(
            bytes_at_virtual_address(self.image, 0x00607B6D, 9),
            bytes.fromhex("57508BCEE81AF4FFFF"),
        )

    def test_runtime_crash_binds_minus_one_camera_child_to_same_eip(self) -> None:
        crash = os.environ.get("WXL_FREECAM_CRASH")
        if not crash:
            self.skipTest("set WXL_FREECAM_CRASH for runtime crash binding")
        report = Path(crash)
        text = report.read_text(encoding="utf-8", errors="replace")
        self.assertEqual(sha256(report), CRASH_SHA256)
        self.assertIn("at 0023:0075AF47", text)
        self.assertIn('referenced memory at "0x000000A3"', text)
        self.assertIn("ECX=FFFFFFFF", text)
        self.assertIn("ESI=FFFFFFFF", text)
        self.assertIn("0x7C60F260", text)  # exact 0.7.2 replacement pointer


if __name__ == "__main__":
    unittest.main(verbosity=2)
