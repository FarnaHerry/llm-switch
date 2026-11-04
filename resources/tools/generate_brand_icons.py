#!/usr/bin/env python3
"""Export brand assets from lotus_bloom.svg (developer tool: CairoSVG + Pillow)."""

import argparse
import io
from pathlib import Path
import xml.etree.ElementTree as ET

import cairosvg
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "resources/images/lotus_bloom.svg"
SCALES = (1, 2, 3, 4, 8)


def raster(svg, size):
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size * 4, output_height=size * 4)
    with Image.open(io.BytesIO(png)) as image:
        return image.convert("RGBA").resize((size, size), Image.Resampling.LANCZOS)


def png_bytes(image):
    output = io.BytesIO()
    image.save(output, format="PNG")
    return output.getvalue()


def exports():
    source = SOURCE.read_text()
    tree = ET.fromstring(source)
    x, y, width, height = map(float, tree.attrib["viewBox"].split())
    assert x == y == 0 and width == height and width > 0
    assert all(node.attrib.get("fill", "#FFFFFF") == "#FFFFFF" for node in tree.iter())
    # Share the exact mark, including its clear space, across all generated variants.
    mark = source[source.index("  <g"):source.rindex("</svg>")].strip()
    scale = 20.16 / width
    app = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 24 24">\n'
        '  <rect width="24" height="24" rx="5" fill="#000000"/>\n'
        f'  <g transform="translate(1.92 1.92) scale({scale:g})">{mark}</g>\n</svg>\n'
    )
    tray = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">\n'
        '  <circle cx="12" cy="12" r="12" fill="#000000"/>\n'
        f'  <g transform="translate(1.92 1.92) scale({scale:g})">{mark}</g>\n</svg>\n'
    )
    result = {
        ROOT / "resources/images/home.svg": source.encode(),
        ROOT / "platform/windows/package/resources/images/lotus.svg": source.encode(),
        ROOT / "platform/linux/package/llm-switch.svg": app.encode(),
    }
    for scale in SCALES:
        suffix = "" if scale == 1 else f"@{scale}x"
        for name, svg in (("lotus_bloom_raster", source), ("lotus_tray_bloom", tray)):
            result[ROOT / f"resources/images/{name}{suffix}.png"] = png_bytes(raster(svg, 24 * scale))
    output = io.BytesIO()
    raster(app, 256).save(output, format="ICO", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    result[ROOT / "platform/windows/app.ico"] = output.getvalue()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify generated files without writing")
    args = parser.parse_args()
    stale = []
    for path, data in exports().items():
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                stale.append(str(path.relative_to(ROOT)))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    if stale:
        parser.exit(1, "Out-of-date brand assets:\n" + "\n".join(stale) + "\n")
    print("Brand assets verified" if args.check else "Brand assets generated")


if __name__ == "__main__":
    main()
