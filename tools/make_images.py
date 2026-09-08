#!/usr/bin/env python3
"""
Render the 320x240 RGB565 test images that M1 compresses and decompresses.

Why Python and PIL rather than drawing them in C: the point of the corpus is to
be *representative of real screen content*, and the single most important case
-- a terminal full of text -- only compresses realistically if it is rendered
with a real antialiased font at a real size. Faking glyphs with random bitmaps
produces something far noisier than actual text and would make LZ4 look much
worse than it is.

Output: images/<name>.raw, each exactly 320*240*2 = 153600 bytes, RGB565
little-endian, which is the byte order the ILI9341 wants and the byte order the
gud driver sends (drm_fb_xrgb8888_to_rgb565).

Usage:
    python3 make_images.py                    # the synthetic corpus
    python3 make_images.py shot.png           # also add a real screenshot
"""

import os
import sys
import struct
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
OUTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "images")

MONO = "/usr/share/fonts/noto/NotoSansMono-Regular.ttf"


def find_mono_font(size):
    """Any real monospace TTF will do; look in the usual places."""
    candidates = [
        MONO,
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise SystemExit("no monospace TTF found; edit find_mono_font()")


def to_rgb565(im):
    """
    Truncate 8 bits per channel down to 5/6/5 and pack little-endian.

    This is exactly what the kernel does on the host side before sending: it
    keeps the top 5 bits of red, top 6 of green, top 5 of blue and drops the
    rest. No dithering -- gud does not dither for RGB565, only the host's
    XRGB8888 emulation path would.
    """
    im = im.convert("RGB")
    out = bytearray(W * H * 2)
    px = im.load()
    i = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into("<H", out, i, v)
            i += 2
    return bytes(out)


def save(name, im):
    data = to_rgb565(im)
    assert len(data) == W * H * 2
    with open(os.path.join(OUTDIR, name + ".raw"), "wb") as f:
        f.write(data)
    print(f"  {name}.raw")


# ---------------------------------------------------------------- images


def img_solid():
    return Image.new("RGB", (W, H), (0x20, 0x40, 0x80))


def img_flat_ui():
    """A status panel: large flat fills, straight edges, a few colours.
    This is the best case for LZ4 and the case this project is aimed at."""
    bg, panel, accent, light = (0x2E, 0x34, 0x40), (0x3B, 0x42, 0x52), \
                               (0x88, 0xC0, 0xD0), (0xEC, 0xEF, 0xF4)
    im = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(im)
    font = find_mono_font(11)

    d.rectangle([0, 0, W, 23], fill=panel)
    d.text((8, 5), "System Monitor", font=font, fill=light)
    d.rectangle([0, 24, 71, H], fill=panel)
    for i, label in enumerate(["CPU", "MEM", "NET", "DISK", "TEMP", "LOG"]):
        d.rectangle([8, 36 + i * 28, 63, 53 + i * 28], fill=accent)
        d.text((14, 38 + i * 28), label, font=font, fill=bg)
    d.rectangle([84, 36, 307, 155], fill=light)
    d.rectangle([84, 168, 307, 227], fill=panel)
    d.rectangle([88, 172, 303, 175], fill=accent)
    return im


def img_terminal_text():
    """A terminal window: dark background, antialiased monospace text.
    Rendered with a real font so the compression ratio is honest."""
    lines = [
        "rok@misko3 ~/Documents/misko $ ls -la",
        "total 60",
        "drwxr-xr-x  7 rok rok  4096 Aug 25 13:39 .",
        "drwxr-xr-x 66 rok rok  4096 Aug 24 19:08 ..",
        "-rw-r--r--  1 rok rok 12952 Aug 25 13:39 CLAUDE.md",
        "-rw-r--r--  1 rok rok 16321 Aug 25 13:33 FINDINGS.md",
        "drwxr-xr-x  2 rok rok  4096 Aug 24 19:16 backup",
        "drwxr-xr-x  5 rok rok  4096 Aug 24 19:10 MiSKo3",
        "drwxr-xr-x  5 rok rok  4096 Aug 24 19:50 phase1",
        "drwxr-xr-x  4 rok rok  4096 Aug 24 20:19 phase2",
        "drwxr-xr-x 16 rok rok  4096 Aug 24 20:15 tinyusb",
        "",
        "rok@misko3 ~/Documents/misko $ probe-rs list",
        "The following debug probes were found:",
        "[0]: STLink V2-1 -- 0483:374b (ST-LINK)",
        "",
        "rok@misko3 ~/Documents/misko $ cmake --build build",
        "[1/9] Building C object CMakeFiles/m1.dir/src/main.c.o",
        "[9/9] Linking C executable m1_lz4",
        "rok@misko3 ~/Documents/misko $ _",
    ]
    im = Image.new("RGB", (W, H), (0x0C, 0x0C, 0x0C))
    d = ImageDraw.Draw(im)
    font = find_mono_font(9)
    for i, line in enumerate(lines):
        d.text((2, i * 12), line, font=font, fill=(0xD0, 0xD0, 0xD0))
    return im


def img_gradient():
    """Smooth gradient: no exact pixel matches, but each row is nearly a copy
    of the row above, which is the redundancy LZ4 actually finds."""
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = (x * 255 // (W - 1),
                        y * 255 // (H - 1),
                        (x + y) * 255 // (W + H - 2))
    return im


def img_noisy_gradient():
    """The same gradient with per-pixel noise, standing in for a photograph or
    a video frame. Row-to-row matches are destroyed, so LZ4 has almost nothing
    to work with -- this is the case the project accepts will be slow."""
    import random
    random.seed(1234)
    im = img_gradient()
    px = im.load()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + random.randint(-16, 15))),
                        max(0, min(255, g + random.randint(-16, 15))),
                        max(0, min(255, b + random.randint(-16, 15))))
    return im


def img_from_file(path):
    """A real screenshot, scaled to fit the panel. This is the only image in
    the corpus that is not synthetic."""
    im = Image.open(path).convert("RGB")
    im.thumbnail((W, H), Image.LANCZOS)
    canvas = Image.new("RGB", (W, H), (0, 0, 0))
    canvas.paste(im, ((W - im.width) // 2, (H - im.height) // 2))
    return canvas


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    print("writing", OUTDIR)
    save("solid", img_solid())
    save("flat_ui", img_flat_ui())
    save("terminal_text", img_terminal_text())
    save("gradient", img_gradient())
    save("noisy_gradient", img_noisy_gradient())
    for path in sys.argv[1:]:
        name = "screenshot_" + os.path.splitext(os.path.basename(path))[0]
        save(name, img_from_file(path))


if __name__ == "__main__":
    main()
