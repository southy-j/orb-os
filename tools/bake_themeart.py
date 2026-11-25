#!/usr/bin/env python3
"""Bake an .orb theme into a themeart partition image. No SD card required.

The image matches src/theme_art.cpp version 8: an 8 KB index, then sector-aligned
blobs. PNGs become RGB565 (or RGB565+alpha) the same way theme_art_bake.cpp packs
them. Fonts and style JSON are stored raw. A raw asset named "slug" carries the
theme slug, because the index itself only keeps a hash of it.

Steam Punk has no vessel mark. This writes radar_ship.png, a brass ship in the
flight-tracker blip's size and palette, and adds that name to the baked theme.json
asset list. assetsHash is left as the theme shipped it.

    python tools/bake_themeart.py <theme.orb> <themeart.bin>
"""
import json
import os
import struct
import sys

from PIL import Image, ImageDraw

MAGIC = 0x4F524254
VERSION = 8
INDEX_BYTES = 8192
SECTOR = 4096
PART_SIZE = 0x9A0000
FMT_RGB565 = 0
FMT_ALPHA = 1
FMT_RAW = 2
USABLE = PART_SIZE - INDEX_BYTES

ALPHA_MARKS = ("overlay", "hand", "shadow", "sweep", "blip", "ship", "rings", "static", "crank")
SKIP = {"studio.json", "_installed"}


