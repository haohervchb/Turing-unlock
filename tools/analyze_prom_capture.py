#!/usr/bin/env python3
"""Read-only, byte-level analysis of v4 preboot ROM captures against Linux.

No device access or writes. Emits JSON to stdout. Partial captures are reported
as incomplete, never accepted as firmware identity evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

SIZE = 0x80000
REGIONS = {
    "legacy_pci_image": (0, 0xEC00),
    "efi_pci_image": (0xEC00, 0x11600),
    "fwsec_prod_descriptor": (0x3F024, 0x3C),
    "fwsec_prod_code_data": (0x3F060, 0x9A10),
}


def digest(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "crc32": f"{zlib.crc32(data):08x}"}


def compare(reference, candidate):
    limit = min(len(reference), len(candidate), SIZE)
    differences = [i for i in range(limit) if reference[i] != candidate[i]]
    result = {
        "capture": digest(candidate),
        "reference_first512k": digest(reference[:SIZE]),
        "complete_512k": len(candidate) == SIZE,
        "exact_first512k_match": len(candidate) == SIZE and candidate == reference[:SIZE],
        "compared_bytes": limit,
        "differing_bytes_in_overlap": len(differences),
        "first_differences": [
            {"offset": f"0x{i:x}", "reference": f"{reference[i]:02x}", "capture": f"{candidate[i]:02x}"}
            for i in differences[:64]
        ],
        "regions": {},
        "blocks_64k": [],
    }
    if differences:
        first = differences[0]
        result["difference_boundary"] = {
            "first_offset": f"0x{first:x}",
            "last_offset": f"0x{differences[-1]:x}",
            "identical_prefix_bytes": first,
            "capture_suffix_all_ff": all(b == 0xff for b in candidate[first:]),
            "reference_bytes_at_boundary": reference[first:first + 16].hex(),
            "interpretation": "Readout evidence only; does not establish physical SPI contents or why visibility differs.",
        }
    for name, (start, length) in REGIONS.items():
        complete = start + length <= len(candidate)
        data = candidate[start:start + length]
        result["regions"][name] = {
            "offset": f"0x{start:x}", "expected_bytes": length,
            "complete": complete, **digest(data),
            "exact_match": complete and data == reference[start:start + length],
        }
    for start in range(0, SIZE, 0x10000):
        end = start + 0x10000
        data = candidate[start:end]
        result["blocks_64k"].append({
            "offset": f"0x{start:x}", **digest(data),
            "exact_match": len(data) == 0x10000 and data == reference[start:end],
        })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--reference", type=Path, default=Path("artifacts/2080ti-original-prom.bin"))
    args = parser.parse_args()
    reference = args.reference.read_bytes()
    if len(reference) < SIZE:
        parser.error("reference must contain at least 512 KiB")
    output = {}
    for path in args.captures:
        if path.stat().st_size > SIZE:
            parser.error(f"{path}: expected a v4 capture of at most 512 KiB")
        output[str(path)] = compare(reference, path.read_bytes())
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
