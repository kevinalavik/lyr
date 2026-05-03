#!/usr/bin/env python3
import os
import stat
import sys


def align4(value):
    return (value + 3) & ~3


def pad4(out):
    while out.tell() != align4(out.tell()):
        out.write(b"\0")


def field(value):
    return f"{value & 0xFFFFFFFF:08x}".encode("ascii")


def write_entry(out, name, mode, uid, gid, nlink, data=b""):
    name_bytes = name.encode("utf-8") + b"\0"
    header = b"".join(
        [
            b"070701",
            field(0),
            field(mode),
            field(uid),
            field(gid),
            field(nlink),
            field(0),
            field(len(data)),
            field(0),
            field(0),
            field(0),
            field(0),
            field(len(name_bytes)),
            field(0),
        ]
    )
    out.write(header)
    out.write(name_bytes)
    pad4(out)
    out.write(data)
    pad4(out)


def collect_entries(root, output):
    entries = []
    root = os.path.abspath(root)
    output = os.path.abspath(output)

    for current, dirs, files in os.walk(root):
        dirs.sort()
        files.sort()
        dirs[:] = [d for d in dirs if os.path.abspath(os.path.join(current, d)) != output]

        rel_dir = os.path.relpath(current, root)
        if rel_dir != ".":
            if os.path.abspath(current) == output:
                continue
            st = os.lstat(current)
            mode = stat.S_IFDIR | stat.S_IMODE(st.st_mode)
            entries.append((rel_dir, mode, 0, 0, 2, b""))

        for filename in files:
            path = os.path.join(current, filename)
            if os.path.abspath(path) == output:
                continue
            rel = os.path.relpath(path, root)
            st = os.lstat(path)
            if not stat.S_ISREG(st.st_mode):
                continue
            with open(path, "rb") as f:
                data = f.read()
            mode = stat.S_IFREG | stat.S_IMODE(st.st_mode)
            entries.append((rel, mode, 0, 0, 1, data))

    return entries


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <root-dir> <output.cpio>", file=sys.stderr)
        return 2

    root, output = argv[1], argv[2]
    entries = collect_entries(root, output)
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)

    with open(output, "wb") as out:
        for entry in entries:
            write_entry(out, *entry)
        write_entry(out, "TRAILER!!!", 0, 0, 0, 1)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
