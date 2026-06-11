#!/usr/bin/env python3
"""Decode the HALESTORM 'MDRV' driver resource into raw 68k code.

The game loads MDRV id 11 ("MIDI Synth 3.45"), then (f11_21b8) decrypts
bytes [7..end) in place with a word LCG stream cipher seeded 0xDCE5:

    out   = b ^ (state >> 8)
    state = ((b + state) * 0xCE6D + 0x58BF) & 0xFFFF      # b = encrypted byte

then (f11_39de/f11_3858) LZSS-decompresses the payload:

    [0..4)  big-endian unpacked size (plain)
    [4..)   LZSS stream: flag byte, LSB first; bit=1 literal, bit=0 match:
            two bytes BE -> offset = w & 0xFFF (window position, source =
            out_pos - 0x1000 + offset), length = (w >> 12) + 3

Output is the driver code image the game jumps into (entry at offset 0);
we recompile it as a fixed-address segment.
"""
import struct
import sys
from pathlib import Path


def decrypt(buf: bytearray) -> None:
    state = 0xDCE5
    for i in range(7, len(buf)):
        b = buf[i]
        buf[i] = b ^ ((state >> 8) & 0xFF)
        state = ((b + state) * 0xCE6D + 0x58BF) & 0xFFFF


def unlzss(src: bytes, out_size: int) -> bytearray:
    out = bytearray()
    pos = 0
    while pos < len(src) and len(out) < out_size:
        flags = src[pos]
        pos += 1
        for _ in range(8):
            if len(out) >= out_size or pos >= len(src):
                break
            bit = flags & 1
            flags >>= 1
            if bit:
                out.append(src[pos])
                pos += 1
            else:
                w = (src[pos] << 8) | src[pos + 1]
                pos += 2
                offset = w & 0xFFF
                length = ((w >> 12) & 15) + 3
                sp = len(out) - 0x1000 + offset
                for k in range(length):
                    out.append(out[sp + k] if sp + k >= 0 else 0)
                    if len(out) >= out_size:
                        break
    return out


def main() -> None:
    src_path = Path(sys.argv[1])
    dst_path = Path(sys.argv[2])
    buf = bytearray(src_path.read_bytes())
    print(f"packed: {len(buf)} bytes, head: {buf[:8].hex()}")
    decrypt(buf)
    out_size = struct.unpack(">I", buf[0:4])[0]
    print(f"unpacked size (header): {out_size}")
    out = unlzss(bytes(buf[4:]), out_size)
    print(f"unpacked: {len(out)} bytes, entry words: {out[:16].hex()}")
    dst_path.write_bytes(out)


if __name__ == "__main__":
    main()
