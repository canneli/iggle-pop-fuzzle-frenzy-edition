import argparse
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET

HEADER = struct.Struct("<8sB31s31sI")
ENTRY = struct.Struct("<72sI")
MAGIC = b"IGGLEMOD"
KINDS = {"world": 1, "hero": 2}
LIMIT = 12 * 1024 * 1024
WORLD = {"stage.xml", "themebg.jpg", "floor.rgba", "filled.rgba", "wallh.rgba", "wallv.rgba"}
for stem in ("hwallsegment", "vwallsegment", "wallpost", "filledtile", "heart", "swirl"):
    WORLD.update({stem + ".jpg", "_" + stem + ".png"})
WORLD_FLOOR = {"floortile0.jpg", "floortile1.jpg"}
HERO = {name for direction in ("north", "south", "east", "west")
        for name in (f"fizzy{direction}0.jpg", f"_fizzy{direction}0.png")}
HERO_ANIMATION = {name for direction, count in (("north", 3), ("south", 2), ("east", 3), ("west", 3))
                  for frame in range(1, count)
                  for name in (f"fizzy{direction}{frame}.jpg", f"_fizzy{direction}{frame}.png")}


def files_for(kind):
    return (WORLD, WORLD_FLOOR) if kind == 1 else (HERO, HERO_ANIMATION)


def fixed(text, width):
    result = text.encode("ascii")
    if not result or len(result) >= width or any(c < 32 or c > 126 for c in result):
        raise ValueError(f"Expected 1–{width - 1} printable ASCII characters")
    return result


def pack(args):
    kind = KINDS[args.kind]
    if not re.fullmatch("[a-z0-9-]{1,30}", args.id):
        raise ValueError("Invalid package ID")
    required, extended = files_for(kind)
    present = {name for name in extended if (args.folder / name).is_file()}
    if present and present != extended:
        raise ValueError("Include the full extended asset set: " + ", ".join(sorted(extended - present)))
    files = required | present
    parts = [HEADER.pack(MAGIC, kind, fixed(args.id, 31), fixed(args.name, 31), len(files))]
    for name in sorted(files):
        data = (args.folder / name).read_bytes()
        if name == "stage.xml":
            level = ET.fromstring(data)
            if level.tag != "level":
                raise ValueError("stage.xml must contain an Iggle Pop level")
            level.set("editorTheme", f"res/Themes/igglemod-{args.id}.xml")
            data = ET.tostring(level, encoding="utf-8")
        if not data:
            raise ValueError(f"Empty asset: {name}")
        parts.extend((ENTRY.pack(name.encode("ascii"), len(data)), data))
    if sum(map(len, parts)) > LIMIT:
        raise ValueError("Package exceeds 12 MiB")
    args.output.write_bytes(b"".join(parts))


def extract(args):
    if args.archive.stat().st_size > LIMIT:
        raise ValueError("Package exceeds 12 MiB")
    data = args.archive.read_bytes()
    magic, kind, _id, _name, count = HEADER.unpack_from(data)
    if magic != MAGIC or kind not in KINDS.values():
        raise ValueError("Unrecognized package")
    required, extended = files_for(kind)
    allowed = required | extended
    if count not in (len(required), len(allowed)):
        raise ValueError("Invalid asset count")
    offset = HEADER.size
    files = {}
    for _ in range(count):
        raw, size = ENTRY.unpack_from(data, offset)
        name = raw.split(b"\0", 1)[0].decode("ascii")
        offset += ENTRY.size
        if name not in allowed or name in files or not size or offset + size > len(data):
            raise ValueError("Invalid file entry")
        files[name] = data[offset:offset + size]
        offset += size
    if offset != len(data) or set(files) not in (required, allowed):
        raise ValueError("Invalid package contents")
    args.folder.mkdir(parents=True, exist_ok=False)
    for name, content in files.items():
        (args.folder / name).write_bytes(content)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Pack or extract an Iggle Pop mod")
    commands = parser.add_subparsers(dest="action", required=True)
    build = commands.add_parser("pack")
    build.add_argument("kind", choices=KINDS)
    build.add_argument("id")
    build.add_argument("name")
    build.add_argument("folder", type=Path)
    build.add_argument("output", type=Path)
    unpack = commands.add_parser("extract")
    unpack.add_argument("archive", type=Path)
    unpack.add_argument("folder", type=Path)
    args = parser.parse_args()
    try:
        (pack if args.action == "pack" else extract)(args)
    except (ValueError, OSError, struct.error, ET.ParseError) as error:
        parser.error(str(error))
