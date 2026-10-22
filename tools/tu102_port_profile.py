#!/usr/bin/env python3
"""Offline profile for this ASUS 2080 Ti, NOT an unlock or device-access tool.

Accepts saved ROM bytes only. Validates the exact native FWSEC descriptor and
firmware hash, follows its interface/mapper offsets, and checks a proposed FRTS
command in a temporary Python buffer. No patched binary is emitted or executed.

Layout reference: NVIDIA open-gpu-kernel-modules tag 580.178.04 (installed
driver), kernel_gsp_tu102.c:kgspPopulateWprMeta_TU102,
kernel_gsp_frts_tu102.c:s_prepareForFwsec_TU102,
kernel_gsp.c:kgspGetWprEndMargin_IMPL, kern_disp_0400.c.
This models the ordinary driver layout, not a validated preboot loader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DESC_OFFSET = 0x3F024
CODE_OFFSET = DESC_OFFSET + 0x3C
PROD_SHA256 = "584b5fe0826c40aaa8731d70f4fe5fb505380075dca77f7a94d31cfa09d20f6e"
DESC_FIELDS = (
    "vDesc", "StoredSize", "UncompressedSize", "VirtualEntry",
    "InterfaceOffset", "IMEMPhysBase", "IMEMLoadSize", "IMEMVirtBase",
    "IMEMSecBase", "IMEMSecSize", "DMEMOffset", "DMEMPhysBase",
    "DMEMLoadSize", "altIMEMLoadSize", "altDMEMLoadSize",
)
EXPECTED_DESC = (
    0x3C0201, 0x9A10, 0x9A10, 0, 0xDC, 0, 0x9600, 0,
    0x300, 0x9300, 0x9600, 0, 0x410, 0x9600, 0x6618,
)
FB_SIZE = 11 << 30


def require(condition, message):
    if not condition:
        raise ValueError(message)


def region(data, start, size):
    require(start >= 0 and size >= 0 and start + size <= len(data),
            "region outside supplied data")
    return data[start:start + size]


def native_fwsec(rom):
    require(len(rom) in (0x80000, 0x100000), "expected complete 512 KiB or 1 MiB ROM capture")
    desc = struct.unpack("<15I", region(rom, DESC_OFFSET, 0x3C))
    require(desc == EXPECTED_DESC, "descriptor is not this card's validated native FWSEC")
    blob = region(rom, CODE_OFFSET, desc[1])
    require(hashlib.sha256(blob).hexdigest() == PROD_SHA256,
            "native FWSEC SHA256 mismatch")
    return dict(zip(DESC_FIELDS, desc)), blob


def mapper_info(dmem, interface_offset):
    version, hdr_size, entry_size, count = region(dmem, interface_offset, 4)
    require((version, hdr_size, entry_size) == (1, 4, 8) and 2 <= count <= 32,
            "unsupported interface header")
    table_start = interface_offset + hdr_size
    region(dmem, table_start, count * entry_size)
    matches = []
    for index in range(count):
        ident, offset = struct.unpack("<II", region(dmem, table_start + index * entry_size, 8))
        if ident == 4:
            matches.append(offset)
    require(len(matches) == 1, "expected exactly one DMEM mapper interface")
    offset = matches[0]
    require(offset % 4 == 0, "unaligned mapper")
    mapper = struct.unpack("<16I", region(dmem, offset, 64))
    require(mapper[:2] == (0x50414D44, 0x00400003), "unsupported DMEM mapper")
    command_offset, command_size = mapper[2:4]
    require(command_offset % 4 == 0 and command_size >= 48,
            "unaligned or undersized command buffer")
    region(dmem, command_offset, command_size)
    require(command_offset >= offset + 64 or command_offset + command_size <= offset,
            "command buffer overlaps mapper")
    require(command_offset >= table_start + count * entry_size or
            command_offset + command_size <= interface_offset,
            "command buffer overlaps interface table")
    return {"mapper_offset": offset, "command_offset": command_offset,
            "command_size": command_size, "init_command_offset": offset + 44,
            "interface_offset": interface_offset, "interface_entries": count}


def driver_top_layout(fb_size, workspace_reg, wpr_end_margin=0):
    require(fb_size == FB_SIZE, "this profile is restricted to the measured 11 GiB card")
    require(0 <= workspace_reg <= 0xFFFFFFFF, "workspace register is not uint32")
    require(0 <= wpr_end_margin < fb_size - 0x200000, "invalid WPR end margin")
    valid = bool(workspace_reg & 8)
    original = (workspace_reg >> 8) << 16 if valid else None
    if valid:
        require(original < fb_size, "workspace outside framebuffer")
        workspace = fb_size - 0x20000 if original < fb_size - 0x100000 else original
    else:
        workspace = fb_size - 0x100000
    end = (workspace - wpr_end_margin) & ~0x1FFFF
    require(end >= 0x100000, "insufficient FRTS space")
    return {"fb_size": fb_size, "workspace_register": workspace_reg,
            "workspace_valid": valid, "observed_workspace_base": original,
            "driver_workspace_base": workspace, "driver_workspace_size": fb_size - workspace,
            "driver_relocation_required": valid and original != workspace,
            "wpr_end_margin": wpr_end_margin, "gsp_fw_wpr_end": end,
            "frts_offset": end - 0x100000, "frts_size": 0x100000}


def simulate_frts_command(blob, desc, layout):
    """Modify only an in-memory copy and prove code bytes remain unchanged."""
    code_size = desc["IMEMLoadSize"]
    dmem = region(blob, desc["DMEMOffset"], desc["DMEMLoadSize"])
    mapper = mapper_info(dmem, desc["InterfaceOffset"])
    frts = layout["frts_offset"]
    require(frts > 0 and frts % 0x1000 == 0 and frts >> 12 <= 0xFFFFFFFF,
            "FRTS offset not representable")
    # READ_VBIOS_DESC is 24 B; REGION_DESC is 20 B; combined host struct
    # has 4 B trailing alignment padding. Explicitly zero the padding.
    words = (1, 24, 0, 0, 0, 2, 1, 20, frts >> 12, 0x100, 2, 0)
    command = struct.pack("<12I", *words)
    patched = bytearray(blob)
    cmd_at = desc["DMEMOffset"] + mapper["command_offset"]
    init_at = desc["DMEMOffset"] + mapper["init_command_offset"]
    patched[cmd_at:cmd_at + len(command)] = command
    struct.pack_into("<I", patched, init_at, 0x15)
    changed = [i for i, (a, b) in enumerate(zip(blob, patched)) if a != b]
    require(len(patched) == len(blob) and patched[:code_size] == blob[:code_size],
            "simulation unexpectedly changed executable code")
    require(all(cmd_at <= i < cmd_at + len(command) or init_at <= i < init_at + 4 for i in changed),
            "simulation changed bytes outside command and init fields")
    return {**mapper, "command_words": [hex(w) for w in words],
            "command_size_bytes": len(command), "init_command": "0x15",
            "changed_blob_offsets": [hex(i) for i in changed],
            "code_unchanged": True, "patched_binary_emitted": False,
            "firmware_executed": False}


def hex_values(values):
    return {k: hex(v) if type(v) is int else v for k, v in values.items()}


def profile(rom, workspace_reg):
    desc, blob = native_fwsec(rom)
    layout = driver_top_layout(FB_SIZE, workspace_reg)
    command = simulate_frts_command(blob, desc, layout)
    return {
        "mode": "OFFLINE ONLY: no GPU access, flash, firmware execution, or binary output",
        "target": "ASUS 10de:1e07 / 1043:866a, BOOT0 162000a1, VBIOS 90.02.17.00.47",
        "native_fwsec_sha256": PROD_SHA256,
        "descriptor": hex_values(desc),
        "reference_driver": "NVIDIA open-gpu-kernel-modules 580.178.04",
        "candidate_driver_top_layout": hex_values(layout),
        "frts_command_simulation": command,
        "not_validated": [
            "No proof this UEFI environment can execute native FWSEC or accept the command.",
            "Driver layout assumes display support, no TU102 MMU lock, and normal boot with zero recovery margin.",
            "Driver workspace relocation has NOT been implemented or executed in UEFI.",
            "Complete Booter metadata, including the 11 GiB heap and loaded firmware sizes, is not built here.",
            "No unlock selector write or proof of increased tensor throughput.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    parser.add_argument("--workspace-register", type=lambda s: int(s, 0), required=True,
                        help="value from the preboot diagnostic, not a live device read")
    args = parser.parse_args()
    try:
        result = profile(args.rom.read_bytes(), args.workspace_register)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
