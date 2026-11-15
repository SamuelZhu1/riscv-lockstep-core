#!/usr/bin/env python3
"""Converts a flat binary to a $readmemh file of 32-bit little-endian words,
padded to 16384 words, for the FPGA build: bin2hex.py prog.bin program.hex"""
import sys

data = open(sys.argv[1], "rb").read()
data += b"\0" * (-len(data) % 4)
words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
words += [0] * (16384 - len(words))
with open(sys.argv[2], "w") as f:
    for w in words[:16384]:
        f.write("%08x\n" % w)
