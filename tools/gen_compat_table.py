#!/usr/bin/env python3
"""Generate the compatibility tables in docs/{en,ja}/compatibility.md.

The tables are built from tests/fixtures/matrix/<flavor>-<version>/<config>/meta.json
and written between marker comments in each page:

    <!-- compat:summary:begin --> ... <!-- compat:summary:end -->
    <!-- compat:configs:begin --> ... <!-- compat:configs:end -->

Usage:
    python3 tools/gen_compat_table.py          rewrite the marked regions
    python3 tools/gen_compat_table.py --check  exit 1 if a page is out of date
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MATRIX = ROOT / "tests" / "fixtures" / "matrix"
PAGES = {
    "en": ROOT / "docs" / "en" / "compatibility.md",
    "ja": ROOT / "docs" / "ja" / "compatibility.md",
}

FLAVOR_ORDER = {"mysql": 0, "mariadb": 1}
FLAVOR_NAME = {"mysql": "MySQL", "mariadb": "MariaDB"}
NOT_APPLICABLE = "-"

# Column order and value order within a summary cell.
CHECKSUM_ORDER = ["NONE", "CRC32"]
IMAGE_ORDER = ["FULL", "MINIMAL", "NOBLOB"]
METADATA_ORDER = ["MINIMAL", "FULL"]
COMPRESSION_ORDER = ["OFF", "zstd", "zlib"]
PARTIAL_JSON_ORDER = ["OFF", "ON"]

HEADERS = {
    "en": {
        "summary": ["Flavor", "Version", "Server version", "Configurations", "Checksum",
                    "Row image", "Row metadata", "Compression", "PARTIAL_JSON"],
        "configs": ["Flavor", "Version", "Checksum", "Row image", "Row metadata",
                    "Compression", "PARTIAL_JSON"],
    },
    "ja": {
        "summary": ["種類", "版", "サーバーの版", "設定数", "チェックサム",
                    "行イメージ", "行メタデータ", "圧縮", "PARTIAL_JSON"],
        "configs": ["種類", "版", "チェックサム", "行イメージ", "行メタデータ",
                    "圧縮", "PARTIAL_JSON"],
    },
}


def version_key(version):
    return tuple(int(part) if part.isdigit() else 0 for part in version.split("."))


def load_configs():
    """One record per captured configuration, sorted by flavor, version, settings."""
    records = []
    for meta_path in sorted(MATRIX.glob("*/*/meta.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        flavor = meta["flavor"]
        if flavor not in FLAVOR_ORDER:
            raise SystemExit(f"{meta_path}: unknown flavor {flavor!r}")
        settings = meta["settings"]
        if flavor == "mysql":
            compressed = settings["binlog_transaction_compression"].upper() == "ON"
            compression = "zstd" if compressed else "OFF"
            options = settings["binlog_row_value_options"].upper()
            partial_json = "ON" if "PARTIAL_JSON" in options else "OFF"
        else:
            compressed = settings["log_bin_compress"].upper() == "ON"
            compression = "zlib" if compressed else "OFF"
            partial_json = NOT_APPLICABLE
        record = {
            "flavor": flavor,
            "version": meta["version"],
            "server_version": meta["server_version"],
            "checksum": settings["binlog_checksum"].upper(),
            "image": settings["binlog_row_image"].upper(),
            "metadata": settings["binlog_row_metadata"].upper(),
            "compression": compression,
            "partial_json": partial_json,
        }
        for field, order in (("checksum", CHECKSUM_ORDER), ("image", IMAGE_ORDER),
                             ("metadata", METADATA_ORDER)):
            if record[field] not in order:
                raise SystemExit(f"{meta_path}: unexpected {field} {record[field]!r}")
        records.append(record)
    if not records:
        raise SystemExit(f"no meta.json under {MATRIX}")

    def sort_key(r):
        return (FLAVOR_ORDER[r["flavor"]], version_key(r["version"]),
                CHECKSUM_ORDER.index(r["checksum"]), IMAGE_ORDER.index(r["image"]),
                METADATA_ORDER.index(r["metadata"]),
                COMPRESSION_ORDER.index(r["compression"]), r["partial_json"])

    return sorted(records, key=sort_key)


def covered(records, field, order):
    values = {r[field] for r in records}
    if values == {NOT_APPLICABLE}:
        return NOT_APPLICABLE
    return ", ".join(v for v in order if v in values)


def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


def summary_table(records, lang):
    groups = {}
    for r in records:
        groups.setdefault((r["flavor"], r["version"]), []).append(r)
    rows = []
    for (flavor, version), group in groups.items():
        server_versions = sorted({r["server_version"] for r in group})
        rows.append([
            FLAVOR_NAME[flavor],
            version,
            ", ".join(f"`{v}`" for v in server_versions),
            str(len(group)),
            covered(group, "checksum", CHECKSUM_ORDER),
            covered(group, "image", IMAGE_ORDER),
            covered(group, "metadata", METADATA_ORDER),
            covered(group, "compression", COMPRESSION_ORDER),
            covered(group, "partial_json", PARTIAL_JSON_ORDER),
        ])
    return table(HEADERS[lang]["summary"], rows)


def configs_table(records, lang):
    rows = [[FLAVOR_NAME[r["flavor"]], r["version"], r["checksum"], r["image"], r["metadata"],
             r["compression"], r["partial_json"]] for r in records]
    return table(HEADERS[lang]["configs"], rows)


def replace_region(text, name, body, page):
    begin = f"<!-- compat:{name}:begin -->"
    end = f"<!-- compat:{name}:end -->"
    pattern = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.DOTALL)
    if len(pattern.findall(text)) != 1:
        raise SystemExit(f"{page}: expected exactly one {begin} ... {end} region")
    return pattern.sub(lambda _: f"{begin}\n{body}\n{end}", text)


def render(page, lang, records):
    text = page.read_text(encoding="utf-8")
    text = replace_region(text, "summary", summary_table(records, lang), page)
    return replace_region(text, "configs", configs_table(records, lang), page)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="report pages whose tables are out of date; write nothing")
    args = parser.parse_args()

    records = load_configs()
    stale = []
    for lang, page in PAGES.items():
        current = page.read_text(encoding="utf-8")
        updated = render(page, lang, records)
        if updated == current:
            continue
        stale.append(page)
        if not args.check:
            page.write_text(updated, encoding="utf-8")

    if args.check:
        for page in stale:
            print(f"{page.relative_to(ROOT)} is out of date; run python3 tools/gen_compat_table.py",
                  file=sys.stderr)
        return 1 if stale else 0
    for page in stale:
        print(f"updated {page.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