def unpack(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"ORBTHM01":
        raise SystemExit(f"not an Orb theme file (magic is {data[:8]!r})")
    at = 8
    (slug_len,) = struct.unpack_from("<H", data, at)
    at += 2
    slug = data[at:at + slug_len].decode()
    at += slug_len
    (count,) = struct.unpack_from("<H", data, at)
    at += 2
    files = {}
    for _ in range(count):
        (n,) = struct.unpack_from("<H", data, at)
        at += 2
        name = data[at:at + n].decode()
        at += n
        (size,) = struct.unpack_from("<I", data, at)
        at += 4
        files[name] = data[at:at + size]
        at += size
    return slug, files


def fnv1a(text):
    h = 2166136261
    for ch in text.encode():
        h ^= ch
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def wants_alpha(name):
    low = name.lower()
    if "plate" in low:
        return False
    return any(mark in low for mark in ALPHA_MARKS)


def pack_png(png_bytes, alpha):
    im = Image.open(__import__("io").BytesIO(png_bytes)).convert("RGBA")
    w, h = im.size
    px = im.tobytes()
    out = bytearray()
    for i in range(0, len(px), 4):
        r, g, b, a = px[i], px[i + 1], px[i + 2], px[i + 3]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(v & 0xFF)
        out.append(v >> 8)
        if alpha:
            out.append(a)
    return w, h, bytes(out)


def brass_from(im):
    """A colour already in the aircraft mark, so the ship sits in the same metal."""
    px = im.convert("RGBA").tobytes()
    opaque = [px[i:i + 4] for i in range(0, len(px), 4) if px[i + 3] > 200]
    if not opaque:
        return (196, 148, 74, 255), (92, 64, 36, 255), (232, 196, 120, 255)
    opaque.sort(key=lambda p: p[0] + p[1] + p[2])
    mid = tuple(opaque[len(opaque) // 2])
    dark = tuple(opaque[max(0, len(opaque) // 8)])
    light = tuple(opaque[min(len(opaque) - 1, (len(opaque) * 7) // 8)])
    return mid, dark, light


def make_ship(blip_png):
    """Top-down paddle steamer, bow toward 12 o'clock, on the blip's own canvas.

    Drawn several times larger and scaled down so the hull edge is smooth. The beam
    stays well inside the balloon's width: a hull as wide as that canvas reads as a
    house once the bow comes to a point.
    """
    blip = Image.open(__import__("io").BytesIO(blip_png)).convert("RGBA")
    w, h = blip.size
    brass, iron, highlight = brass_from(blip)
    scale = 4
    W, H = w * scale, h * scale
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = W / 2, H / 2
    length = H * 0.78
    beam = W * 0.30
    top = cy - length / 2
    bot = cy + length / 2
    half = beam / 2
    hull = [
        (cx, top),
        (cx + half * 0.42, top + length * 0.16),
        (cx + half, top + length * 0.34),
        (cx + half * 0.96, top + length * 0.62),
        (cx + half * 0.72, bot),
        (cx - half * 0.72, bot),
        (cx - half * 0.96, top + length * 0.62),
        (cx - half, top + length * 0.34),
        (cx - half * 0.42, top + length * 0.16),
    ]
    d.polygon(hull, fill=brass)
    d.line(hull + [hull[0]], fill=iron, width=max(2, scale))
    # Paddle boxes, port and starboard. This is the part that says steamer.
    box_w = half * 0.55
    box_h = length * 0.22
    box_y = top + length * 0.46
    for sign in (-1, 1):
        x = cx + sign * half - (box_w if sign > 0 else 0)
        d.rounded_rectangle([x, box_y, x + box_w, box_y + box_h], radius=scale, fill=iron)
        d.arc([x + scale, box_y + scale, x + box_w - scale, box_y + box_h - scale],
              200 if sign < 0 else 20, 340 if sign < 0 else 160, fill=highlight, width=max(1, scale // 2))
    # Deckhouse, then two funnels along the centreline so the heading is unambiguous.
    house_w = half * 0.70
    house_h = length * 0.28
    house_y = top + length * 0.40
    d.rounded_rectangle(
        [cx - house_w / 2, house_y, cx + house_w / 2, house_y + house_h],
        radius=scale, fill=highlight,
    )
    fw = max(scale * 2, int(half * 0.28))
    fh = int(length * 0.10)
    for fy in (top + length * 0.46, top + length * 0.58):
        d.ellipse([cx - fw / 2, fy, cx + fw / 2, fy + fh], fill=iron)
    d.polygon([(cx, top + scale), (cx - half * 0.12, top + length * 0.08),
               (cx + half * 0.12, top + length * 0.08)], fill=highlight)
    return im.resize((w, h), Image.Resampling.LANCZOS)


def pad_name(text, n):
    raw = text.encode()[: n - 1]
    return raw + b"\x00" * (n - len(raw))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    orb, dest = sys.argv[1], sys.argv[2]
    slug, files = unpack(orb)
    if len(slug) > 31:
        raise SystemExit(f"slug {slug!r} does not fit the firmware's 32-byte buffer")
    if "radar_blip.png" not in files:
        raise SystemExit("theme has no radar_blip.png to size the ship against")

    ship = make_ship(files["radar_blip.png"])
    ship_path = os.path.join(os.path.dirname(__file__), "steam_punk_ship.png")
    ship.save(ship_path)
    import io
    buf = io.BytesIO()
    ship.save(buf, format="PNG")
    files["radar_ship.png"] = buf.getvalue()

    theme = json.loads(files["theme.json"].decode("utf-8"))
    assets = list(theme.get("assets") or [])
    if "radar_ship.png" not in assets:
        assets.append("radar_ship.png")
    theme["assets"] = assets
    # assetsHash stays. The device treats a changed hash as "re-bake from the card",
    # and there is no card. The new name is in the list the device actually loads.
    files["theme.json"] = json.dumps(theme, separators=(",", ":")).encode()
    manifest = int(theme.get("assetsHash") or 0) & 0xFFFFFFFF

    blobs = []

    def add(name, payload, w, h, fmt):
        if len(name) > 23:
            raise SystemExit(f"{name} is longer than the 23-character index name")
        blobs.append((name, payload, w, h, fmt))

    add("slug", slug.encode(), 0, 0, FMT_RAW)
    for name, data in sorted(files.items()):
        if name in SKIP or name.endswith(".pcm"):
            continue
        if name.endswith(".png"):
            alpha = wants_alpha(name)
            w, h, packed = pack_png(data, alpha)
            add(name, packed, w, h, FMT_ALPHA if alpha else FMT_RGB565)
        elif name.endswith(".bin") or name.endswith(".json"):
            add(name, data, 0, 0, FMT_RAW)

    image = bytearray(b"\xFF" * PART_SIZE)
    count = len(blobs)
    if 16 + count * 64 > INDEX_BYTES:
        raise SystemExit(f"{count} assets do not fit the 127-entry index")
    slug_hash = fnv1a(slug)
    cursor = INDEX_BYTES
    for i, (name, payload, w, h, fmt) in enumerate(blobs):
        padded = (len(payload) + SECTOR - 1) & ~(SECTOR - 1)
        if cursor + padded > PART_SIZE:
            raise SystemExit(
                f"out of themeart at {name}: need {cursor + padded} bytes, partition is {PART_SIZE}"
            )
        image[cursor:cursor + len(payload)] = payload
        entry = struct.pack(
            "<I24sIIHHB3sI16s",
            slug_hash,
            pad_name(name, 24),
            cursor,
            len(payload),
            w,
            h,
            fmt,
            b"\x00\x00\x00",
            manifest,
            pad_name(slug, 16),
        )
        if len(entry) != 64:
            raise SystemExit(f"entry packed to {len(entry)} bytes")
        image[16 + i * 64:16 + (i + 1) * 64] = entry
        cursor += padded
    used = cursor - INDEX_BYTES
    header = struct.pack("<IIII", MAGIC, VERSION, count, used)
    image[:16] = header
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(image)
    print(f"slug {slug}")
    print(f"assetsHash {manifest:#010x}")
    print(f"entries {count}")
    print(f"ship {ship.size[0]}x{ship.size[1]} -> {ship_path}")
    print(f"blobs {used} bytes ({used / 1024:.1f} KB) of {USABLE} usable ({USABLE / 1024:.0f} KB)")
    print(f"free after bake {(USABLE - used) / 1024:.1f} KB")
    print(f"image {dest} ({len(image)} bytes)")
    plates = [(n, w, h, len(p)) for n, p, w, h, fmt in blobs if n.endswith("_plate.png") or n in ("splash.png", "wind_bg.png")]
    for n, w, h, ln in plates:
        print(f"  plate {n} {w}x{h} {ln} bytes")


if __name__ == "__main__":
    main()
