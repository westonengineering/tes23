#!/usr/bin/env python
"""Writes a fake ICD in the pasted-from-Word text format.

Each message is 375 bytes sent (type byte + 374 payload bytes, before the CRC).
The response is the same message without the last 100 bytes, so 275 bytes.
Channels C and D use different IDs and layouts, as real ICDs would.
"""
from __future__ import print_function
import io
import sys

LAST_RETURNED = 275
LAST_BYTE = 375


def section(first, last, shift, direction, rows):
    byte = first
    while byte <= last:
        kind = (byte + shift) % 7
        left = last - byte + 1
        if kind == 0:
            rows.append((byte, "0", "B%03d Flag" % byte, "", direction))
            rows.append(("", "3-1", "B%03d Mode" % byte, "", direction))
            rows.append(("", "7-4", "B%03d Level" % byte, "", direction))
            byte += 1
        elif kind == 1 and left >= 2:
            rows.append(("%d-%d" % (byte, byte + 1), "", "B%03d Count" % byte, "uint16", direction))
            byte += 2
        elif kind == 2 and left >= 4:
            rows.append(("%d-%d" % (byte, byte + 3), "", "B%03d Gain" % byte, "float", direction))
            byte += 4
        elif kind == 3 and left >= 2:
            rows.append(("%d-%d" % (byte, byte + 1), "", "B%03d Temp" % byte, "int16", direction))
            byte += 2
        else:
            rows.append((byte, "", "B%03d Data" % byte, "uint8", direction))
            byte += 1


def table(heading, shift):
    rows = []
    section(2, LAST_RETURNED, shift, "both", rows)
    section(LAST_RETURNED + 1, LAST_BYTE, shift, "to", rows)
    lines = [heading, "Byte\tBits\tDesc\tType\tDir"]
    lines.extend("\t".join(str(c) for c in row) for row in rows)
    return lines


def main():
    lines = table("Message 0xA5 BigC (Channel C)", 0) + [""] + table("Message 0xB7 BigD (Channel D)", 3)
    with io.open(sys.argv[1], "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(u"\n".join(lines) + u"\n")


if __name__ == "__main__":
    main()
