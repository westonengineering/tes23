#!/usr/bin/env python
"""Checks raw frames dumped by IcdMatrixTests, with no project code involved.

Each line is: name txLen rxLen sameLayout <tx frame hex> <rx frame hex>
sameLayout is 0 when the send and receive ICDs differ, so only length and CRC apply.
Usage: check_wire.py dump.txt
"""
from __future__ import print_function
import binascii
import struct
import sys
import zlib


def crc_ok(frame):
    return struct.unpack("<I", frame[-4:])[0] == (zlib.crc32(frame[:-4]) & 0xFFFFFFFF)


def main():
    checked = failed = 0
    with open(sys.argv[1]) as handle:
        for line in handle:
            name, tx_len, rx_len, same_layout, tx_hex, rx_hex = line.split()
            tx_len, rx_len = int(tx_len), int(rx_len)
            tx, rx = binascii.unhexlify(tx_hex), binascii.unhexlify(rx_hex)
            problems = []
            if len(tx) != tx_len + 4:
                problems.append("tx is %d bytes on the wire" % len(tx))
            if len(rx) != rx_len + 4:
                problems.append("rx is %d bytes on the wire" % len(rx))
            if not crc_ok(tx):
                problems.append("tx CRC wrong")
            if not crc_ok(rx):
                problems.append("rx CRC wrong")
            if same_layout == "1" and rx[:rx_len] != tx[:rx_len]:
                problems.append("rx is not the start of tx")
            checked += 1
            if problems:
                failed += 1
                print("FAIL %s: %s" % (name, ", ".join(problems)))
    print("%d frame pairs checked independently, %d failed" % (checked, failed))
    return 1 if failed or not checked else 0


if __name__ == "__main__":
    sys.exit(main())
