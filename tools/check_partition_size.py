#!/usr/bin/env python3
# ENH-4: Validate the C3 partition CSV before build.
import csv
import sys


def parse_size(value):
    value = value.strip()
    if not value:
        raise ValueError("empty Size")
    return int(value, 0)


def main():
    if len(sys.argv) != 2:
        print("usage: check_partition_size.py <partitions.csv>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        with open(path, newline="", encoding="utf-8") as handle:
            rows = []
            for raw in handle:
                if not raw.strip() or raw.lstrip().startswith("#"):
                    continue
                row = next(csv.reader([raw]))
                if len(row) != 6:
                    print(f"ERROR: invalid column count in row: {raw.rstrip()}", file=sys.stderr)
                    return 1
                rows.append(row)
    except OSError as exc:
        print(f"ERROR: cannot read partition file {path}: {exc}", file=sys.stderr)
        return 1

    if rows and [c.strip().lower() for c in rows[0]] == [
        "name", "type", "subtype", "offset", "size", "flags"
    ]:
        rows = rows[1:]

    partitions = {}
    total = 0
    for row in rows:
        name, _type, _subtype, _offset, size_text, _flags = (c.strip() for c in row)
        try:
            size = parse_size(size_text)
        except ValueError:
            print(f"ERROR: invalid Size for partition {name}: {size_text}", file=sys.stderr)
            return 1
        if size % 0x1000 != 0:
            print(f"ERROR: partition {name} Size 0x{size:X} is not a 4KB multiple", file=sys.stderr)
            return 1
        partitions[name] = size
        total += size

    if total > 0x800000:
        print(f"ERROR: total partition size 0x{total:X} exceeds 8MB", file=sys.stderr)
        return 1

    if "app0" in partitions and "app1" in partitions and partitions["app0"] != partitions["app1"]:
        print(
            f"ERROR: app0 Size 0x{partitions['app0']:X} != "
            f"app1 Size 0x{partitions['app1']:X}",
            file=sys.stderr,
        )
        return 1

    if "nvs" not in partitions or partitions["nvs"] < 0x4000:
        size = partitions.get("nvs", 0)
        print(f"ERROR: nvs Size 0x{size:X} is below minimum 0x4000", file=sys.stderr)
        return 1

    print("OK: partition layout valid for C3")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
