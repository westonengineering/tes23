#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""icdgen.py - turn Interface Control Documents into C++, C# and a test GUI.

Standard library only. Written for Python 2.7 and every Python 3.

    python icdgen.py ../icd/project.json [--out ../generated]

Word ICDs: every table with a Byte column, a Bits column and a Desc (or Name,
Field, Description) column is a message. Cells merged vertically carry down.
Optional extra columns are read when present:
    Type     bool, uint8..64, int8..64, float, double, enum
    Scale, Offset, Unit, Default
    Dir      both, to (only sent), from (only returned)
    Enum     0=Off,1=On

The message ID, name and channel come from the paragraph above the table, for
example "Message 0x90 Status (Channel C)", or from the source entry in
project.json when the document does not state them.

Text ICDs: select the table in Word, paste it into a .txt (or .tsv) file and use
that file as the source. Columns are separated by tabs, a blank Byte cell carries
down from the row above, and everything else works as for a Word table. Several
tables may be pasted into one file, each with its own header row.
"""
from __future__ import print_function, unicode_literals, division

import io
import json
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
RESERVED_RE = re.compile(r"^\s*(reserved|rsvd|spare|unused|n/?a)\b", re.I)
HEADER_ALIASES = {
    "byte": "byte", "bytes": "byte", "byte#": "byte", "offset": "byte",
    "bit": "bits", "bits": "bits",
    "desc": "desc", "description": "desc", "name": "desc", "field": "desc", "signal": "desc",
    "type": "type", "datatype": "type", "format": "type",
    "scale": "scale", "lsb": "scale", "resolution": "scale",
    "valueoffset": "offset", "bias": "offset",
    "unit": "unit", "units": "unit",
    "default": "default", "init": "default",
    "dir": "dir", "direction": "dir", "back": "dir",
    "enum": "enum", "values": "enum", "enumeration": "enum",
}
DIR_MAP = {"": 3, "both": 3, "tofrom": 3, "bidir": 3,
           "to": 1, "tx": 1, "out": 1, "send": 1, "toonly": 1, "txonly": 1,
           "from": 2, "rx": 2, "in": 2, "back": 2, "fromonly": 2, "rxonly": 2}
MAX_IMAGE = 4096


class IcdError(Exception):
    pass


def text_type(value):
    if isinstance(value, bytes):
        return value.decode("utf-8")
    try:
        return unicode(value)  # noqa: F821  (Python 2)
    except NameError:
        return str(value)


def parse_int(value, what):
    try:
        if isinstance(value, int):
            return value
        return int(text_type(value).strip(), 0)
    except (TypeError, ValueError):
        raise IcdError("%s: '%s' is not an integer" % (what, value))


def identifier(text, fallback):
    words = re.findall(r"[A-Za-z0-9]+", text or "")
    name = "".join(w[0].upper() + w[1:] for w in words)
    if not name:
        name = fallback
    if name[0].isdigit():
        name = "F" + name
    if name in CS_KEYWORDS:
        name += "_"
    return name


CS_KEYWORDS = frozenset(["Name", "Index", "Channel", "Side", "IsValid", "Sequence", "AgeMs",
                         "Device", "Info", "PropertyChanged", "GetType", "ToString", "Equals", "Send", "Refresh"])


# ---------------------------------------------------------------- docx reading

def para_text(p):
    return "".join(t.text or "" for t in p.iter(W + "t")).strip()


def table_rows(tbl):
    rows, carry = [], {}
    for tr in tbl.findall(W + "tr"):
        row, col = [], 0
        for tc in tr.findall(W + "tc"):
            pr = tc.find(W + "tcPr")
            span, continuing = 1, False
            if pr is not None:
                gs = pr.find(W + "gridSpan")
                if gs is not None:
                    span = int(gs.get(W + "val"))
                vm = pr.find(W + "vMerge")
                if vm is not None and vm.get(W + "val") != "restart":
                    continuing = True
            text = " ".join(x for x in (para_text(p) for p in tc.findall(W + "p")) if x)
            if continuing:
                text = carry.get(col, "")
            else:
                carry[col] = text
            for _ in range(span):
                row.append(text)
                col += 1
        rows.append(row)
    return rows


def header_map(row):
    found = {}
    for i, cell in enumerate(row):
        key = re.sub(r"[^a-z#]", "", cell.lower())
        name = HEADER_ALIASES.get(key)
        if name and name not in found:
            found[name] = i
    if "byte" in found and "desc" in found and "bits" in found:
        return found
    return None


def fields_from_rows(rows, header):
    fields, last_byte = [], ""
    for row in rows:
        cell = lambda n: row[header[n]].strip() if n in header and header[n] < len(row) else ""
        if not cell("desc") and not cell("bits") and not cell("byte"):
            continue
        byte = cell("byte") or last_byte
        last_byte = byte
        item = {"byte": byte, "bits": cell("bits"), "desc": cell("desc")}
        for opt in ("type", "scale", "offset", "unit", "default", "dir", "enum"):
            if cell(opt):
                item[opt] = cell(opt)
        fields.append(item)
    return fields


def decode_text(data):
    """Notepad and Word can save as UTF-8, UTF-16 or the ANSI code page."""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return data.decode("utf-16")
    if data[:3] == b"\xef\xbb\xbf":
        return data[3:].decode("utf-8")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("cp1252")


def split_cells(line):
    return [c.replace("\xa0", " ").strip() for c in line.split("\t")]


def read_txt(path):
    """Reads tables pasted from Word. Returns the same shape as read_docx."""
    try:
        with io.open(path, "rb") as handle:
            lines = decode_text(handle.read()).splitlines()
    except (IOError, OSError) as exc:
        raise IcdError("%s: cannot read text file (%s)" % (path, exc))

    results, paragraphs, i = [], [], 0
    while i < len(lines):
        line = lines[i]
        cells = split_cells(line)
        header = header_map(cells)
        if header is None:
            if "\t" in line:
                raise IcdError("%s line %d: this row has no Byte, Bits and Desc header row above it" % (path, i + 1))
            if line.strip():
                paragraphs.append(line.strip())
            i += 1
            continue

        rows = []
        i += 1
        while i < len(lines):
            row_line = lines[i]
            if "\t" not in row_line:
                # A line with no tabs is either the text before the next table or
                # a cell containing a line break. Guessing would corrupt the ICD.
                following = i
                while following < len(lines) and "\t" not in lines[following]:
                    following += 1
                if not row_line.strip() or following == len(lines) or header_map(split_cells(lines[following])):
                    break
                raise IcdError("%s line %d: row has no columns, so a cell probably contains a line break. "
                               "Remove the line break from that cell." % (path, i + 1))
            row = split_cells(row_line)
            if len(row) > len(cells):
                raise IcdError("%s line %d: row has %d columns but the header has %d" % (path, i + 1, len(row), len(cells)))
            if header_map(row) is None:  # a header repeated at a page break is skipped
                has_desc = header["desc"] < len(row) and row[header["desc"]]
                if any(row) and not has_desc:
                    raise IcdError("%s line %d: row has no Desc value" % (path, i + 1))
                rows.append(row)
            i += 1
        results.append((paragraphs[-3:], fields_from_rows(rows, header)))
        paragraphs = []
    if not results:
        raise IcdError("%s: no tab-separated table with Byte, Bits and Desc columns was found" % path)
    return results


def read_docx(path):
    """Returns a list of (heading_lines, field_dicts), one per message table."""
    try:
        archive = zipfile.ZipFile(path)
        root = ET.fromstring(archive.read("word/document.xml"))
    except (IOError, OSError, KeyError, zipfile.BadZipfile) as exc:
        raise IcdError("%s: cannot read Word document (%s)" % (path, exc))
    body = root.find(W + "body")
    results, paragraphs = [], []
    for child in body:
        if child.tag == W + "p":
            text = para_text(child)
            if text:
                paragraphs.append(text)
        elif child.tag == W + "tbl":
            rows = table_rows(child)
            header = header_map(rows[0]) if rows else None
            if header is None:
                paragraphs = []
                continue
            results.append((paragraphs[-3:], fields_from_rows(rows[1:], header)))
            paragraphs = []
    if not results:
        raise IcdError("%s: no table with Byte, Bits and Desc columns was found" % path)
    return results


def parse_heading(lines):
    info = {}
    for line in reversed(lines):
        m = re.search(r"0[xX]([0-9A-Fa-f]{1,2})\b", line)
        if m and "id" not in info:
            info["id"] = int(m.group(1), 16)
            ch = re.search(r"(?:channel|chan|ch|interface|port)\s*[:=]?\s*([CDcd])\b|\(([CDcd])\)", line, re.I)
            if ch:
                info["channel"] = (ch.group(1) or ch.group(2)).upper()
            info["cyclic"] = not re.search(r"one[\s-]?shot|acyclic|on[\s-]?demand", line, re.I)
            rest = re.sub(r"0[xX][0-9A-Fa-f]{1,2}\b", " ", line)
            rest = re.sub(r"(?:channel|chan|ch|interface|port)\s*[:=]?\s*[CDcd]\b|\([CDcd]\)", " ", rest, flags=re.I)
            rest = re.sub(r"\b(message|msg|id|type|one[\s-]?shot|cyclic)\b", " ", rest, flags=re.I)
            rest = re.sub(r"[^A-Za-z0-9 _]", " ", rest)
            if rest.strip():
                info["name"] = rest.strip()
    if "name" not in info and lines:
        info["title"] = lines[-1]
    return info


# ------------------------------------------------------------- ICD validation

def parse_range(text, what):
    text = text.strip()
    m = re.match(r"^(\d+)\s*(?:-|:|\.\.|to|–|—)\s*(\d+)$", text, re.I)
    if m:
        a, b = int(m.group(1)), int(m.group(2))
        return min(a, b), max(a, b)
    if re.match(r"^\d+$", text):
        return int(text), int(text)
    raise IcdError("%s: cannot understand range '%s'" % (what, text))


def type_info(type_text, width, enum_text, what):
    t = re.sub(r"[^a-z0-9]", "", (type_text or "").lower())
    t = {"unsigned": "uint", "signed": "int", "boolean": "bool", "single": "float", "real": "float",
         "float32": "float", "float64": "double", "enumeration": "enum"}.get(t, t)
    if enum_text and not t:
        t = "enum"
    if not t:
        return "bool" if width == 1 else "uint"
    if t == "bool":
        if width != 1:
            raise IcdError("%s: bool must be exactly 1 bit, got %d" % (what, width))
        return "bool"
    if t == "float":
        if width != 32:
            raise IcdError("%s: float must be 32 bits, got %d" % (what, width))
        return "float32"
    if t == "double":
        if width != 64:
            raise IcdError("%s: double must be 64 bits, got %d" % (what, width))
        return "float64"
    if t == "enum":
        return "enum"
    m = re.match(r"^(u?)(?:int)?(\d*)$", t)
    if t in ("uint", "int") or (m and (t.startswith("uint") or t.startswith("int") or re.match(r"^[us]\d+$", t))):
        return "uint" if t.startswith("u") else "int"
    if re.match(r"^s\d+$", t):
        return "int"
    raise IcdError("%s: unknown type '%s'" % (what, type_text))


def parse_enum(text, what):
    items = []
    if not text:
        return items
    seen = set()
    for part in re.split(r"[,;]", text):
        if not part.strip():
            continue
        m = re.match(r"^\s*(0[xX][0-9A-Fa-f]+|\d+)\s*[=:]\s*(.+?)\s*$", part)
        if not m:
            raise IcdError("%s: bad enum entry '%s', expected 0=Name" % (what, part))
        value = int(m.group(1), 0)
        name = identifier(m.group(2), "V%d" % value)
        if name in seen:
            raise IcdError("%s: duplicate enum name '%s'" % (what, name))
        seen.add(name)
        items.append((value, name))
    return items


def to_number(value, what):
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    text = text_type(value).strip().lower()
    if text in ("true", "on", "yes"):
        return 1.0
    if text in ("false", "off", "no"):
        return 0.0
    try:
        return float(int(text, 0))
    except ValueError:
        try:
            return float(text)
        except ValueError:
            raise IcdError("%s: '%s' is not a number" % (what, value))


def build_message(raw, project):
    name = identifier(raw["name"], "Msg%02X" % raw["id"])
    channel = raw["channel"]
    where = "%s (channel %s)" % (name, channel)
    base = project["byte_base"]
    endian_big = project["endianness"] == "big"
    omit = set(identifier(x, x) for x in raw.get("omit_from", []))
    fields, reserved_end, used, names = [], 1, {}, set()

    for item in raw["fields"]:
        desc = text_type(item.get("desc", ""))
        fwhat = "%s field '%s'" % (where, desc)
        first, last = parse_range(text_type(item.get("byte", "")), fwhat)
        offset = first - base
        count = last - first + 1
        if offset < 1:
            raise IcdError("%s: byte %d lands on the message-type byte. Check byte_base in project.json." % (fwhat, first))
        if count > 8:
            raise IcdError("%s: spans %d bytes, the maximum is 8" % (fwhat, count))
        bits_text = text_type(item.get("bits", "")).strip()
        if bits_text:
            low, high = parse_range(bits_text, fwhat)
        else:
            low, high = 0, count * 8 - 1
        width = high - low + 1
        if high >= count * 8:
            raise IcdError("%s: bit %d does not fit in %d byte(s)" % (fwhat, high, count))

        direction = DIR_MAP.get(re.sub(r"[^a-z]", "", text_type(item.get("dir", "")).lower()))
        if direction is None:
            raise IcdError("%s: unknown direction '%s'" % (fwhat, item.get("dir")))
        pname = identifier(desc, "Field%d" % (len(fields) + 1))
        if pname in omit:
            direction = 1

        for bit in range(low, high + 1):
            index = bit // 8
            byte_at = offset + (count - 1 - index if endian_big and count > 1 else index)
            key = (byte_at, bit % 8)
            for other_name, other_dir in used.get(key, []):
                if other_dir & direction:
                    raise IcdError("%s overlaps '%s' at byte %d bit %d" % (fwhat, other_name, byte_at + base, bit % 8))
            used.setdefault(key, []).append((desc, direction))
        reserved_end = max(reserved_end, offset + count)

        if RESERVED_RE.match(desc):
            continue
        if pname in names:
            suffix = 2
            while "%s_%d" % (pname, suffix) in names:
                suffix += 1
            pname = "%s_%d" % (pname, suffix)
        names.add(pname)

        kind = type_info(item.get("type"), width, item.get("enum"), fwhat)
        enum = parse_enum(item.get("enum", ""), fwhat)
        scale = to_number(item.get("scale", 1.0), fwhat)
        value_offset = to_number(item.get("offset", 0.0), fwhat)
        if scale == 0:
            raise IcdError("%s: scale must not be zero" % fwhat)
        scaled = scale != 1.0 or value_offset != 0.0
        if scaled and kind in ("bool", "enum"):
            raise IcdError("%s: %s fields cannot have scale or offset" % (fwhat, kind))
        default = to_number(item["default"], fwhat) if "default" in item else value_offset
        if kind in ("uint", "enum", "bool", "int"):
            raw_default = (default - value_offset) / scale
            lo = -(2 ** (width - 1)) if kind == "int" else 0
            hi = 2 ** (width - 1) if kind == "int" else 2 ** width
            if raw_default < lo or raw_default >= hi:
                raise IcdError("%s: default %s does not fit %d bits" % (fwhat, default, width))
        fields.append({
            "name": pname, "desc": desc, "offset": offset, "count": count, "low": low, "width": width,
            "kind": kind, "dir": direction, "big": endian_big, "scale": scale, "value_offset": value_offset,
            "scaled": scaled, "default": default, "unit": text_type(item.get("unit", "")),
            "enum": enum, "enum_text": ",".join("%d=%s" % e for e in enum),
        })

    if not fields:
        raise IcdError("%s: has no fields" % where)
    tx_len = reserved_end
    if "tx_len" in raw:
        tx_len = max(tx_len, parse_int(raw["tx_len"], where + " tx_len"))
    has_response = bool(raw.get("has_response", True))
    from_end = [f["offset"] + f["count"] for f in fields if f["dir"] & 2]
    rx_len = max(from_end) if from_end else 1
    if "rx_len" in raw:
        requested = parse_int(raw["rx_len"], where + " rx_len") - base + 1
        if requested < rx_len:
            raise IcdError("%s: rx_len %d is too short for its From fields" % (where, requested))
        rx_len = requested
    if tx_len > MAX_IMAGE or rx_len > MAX_IMAGE:
        raise IcdError("%s: longer than %d bytes" % (where, MAX_IMAGE))
    return {
        "name": name, "channel": channel, "id": raw["id"], "rx_id": raw.get("rx_id", raw["id"]),
        "tx_len": tx_len, "rx_len": rx_len, "has_response": has_response,
        "cyclic": bool(raw.get("cyclic", True)), "fields": fields,
    }


def load_sources(project, project_dir):
    raw_messages = []
    for src in project["sources"]:
        path = os.path.normpath(os.path.join(project_dir, src["file"]))
        if not os.path.isfile(path):
            raise IcdError("source file not found: %s" % path)
        channel = text_type(src.get("channel", "")).upper()
        if path.lower().endswith((".docx", ".txt", ".tsv")):
            tables = read_docx(path) if path.lower().endswith(".docx") else read_txt(path)
            merged, order = {}, []
            for heading_lines, fields in tables:
                heading = parse_heading(heading_lines)
                msg_id = heading.get("id")
                if msg_id is None and "id" in src:
                    msg_id = parse_int(src["id"], path + " id")
                if msg_id is None:
                    raise IcdError("%s: a table has no message ID. Put 0xNN in the paragraph above the table "
                                   "or set \"id\" for this source in project.json." % path)
                name = heading.get("name") or src.get("name") or heading.get("title") or ("Msg%02X" % msg_id)
                if msg_id == parse_int(src.get("id", -1), path + " id") and src.get("name"):
                    name = src["name"]
                if msg_id not in merged:
                    entry = {"name": name, "id": msg_id, "fields": [],
                             "channel": heading.get("channel") or channel,
                             "cyclic": heading.get("cyclic", True)}
                    merged[msg_id] = entry
                    order.append(msg_id)
                merged[msg_id]["fields"].extend(fields)  # a table continued over a page
            for msg_id in order:
                entry = merged[msg_id]
                for key in ("omit_from", "tx_len", "rx_len", "rx_id", "has_response", "cyclic"):
                    if key in src:
                        entry[key] = src[key]
                raw_messages.append(entry)
        else:
            with io.open(path, "r", encoding="utf-8") as handle:
                doc = json.load(handle)
            for msg in doc["messages"]:
                entry = dict(msg)
                entry.setdefault("channel", channel)
                entry["channel"] = text_type(entry["channel"]).upper()
                entry["id"] = parse_int(msg["id"], path + " id")
                raw_messages.append(entry)

    built, seen, names = [], set(), set()
    for raw in raw_messages:
        if raw["channel"] not in ("C", "D"):
            raise IcdError("message %s: channel must be C or D" % raw.get("name"))
        if "rx_id" in raw:
            raw["rx_id"] = parse_int(raw["rx_id"], "rx_id")
        msg = build_message(raw, project)
        key = (msg["channel"], msg["id"])
        if key in seen:
            raise IcdError("channel %s defines message 0x%02X twice" % key)
        seen.add(key)
        if (msg["channel"], msg["name"]) in names:
            raise IcdError("channel %s defines message name %s twice" % (msg["channel"], msg["name"]))
        names.add((msg["channel"], msg["name"]))
        built.append(msg)
    built.sort(key=lambda m: (m["channel"], m["id"]))
    return built


# ------------------------------------------------------------------ emitters

KIND_CPP = {"bool": "Bool", "uint": "UInt", "int": "Int", "float32": "Float32", "float64": "Float64", "enum": "Enum"}


def cpp_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def num(value):
    return repr(float(value))


def int_type(width, signed):
    for size in (8, 16, 32, 64):
        if width <= size:
            return ("int%d_t" if signed else "uint%d_t") % size
    raise IcdError("field wider than 64 bits")


def cs_int(width, signed):
    table = {8: ("byte", "sbyte"), 16: ("ushort", "short"), 32: ("uint", "int"), 64: ("ulong", "long")}
    for size in (8, 16, 32, 64):
        if width <= size:
            return table[size][1 if signed else 0]


def compute_hash(messages, project):
    canon = [json.dumps(project["crc"], sort_keys=True)]
    for m in messages:
        canon.append("%s|%s|%d|%d|%d|%d|%d|%d" % (m["name"], m["channel"], m["id"], m["rx_id"], m["tx_len"], m["rx_len"], m["has_response"], m["cyclic"]))
        for f in m["fields"]:
            canon.append("%s|%d|%d|%d|%d|%s|%d|%d|%s|%s" % (f["name"], f["offset"], f["count"], f["low"], f["width"], f["kind"], f["dir"], f["big"], num(f["scale"]), num(f["value_offset"])))
    return zlib.crc32("\n".join(canon).encode("utf-8")) & 0xFFFFFFFF


def write_text(path, text):
    with io.open(path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(text)


def cpp_value_type(msg, f):
    if f["kind"] == "bool":
        return "bool"
    if f["kind"] == "float32" and not f["scaled"]:
        return "float"
    if f["scaled"] or f["kind"] == "float64":
        return "double"
    if f["kind"] == "enum":
        return "%s_%s_%s" % (msg["channel"], msg["name"], f["name"])
    return int_type(f["width"], f["kind"] == "int")


def emit_cpp(messages, project, hash_value, out_dir):
    h = ["// Generated by tools/icdgen.py. Do not edit. Change the ICD and regenerate.",
         "#pragma once", '#include "IcdSchema.h"', "#include <cstdint>", "",
         "namespace icdgen {", "",
         "const icd::Schema& GetSchema();",
         "const uint32_t kSchemaHash = 0x%08Xu;" % hash_value, ""]
    c = ["// Generated by tools/icdgen.py. Do not edit. Change the ICD and regenerate.",
         '#include "IcdMessages.h"', "", "namespace icdgen {", ""]

    for index, m in enumerate(messages):
        prefix = "%s_%s" % (m["channel"], m["name"])
        for f in m["fields"]:
            if f["kind"] == "enum":
                h.append("enum class %s_%s : %s {" % (prefix, f["name"], int_type(max(f["width"], 8), False)))
                for value, name in f["enum"]:
                    h.append("    %s = %d," % (name, value))
                h.append("};")
                h.append("")
        c.append("static const icd::FieldDesc k%s_Fields[] = {" % prefix)
        for f in m["fields"]:
            c.append("    { %s, %d, %d, %d, %d, icd::FieldKind::%s, %d, %s, %s, %s, %s, %s, %s }," % (
                cpp_string(f["name"]), f["offset"], f["count"], f["low"], f["width"], KIND_CPP[f["kind"]], f["dir"],
                "true" if f["big"] else "false", num(f["scale"]), num(f["value_offset"]), num(f["default"]),
                cpp_string(f["unit"]), cpp_string(f["enum_text"])))
        c.append("};")
        c.append("")

        for suffix, direction, image_len, msg_id in (("To", 1, m["tx_len"], m["id"]), ("From", 2, m["rx_len"], m["rx_id"])):
            struct = "%s_%s" % (prefix, suffix)
            members = [(i, f) for i, f in enumerate(m["fields"]) if f["dir"] & direction]
            h.append("struct %s {" % struct)
            h.append("    static const int      kMsgIndex = %d;" % index)
            h.append("    static const uint8_t  kId = 0x%02X;" % msg_id)
            h.append("    static const uint16_t kLen = %d;  // type byte + payload" % image_len)
            for _, f in members:
                init = "false" if f["kind"] == "bool" else "%s(0)" % cpp_value_type(m, f)
                unit = (" // " + f["unit"]) if f["unit"] else ""
                h.append("    %s %s = %s;%s" % (cpp_value_type(m, f), f["name"], init, unit))
            h.append("    void Pack(uint8_t* image) const;")
            h.append("    void Unpack(const uint8_t* image);")
            h.append("};")
            h.append("")

            c.append("void %s::Pack(uint8_t* image) const {" % struct)
            c.append("    image[0] = kId;")
            for i, f in members:
                ref = "k%s_Fields[%d]" % (prefix, i)
                if f["scaled"] or f["kind"] in ("float32", "float64"):
                    c.append("    icd::SetValue(image, %s, %s);" % (ref, f["name"]))
                elif f["kind"] == "bool":
                    c.append("    icd::SetRaw(image, %s, %s ? 1u : 0u);" % (ref, f["name"]))
                elif f["kind"] == "int":
                    c.append("    icd::SetRaw(image, %s, static_cast<uint64_t>(static_cast<int64_t>(%s)));" % (ref, f["name"]))
                else:
                    c.append("    icd::SetRaw(image, %s, static_cast<uint64_t>(%s));" % (ref, f["name"]))
            c.append("}")
            c.append("")
            c.append("void %s::Unpack(const uint8_t* image) {" % struct)
            for i, f in members:
                ref = "k%s_Fields[%d]" % (prefix, i)
                vt = cpp_value_type(m, f)
                if f["scaled"] or f["kind"] in ("float32", "float64"):
                    c.append("    %s = static_cast<%s>(icd::GetValue(image, %s));" % (f["name"], vt, ref))
                elif f["kind"] == "bool":
                    c.append("    %s = icd::GetRaw(image, %s) != 0;" % (f["name"], ref))
                elif f["kind"] == "int":
                    c.append("    %s = static_cast<%s>(icd::SignExtend(icd::GetRaw(image, %s), %d));" % (f["name"], vt, ref, f["width"]))
                else:
                    c.append("    %s = static_cast<%s>(icd::GetRaw(image, %s));" % (f["name"], vt, ref))
            c.append("}")
            c.append("")

    c.append("static const icd::MsgDesc kMessages[] = {")
    for m in messages:
        c.append("    { %s, '%s', 0x%02X, 0x%02X, %d, %d, %s, %s, k%s_%s_Fields, %d }," % (
            cpp_string(m["name"]), m["channel"], m["id"], m["rx_id"], m["tx_len"], m["rx_len"],
            "true" if m["has_response"] else "false", "true" if m["cyclic"] else "false",
            m["channel"], m["name"], len(m["fields"])))
    c.append("};")
    c.append("")
    crc = project["crc"]
    tx_total = sum(m["tx_len"] for m in messages)
    rx_total = sum(m["rx_len"] for m in messages)
    channel_rows = []
    for ch in ("C", "D"):
        cfg = project["channels"][ch]
        channel_rows.append("{ %d, %d, %d, %d, %s, %s }" % (
            cfg["baud"], cfg["period_us"], cfg["window_us"], cfg["rx_timeout_us"],
            "true" if cfg.get("trailer_ff") else "false", "true" if cfg.get("rs485_echo") else "false"))
    c.append("static const icd::Schema kSchema = {")
    c.append("    kMessages, %d, %d, %d, 0x%08Xu," % (len(messages), tx_total, rx_total, hash_value))
    c.append("    { 0x%08Xu, 0x%08Xu, 0x%08Xu, %s, %s, %s, %s }," % (
        parse_int(crc["poly"], "crc.poly"), parse_int(crc["init"], "crc.init"), parse_int(crc["xorout"], "crc.xorout"),
        "true" if crc["reflect_in"] else "false", "true" if crc["reflect_out"] else "false",
        "true" if crc["byte_order"] == "big" else "false", "true" if crc["covers_type"] else "false"))
    c.append("    { %s, %s }," % (channel_rows[0], channel_rows[1]))
    c.append("};")
    c.append("")
    c.append("const icd::Schema& GetSchema() { return kSchema; }")
    c.append("")
    c.append("} // namespace icdgen")
    c.append("")
    h.append("} // namespace icdgen")
    h.append("")
    write_text(os.path.join(out_dir, "IcdMessages.h"), "\n".join(h))
    write_text(os.path.join(out_dir, "IcdMessages.cpp"), "\n".join(c))


def cs_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cs_accessor(msg, f, index, writable):
    """Returns (csharp type, getter expression, setter statement)."""
    enum_type = "%s_%s_%s" % (msg["channel"], msg["name"], f["name"])
    if f["kind"] == "bool":
        return "bool", "GetBool(%d)" % index, "SetBool(%d, value);" % index
    if f["scaled"]:
        return "double", "GetScaled(%d)" % index, "SetScaled(%d, value);" % index
    if f["kind"] == "float32":
        return "float", "GetF32(%d)" % index, "SetF32(%d, value);" % index
    if f["kind"] == "float64":
        return "double", "GetF64(%d)" % index, "SetF64(%d, value);" % index
    if f["kind"] == "enum":
        return enum_type, "(%s)GetU(%d)" % (enum_type, index), "SetU(%d, (ulong)value);" % index
    if f["kind"] == "int":
        t = cs_int(f["width"], True)
        return t, "(%s)GetI(%d)" % (t, index), "SetI(%d, value);" % index
    t = cs_int(f["width"], False)
    return t, "(%s)GetU(%d)" % (t, index), "SetU(%d, value);" % index


def emit_cs(messages, project, hash_value, out_dir):
    o = ["// Generated by tools/icdgen.py. Do not edit. Change the ICD and regenerate.",
         "using IcdLink;", "", "namespace IcdLink.Generated", "{"]
    for m in messages:
        for f in m["fields"]:
            if f["kind"] == "enum":
                o.append("    public enum %s_%s_%s : %s" % (m["channel"], m["name"], f["name"], cs_int(max(f["width"], 8), False)))
                o.append("    {")
                for value, name in f["enum"]:
                    o.append("        %s = %d," % (name, value))
                o.append("    }")
                o.append("")

    views = []
    for index, m in enumerate(messages):
        for side, direction, writable in (("To", 1, True), ("From", 2, False)):
            cls = "%s%s_%s" % (side, m["channel"], m["name"])
            views.append((side, m["channel"], m["name"], cls))
            o.append("    /// <summary>%s channel %s, message 0x%02X.</summary>" % ("Sent on" if writable else "Received on", m["channel"], m["id"] if writable else m["rx_id"]))
            o.append("    public sealed class %s : IcdMessageView" % cls)
            o.append("    {")
            o.append("        internal %s(IcdDeviceBase device) : base(device, %d, IcdSide.%s) { }" % (cls, index, side))
            for fi, f in enumerate(m["fields"]):
                if not f["dir"] & direction:
                    continue
                ctype, getter, setter = cs_accessor(m, f, fi, writable)
                if f["desc"] or f["unit"]:
                    note = f["desc"].replace("&", "&amp;").replace("<", "&lt;")
                    if f["unit"]:
                        note += " [%s]" % f["unit"]
                    o.append("        /// <summary>%s</summary>" % note)
                if writable:
                    o.append("        public %s %s { get { return %s; } set { %s } }" % (ctype, f["name"], getter, setter))
                else:
                    o.append("        public %s %s { get { return %s; } }" % (ctype, f["name"], getter))
            o.append("    }")
            o.append("")

    for side, channel in (("To", "C"), ("From", "C"), ("To", "D"), ("From", "D")):
        o.append("    public sealed class %s%sBlock" % (side, channel))
        o.append("    {")
        members = [v for v in views if v[0] == side and v[1] == channel]
        o.append("        internal %s%sBlock(IcdDeviceBase device)" % (side, channel))
        o.append("        {")
        for _, _, name, cls in members:
            o.append("            %s = new %s(device);" % (name, cls))
        o.append("        }")
        for _, _, name, cls in members:
            o.append("        public %s %s { get; }" % (cls, name))
        o.append("        internal IcdMessageView[] All() { return new IcdMessageView[] { %s }; }" % ", ".join(v[2] for v in members))
        o.append("    }")
        o.append("")

    o.append("    public sealed class IcdDevice : IcdDeviceBase")
    o.append("    {")
    o.append("        public IcdDevice() : base(IcdSchema.Instance)")
    o.append("        {")
    for side, channel in (("To", "C"), ("From", "C"), ("To", "D"), ("From", "D")):
        o.append("            %s%s = new %s%sBlock(this); RegisterViews(%s%s.All());" % (side, channel, side, channel, side, channel))
    o.append("        }")
    for side, channel in (("To", "C"), ("From", "C"), ("To", "D"), ("From", "D")):
        o.append("        public %s%sBlock %s%s { get; }" % (side, channel, side, channel))
    o.append("    }")
    o.append("")

    o.append("    public static class IcdSchema")
    o.append("    {")
    crc = project["crc"]
    o.append("        public static readonly IcdSchemaInfo Instance = new IcdSchemaInfo(0x%08Xu, new IcdCrcInfo(0x%08Xu, 0x%08Xu, 0x%08Xu, %s, %s, %s, %s), new[]" % (
        hash_value, parse_int(crc["poly"], "crc.poly"), parse_int(crc["init"], "crc.init"), parse_int(crc["xorout"], "crc.xorout"),
        "true" if crc["reflect_in"] else "false", "true" if crc["reflect_out"] else "false",
        "true" if crc["byte_order"] == "big" else "false", "true" if crc["covers_type"] else "false"))
    o.append("        {")
    for m in messages:
        o.append("            new IcdMsgInfo(%s, '%s', 0x%02X, 0x%02X, %d, %d, %s, %s, new[]" % (
            cs_string(m["name"]), m["channel"], m["id"], m["rx_id"], m["tx_len"], m["rx_len"],
            "true" if m["has_response"] else "false", "true" if m["cyclic"] else "false"))
        o.append("            {")
        for f in m["fields"]:
            o.append("                new IcdFieldInfo(%s, %d, %d, %d, %d, IcdFieldKind.%s, %d, %s, %s, %s, %s, %s)," % (
                cs_string(f["name"]), f["offset"], f["count"], f["low"], f["width"], KIND_CPP[f["kind"]], f["dir"],
                "true" if f["big"] else "false", num(f["scale"]), num(f["value_offset"]),
                cs_string(f["unit"]), cs_string(f["enum_text"])))
        o.append("            }),")
    o.append("        });")
    o.append("    }")
    o.append("}")
    o.append("")
    write_text(os.path.join(out_dir, "IcdMessages.g.cs"), "\n".join(o))


def emit_gui(script_dir, out_dir):
    source = os.path.join(script_dir, "templates", "TestGui")
    target = os.path.join(out_dir, "TestGui")
    if not os.path.isdir(target):
        os.makedirs(target)
    copied = 0
    for name in sorted(os.listdir(source)):
        path = os.path.join(source, name)
        if os.path.isfile(path):
            shutil.copyfile(path, os.path.join(target, name))
            copied += 1
    return copied


def print_report(messages):
    print("")
    for m in messages:
        print("Channel %s  0x%02X %-16s To %d bytes  From %d bytes  %s" % (
            m["channel"], m["id"], m["name"], m["tx_len"], m["rx_len"], "cyclic" if m["cyclic"] else "on demand"))
        for f in m["fields"]:
            print("    wire byte %-3d bits %2d..%-2d  %-8s %-3s %s" % (
                f["offset"], f["low"], f["low"] + f["width"] - 1, f["kind"],
                {1: "to", 2: "frm", 3: "t/f"}[f["dir"]], f["name"]))
    print("")


def main(argv):
    args = list(argv[1:])
    out_dir = None
    if "--out" in args:
        i = args.index("--out")
        if i + 1 >= len(args):
            print("--out needs a directory", file=sys.stderr)
            return 2
        out_dir = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        return 2

    project_path = os.path.abspath(args[0])
    project_dir = os.path.dirname(project_path)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if out_dir is None:
        out_dir = os.path.join(script_dir, "..", "generated")
    out_dir = os.path.abspath(out_dir)

    try:
        with io.open(project_path, "r", encoding="utf-8") as handle:
            project = json.load(handle)
        project.setdefault("byte_base", 1)
        project.setdefault("endianness", "little")
        if project["endianness"] not in ("little", "big"):
            raise IcdError("endianness must be little or big")
        messages = load_sources(project, project_dir)
        if not messages:
            raise IcdError("no messages found")
        if not os.path.isdir(out_dir):
            os.makedirs(out_dir)
        hash_value = compute_hash(messages, project)
        emit_cpp(messages, project, hash_value, out_dir)
        emit_cs(messages, project, hash_value, out_dir)
        copied = emit_gui(script_dir, out_dir)
    except IcdError as exc:
        print("ICD error: %s" % exc, file=sys.stderr)
        return 1

    print_report(messages)
    print("Wrote IcdMessages.h, IcdMessages.cpp, IcdMessages.g.cs and %d TestGui files to %s" % (copied, out_dir))
    print("Schema hash 0x%08X" % hash_value)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
