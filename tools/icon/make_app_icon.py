"""Writes src/ui-win32/FloServer.ico, the icon built into FloServer.exe.

The same drawing as make_app_icon() in src/ui-win32/canvas.cpp (which the
window and the tray draw at run time): on a 16-unit grid, a #0F6CBD rounded
square (1,1)-(15,15) with radius 3, and a white F, 1.6 units wide (at least
1 px), mitred at the top-left corner, flat ends. Drawn 8x larger and scaled
down, so the edges are smooth at every size.

Run from the repository root (needs Pillow):
    python3 tools/icon/make_app_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
BLUE = (0x0F, 0x6C, 0xBD, 255)
WHITE = (255, 255, 255, 255)
OVERSAMPLE = 8


def draw(size: int) -> Image.Image:
    big = size * OVERSAMPLE
    s = big / 16.0
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)
    d.rounded_rectangle([1 * s, 1 * s, 15 * s, 15 * s], radius=3 * s, fill=BLUE)
    half = max(1.0 * OVERSAMPLE, 1.6 * s) / 2.0
    # The stem, from the top bar's edge (the mitred corner) down to y = 11.
    d.rectangle([5 * s - half, 5 * s - half, 5 * s + half, 11 * s], fill=WHITE)
    # The top bar, to x = 11.
    d.rectangle([5 * s - half, 5 * s - half, 11 * s, 5 * s + half], fill=WHITE)
    # The middle bar, to x = 9.
    d.rectangle([5 * s, 8 * s - half, 9 * s, 8 * s + half], fill=WHITE)
    return image.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = Path("src/ui-win32/FloServer.ico")
    images = [draw(n) for n in SIZES]
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
