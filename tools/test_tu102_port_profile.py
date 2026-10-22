#!/usr/bin/env python3
"""Offline native-profile and malformed-input tests; never accesses a GPU."""
from pathlib import Path
import struct
import unittest

from analyze_prom_capture import compare
from tu102_port_profile import (CODE_OFFSET, DESC_OFFSET, FB_SIZE, driver_top_layout,
                                mapper_info, native_fwsec, profile, simulate_frts_command)

ROOT = Path(__file__).resolve().parent.parent


class PortProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rom = (ROOT / "artifacts/2080ti-original-prom.bin").read_bytes()
        cls.capture = (ROOT / "artifacts/preflight-v4-20261021-103713-00-bulk.bin").read_bytes()
        cls.desc, cls.blob = native_fwsec(cls.rom)
        cls.dmem = cls.blob[cls.desc["DMEMOffset"]:]

    def test_linux_and_preboot_profiles_identical(self):
        self.assertEqual(profile(self.rom, 0x20E09), profile(self.capture, 0x20E09))

    def test_exact_native_mapper(self):
        found = mapper_info(self.dmem, self.desc["InterfaceOffset"])
        self.assertEqual((found["mapper_offset"], found["command_offset"], found["command_size"]),
                         (0x380, 0x3D0, 0x40))

    def test_hash_guard(self):
        mutated = bytearray(self.rom)
        mutated[CODE_OFFSET + 0x300] ^= 1
        with self.assertRaisesRegex(ValueError, "SHA256"):
            native_fwsec(mutated)

    def test_descriptor_guard(self):
        mutated = bytearray(self.rom)
        mutated[DESC_OFFSET + 16] ^= 1
        with self.assertRaisesRegex(ValueError, "descriptor"):
            native_fwsec(mutated)

    def test_truncated_rom(self):
        with self.assertRaises(ValueError):
            native_fwsec(self.rom[:0x49000])

    def test_active_workspace_layout(self):
        layout = driver_top_layout(FB_SIZE, 0x20E09)
        self.assertEqual(layout["observed_workspace_base"], 0x20E0000)
        self.assertEqual(layout["driver_workspace_base"], 0x2BFFE0000)
        self.assertEqual(layout["frts_offset"], 0x2BFEE0000)
        self.assertTrue(layout["driver_relocation_required"])

    def test_inactive_workspace_layout(self):
        layout = driver_top_layout(FB_SIZE, 0)
        self.assertEqual(layout["frts_offset"], 0x2BFE00000)
        self.assertFalse(layout["driver_relocation_required"])

    def test_already_relocated_workspace(self):
        reg = ((0x2BFFE0000 >> 16) << 8) | 8
        layout = driver_top_layout(FB_SIZE, reg)
        self.assertFalse(layout["driver_relocation_required"])
        self.assertEqual(layout["frts_offset"], 0x2BFEE0000)

    def test_wrong_card_geometry(self):
        with self.assertRaises(ValueError):
            driver_top_layout(10 << 30, 0x20E09)

    def test_workspace_out_of_fb(self):
        with self.assertRaises(ValueError):
            driver_top_layout(FB_SIZE, ((FB_SIZE >> 16) << 8) | 8)

    def test_command_simulation_does_not_mutate_input(self):
        snapshot = bytes(self.blob)
        result = simulate_frts_command(self.blob, self.desc, driver_top_layout(FB_SIZE, 0x20E09))
        self.assertEqual(self.blob, snapshot)
        self.assertEqual(result["command_words"][8], "0x2bfee0")
        self.assertTrue(result["code_unchanged"])
        self.assertFalse(result["firmware_executed"])

    def test_invalid_mapper_inputs(self):
        # Each case tests the low-level parser, separately from the outer hash guard.
        cases = [(0xDC, b"\x02"), (0xE0, struct.pack("<I", 0)),
                 (0xE8, struct.pack("<II", 4, 0x380)),
                 (0xE4, struct.pack("<I", 0xFFF0)),
                 (0x380, struct.pack("<I", 0)),
                 (0x388, struct.pack("<I", 0x400)),
                 (0x388, struct.pack("<I", 0x3D1)),
                 (0x38C, struct.pack("<I", 16)),
                 (0x388, struct.pack("<I", 0x380)),
                 (0x388, struct.pack("<I", 0xDC))]
        for offset, content in cases:
            with self.subTest(offset=offset, content=content):
                data = bytearray(self.dmem)
                data[offset:offset + len(content)] = content
                with self.assertRaises(ValueError):
                    mapper_info(data, 0xDC)

    def test_unaligned_frts_rejected(self):
        layout = driver_top_layout(FB_SIZE, 0x20E09)
        layout["frts_offset"] += 1
        with self.assertRaises(ValueError):
            simulate_frts_command(self.blob, self.desc, layout)

    def test_real_rom_difference_boundary(self):
        result = compare(self.rom, self.capture)
        self.assertEqual(result["differing_bytes_in_overlap"], 185853)
        boundary = result["difference_boundary"]
        self.assertEqual(boundary["identical_prefix_bytes"], 0x50A00)
        self.assertTrue(boundary["capture_suffix_all_ff"])

    def test_equal_and_partial_comparisons(self):
        self.assertTrue(compare(self.rom, self.rom[:0x80000])["exact_first512k_match"])
        self.assertFalse(compare(self.rom, self.rom[:0x1000])["complete_512k"])


if __name__ == "__main__":
    unittest.main()
