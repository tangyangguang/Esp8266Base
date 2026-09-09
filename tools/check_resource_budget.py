#!/usr/bin/env python3
"""ESP8266 Web + MQTT/TLS static admission gate; not runtime acceptance.

RAM accounting matches PlatformIO espressif8266 SIZEDATAREGEXP:
.data + .rodata + .bss. The 80KiB / 66% limit is intentionally not configurable.
Use the target toolchain's xtensa-lx106-elf-size, never a host size utility.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

RAM_BYTES = 81920
MAX_RAM_BYTES = RAM_BYTES * 66 // 100
RAM_SECTIONS = (".data", ".rodata", ".bss")


def parse_size_output(text):
    sections = {}
    for line in text.splitlines():
        fields = line.split()
        if not fields or fields[0] not in (*RAM_SECTIONS, ".irom0.text"):
            continue
        if len(fields) != 3 or not fields[1].isdigit() or not fields[2].isdigit():
            raise ValueError("invalid decimal section size/address")
        name = fields[0]
        if name in sections:
            raise ValueError("duplicate section or multiple input objects")
        size, address = int(fields[1]), int(fields[2])
        if name in RAM_SECTIONS and not (0x3FFE8000 <= address < 0x40000000
                                         and address + size <= 0x40000000):
            raise ValueError("unexpected ESP8266 DRAM section address")
        if name == ".irom0.text" and not (size > 0 and 0x40200000 <= address < 0x40300000):
            raise ValueError("not an ESP8266 application flash section")
        sections[name] = size
    if any(name not in sections for name in (*RAM_SECTIONS, ".irom0.text")):
        raise ValueError("missing required application sections")
    used = sum(sections[name] for name in RAM_SECTIONS)
    return {
        "scope": "static_only",
        "ram_bytes": used,
        "ram_capacity_bytes": RAM_BYTES,
        "maximum_ram_bytes": MAX_RAM_BYTES,
        "ram_percent": round(100 * used / RAM_BYTES, 3),
        "passed": used <= MAX_RAM_BYTES,
    }


def check_elf(elf, size_tool):
    with Path(elf).open("rb") as source:
        header = source.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x01\x01" or int.from_bytes(header[18:20], "little") != 94:
        raise ValueError("expected a little-endian ELF32 Xtensa application")
    result = subprocess.run([str(size_tool), "-A", "-d", str(elf)],
                            check=True, capture_output=True, text=True)
    return parse_size_output(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--size-tool", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = check_elf(args.elf, args.size_tool)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"ESP8266 resource check failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
