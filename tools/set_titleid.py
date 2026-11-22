#!/usr/bin/env python3
"""Set the title ID in an XBE certificate.

cxbe always writes 0xFFFF0002. The original 2H4UX release used 0x32483455
("2H4U"), so patching it keeps saves under E:\\UDATA\\32483455 and lets
dashboards show the right game for them.

usage: set_titleid.py default.xbe 32483455
"""
import struct
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, title_id = sys.argv[1], int(sys.argv[2], 16)
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if data[:4] != b"XBEH":
            sys.exit(f"{path}: not an XBE")
        base = struct.unpack_from("<I", data, 0x104)[0]
        cert = struct.unpack_from("<I", data, 0x118)[0] - base
        struct.pack_into("<I", data, cert + 8, title_id)
        f.seek(0)
        f.write(data)
    print(f"{path}: title ID set to {title_id:08X}")


if __name__ == "__main__":
    main()
