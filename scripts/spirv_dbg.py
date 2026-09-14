#!/usr/bin/env python3
"""Quick SPIR-V debug-info inspector: counts debug opcodes and total words.

Usage: spirv_dbg.py file.spv
"""
import struct
import sys

OPNAMES = {
    5: "OpName",
    6: "OpMemberName",
    7: "OpString",
    8: "OpLine",
    33: "OpSource",
    34: "OpSourceContinued",
    35: "OpSourceExtension",
    330: "OpModuleProcessed",
}

with open(sys.argv[1], "rb") as f:
    data = f.read()

magic, version, gen, bound, schema = struct.unpack_from("<5I", data, 0)
assert magic == 0x07230203, "not a SPIR-V module"

counts = {}
pos = 20
total = 0
while pos < len(data):
    word = struct.unpack_from("<I", data, pos)[0]
    count = word >> 16
    opcode = word & 0xFFFF
    if count == 0:
        break
    if opcode in OPNAMES:
        counts[opcode] = counts.get(opcode, 0) + 1
    pos += count * 4
    total += 1

print(f"instructions: {total}, words: {len(data)//4}, bound: {bound}")
for op, name in sorted(OPNAMES.items()):
    if op in counts:
        print(f"  {name}: {counts[op]}")
if not counts:
    print("  NO DEBUG OPCODES FOUND")
