#!/usr/bin/env python3
"""Dump all resources from a Mac resource fork into a flat directory tree.

Format reference: Inside Macintosh: More Macintosh Toolbox, "Resource Manager".

Usage:
    dump_resources.py <fork_file> <out_dir> [--list-only]

Output layout:
    out_dir/<TYPE>/<id>[_<name>].bin
Type names are sanitized for the filesystem ('snd ' -> 'snd_', '#' -> '%23').
"""

import argparse
import struct
import sys
from pathlib import Path


def sanitize_type(restype: bytes) -> str:
    out = []
    for b in restype:
        c = chr(b)
        if c.isalnum() or c in "-.":
            out.append(c)
        elif c == " ":
            out.append("_")
        else:
            out.append(f"%{b:02X}")
    return "".join(out)


def sanitize_name(name: str) -> str:
    return "".join(c if (c.isalnum() or c in "-_. ") else "_" for c in name).strip()


APPLEDOUBLE_MAGICS = (0x00051607, 0x00051600)  # AppleDouble, AppleSingle
ENTRY_RESOURCE_FORK = 2


def unwrap_appledouble(data: bytes) -> bytes:
    """If data is AppleSingle/AppleDouble, return the resource fork entry."""
    if len(data) < 26 or struct.unpack_from(">I", data)[0] not in APPLEDOUBLE_MAGICS:
        return data
    (n_entries,) = struct.unpack_from(">H", data, 24)
    for i in range(n_entries):
        eid, off, length = struct.unpack_from(">III", data, 26 + i * 12)
        if eid == ENTRY_RESOURCE_FORK:
            return data[off : off + length]
    raise ValueError("AppleDouble file has no resource fork entry")


def parse_fork(data: bytes):
    """Yield (type_bytes, res_id, attrs, name_or_None, payload_bytes)."""
    data = unwrap_appledouble(data)
    if len(data) < 16:
        raise ValueError("file too small to be a resource fork")
    data_off, map_off, data_len, map_len = struct.unpack_from(">IIII", data, 0)
    if map_off + map_len > len(data) or data_off + data_len > len(data):
        raise ValueError(
            f"header out of bounds: data@{data_off}+{data_len}, "
            f"map@{map_off}+{map_len}, file={len(data)}"
        )

    # Resource map: 16-byte header copy + 4 (handle) + 2 (fileRef) + 2 (attrs)
    type_list_off, name_list_off = struct.unpack_from(">HH", data, map_off + 24)
    tl = map_off + type_list_off
    num_types = struct.unpack_from(">H", data, tl)[0] + 1

    for i in range(num_types):
        toff = tl + 2 + i * 8
        rtype = data[toff : toff + 4]
        count, ref_off = struct.unpack_from(">HH", data, toff + 4)
        count += 1
        for j in range(count):
            roff = tl + ref_off + j * 12
            res_id, name_off = struct.unpack_from(">Hh", data, roff)
            res_id = struct.unpack_from(">h", data, roff)[0]  # signed id
            name_off = struct.unpack_from(">h", data, roff + 2)[0]
            attrs = data[roff + 4]
            d_off = int.from_bytes(data[roff + 5 : roff + 8], "big")
            name = None
            if name_off != -1:
                noff = map_off + name_list_off + name_off
                nlen = data[noff]
                name = data[noff + 1 : noff + 1 + nlen].decode(
                    "mac_roman", errors="replace"
                )
            payload_off = data_off + d_off
            (payload_len,) = struct.unpack_from(">I", data, payload_off)
            payload = data[payload_off + 4 : payload_off + 4 + payload_len]
            yield rtype, res_id, attrs, name, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fork_file", type=Path)
    ap.add_argument("out_dir", type=Path, nargs="?")
    ap.add_argument("--list-only", action="store_true")
    args = ap.parse_args()

    data = args.fork_file.read_bytes()
    by_type = {}
    total = 0
    for rtype, res_id, attrs, name, payload in parse_fork(data):
        by_type.setdefault(rtype, []).append((res_id, attrs, name, payload))
        total += 1
        if not args.list_only:
            tdir = args.out_dir / sanitize_type(rtype)
            tdir.mkdir(parents=True, exist_ok=True)
            fname = f"{res_id}"
            if name:
                fname += f"_{sanitize_name(name)}"
            (tdir / f"{fname}.bin").write_bytes(payload)

    print(f"{args.fork_file.name}: {total} resources, {len(by_type)} types")
    for rtype in sorted(by_type, key=lambda t: sanitize_type(t)):
        entries = by_type[rtype]
        sizes = [len(p) for _, _, _, p in entries]
        ids = sorted(e[0] for e in entries)
        id_rng = f"{ids[0]}..{ids[-1]}" if len(ids) > 1 else f"{ids[0]}"
        print(
            f"  {rtype.decode('mac_roman'):<6} n={len(entries):<4} "
            f"ids={id_rng:<14} bytes={sum(sizes)}"
        )


if __name__ == "__main__":
    main()
