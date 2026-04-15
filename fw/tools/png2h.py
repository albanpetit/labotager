#!/usr/bin/env python3
"""
png2h.py — Convert PNG assets to C header files with pre-decoded RGB565 data.

Eliminates runtime PNG decoding (zlib + pixel conversion) for faster display.
Output is directly compatible with TFT_eSPI pushImage / pushMaskedImage using
setSwapBytes(false) on a little-endian RP2040 (Raspberry Pi Pico).

─── Byte order ───────────────────────────────────────────────────────────────
With setSwapBytes(false), TFT_eSPI sends buffer bytes in LE memory order
(low byte first). The ST7789 expects the high byte of RGB565 first over SPI.
Therefore the uint16_t values stored in the array must be byte-swapped compared
to standard RGB565:

  Red in standard RGB565 : 0xF800  (high byte = 0xF8 = RRRRRGGG)
  Stored in the array    : 0x00F8  → LE memory: [0xF8, 0x00]
                                   → SPI sends 0xF8 first ✓

─── Alpha mask format ────────────────────────────────────────────────────────
The mask layout matches PNGdec getAlphaMask() and TFT_eSPI pushMaskedImage():
  • ceil(width/8) bytes per row, no padding between rows
  • Bit 7 of each byte = leftmost pixel of the group
  • Bit value 1 = opaque (alpha ≥ 128), 0 = transparent

─── Usage ────────────────────────────────────────────────────────────────────
  # Single file, output next to input
  python3 tools/png2h.py src/ui/components/background.png --opaque

  # All components at once, output to the same directory
  python3 tools/png2h.py src/ui/components/*.png --opaque-list background

  # Resize to 320×240 before converting (background replacement workflow)
  python3 tools/png2h.py photo.jpg --resize 320 240 --opaque --output src/ui/components/background.h

─── Firmware integration ─────────────────────────────────────────────────────
After conversion, replace the PNG-based draw calls with direct pushImage calls:

  Opaque (e.g. background):
    _tft->pushImage(0, 0, BACKGROUND_W, BACKGROUND_H, background);

  With alpha (e.g. icons, widgets):
    _tft->pushMaskedImage(x, y, ICON_W, ICON_H, icon, icon_mask);
"""

import argparse
import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow is required.  Install with:  pip install Pillow", file=sys.stderr)
    sys.exit(1)


# ─── Pixel conversion ─────────────────────────────────────────────────────────

def to_rgb565_be(r: int, g: int, b: int) -> int:
    """
    Convert (r, g, b) 0-255 to the uint16_t value for a PROGMEM array used
    with TFT_eSPI setSwapBytes(false) on a little-endian CPU.

    Standard RGB565 = RRRRRGGG GGGBBBBB (high byte first = big-endian SPI).
    Stored uint16_t must have the high byte of RGB565 in the low memory address
    so that SPI (which sends low address first on LE) delivers it to the display
    as the first byte.  This means byte-swapping the standard RGB565 value.
    """
    std = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((std >> 8) | (std << 8)) & 0xFFFF


# ─── Core conversion ──────────────────────────────────────────────────────────

def convert(input_path: Path, output_path: Path, var_name: str,
            emit_mask: bool, resize: tuple | None) -> None:
    img = Image.open(input_path)

    # Resize with crop-to-fit if requested
    if resize:
        target_w, target_h = resize
        img_ratio    = img.width / img.height
        target_ratio = target_w / target_h
        if img_ratio > target_ratio:
            new_w  = int(img.height * target_ratio)
            offset = (img.width - new_w) // 2
            img    = img.crop((offset, 0, offset + new_w, img.height))
        elif img_ratio < target_ratio:
            new_h  = int(img.width / target_ratio)
            offset = (img.height - new_h) // 2
            img    = img.crop((0, offset, img.width, offset + new_h))
        img = img.resize((target_w, target_h), Image.LANCZOS)

    has_alpha = img.mode in ('RGBA', 'LA', 'PA') or \
                (img.mode == 'P' and 'transparency' in img.info)
    img = img.convert('RGBA' if has_alpha else 'RGB')

    width, height = img.size
    px = img.load()

    rgb_vals   = []
    mask_bytes = []
    row_mask_bytes = (width + 7) // 8

    for y in range(height):
        # Build mask row
        if emit_mask:
            for bx in range(row_mask_bytes):
                byte = 0
                for bit in range(8):
                    x = bx * 8 + bit
                    if x < width:
                        alpha = px[x, y][3] if has_alpha else 255
                        if alpha >= 128:
                            byte |= (0x80 >> bit)
                mask_bytes.append(byte)
        # Build pixel row
        for x in range(width):
            p = px[x, y]
            rgb_vals.append(to_rgb565_be(p[0], p[1], p[2]))

    # ── Write header ──────────────────────────────────────────────────────────
    COLS = 16  # uint16_t values per line
    pix_bytes  = len(rgb_vals) * 2
    mask_sz    = len(mask_bytes)
    guard      = var_name.upper() + '_H'

    with open(output_path, 'w') as f:
        f.write(f'// Auto-generated by tools/png2h.py — do not edit manually.\n')
        f.write(f'// Source : {input_path.name}\n')
        f.write(f'// Format : RGB565, BE-TFT byte order (TFT_eSPI setSwapBytes(false))\n')
        if emit_mask:
            f.write(f'//          + 1-bit alpha mask (bit 7 = left pixel, threshold 128)\n')
        f.write(f'//\n')
        f.write(f'// Firmware usage:\n')
        if emit_mask:
            f.write(f'//   _tft->pushMaskedImage(x, y, {var_name.upper()}_W, {var_name.upper()}_H,\n')
            f.write(f'//                         {var_name}, {var_name}_mask);\n')
        else:
            f.write(f'//   _tft->pushImage(x, y, {var_name.upper()}_W, {var_name.upper()}_H, {var_name});\n')
        f.write(f'#pragma once\n')
        f.write(f'#include <Arduino.h>\n\n')
        f.write(f'#define {var_name.upper()}_W  {width}\n')
        f.write(f'#define {var_name.upper()}_H  {height}\n\n')

        # RGB565 array
        f.write(f'// {width}×{height} pixels · {pix_bytes} bytes\n')
        f.write(f'static const uint16_t {var_name}[] PROGMEM = {{\n')
        for i, v in enumerate(rgb_vals):
            if i % COLS == 0:
                f.write('  ')
            f.write(f'0x{v:04X}')
            if i < len(rgb_vals) - 1:
                f.write(', ')
            if (i + 1) % COLS == 0:
                f.write('\n')
        if len(rgb_vals) % COLS != 0:
            f.write('\n')
        f.write('};\n')

        # Mask array
        if emit_mask:
            MCOLS = row_mask_bytes  # one row per line
            f.write(f'\n// 1-bit alpha mask · {mask_sz} bytes '
                    f'({row_mask_bytes} bytes/row × {height} rows)\n')
            f.write(f'static const uint8_t {var_name}_mask[] PROGMEM = {{\n')
            for i, v in enumerate(mask_bytes):
                if i % MCOLS == 0:
                    f.write('  ')
                f.write(f'0x{v:02X}')
                if i < len(mask_bytes) - 1:
                    f.write(', ')
                if (i + 1) % MCOLS == 0:
                    f.write('\n')
            if len(mask_bytes) % MCOLS != 0:
                f.write('\n')
            f.write('};\n')

    mode = 'RGB565 + mask' if emit_mask else 'RGB565 opaque'
    size = f'{pix_bytes} B' + (f' + {mask_sz} B mask' if emit_mask else '')
    print(f'  {input_path.name:<40} → {output_path.name:<40} [{width}×{height}  {mode}  {size}]')


# ─── CLI ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Convert PNG/JPG files to TFT_eSPI-compatible RGB565 C headers',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    parser.add_argument('inputs', nargs='+', metavar='FILE',
                        help='Input image file(s) (PNG, JPG, BMP…)')
    parser.add_argument('--output-dir', metavar='DIR',
                        help='Output directory (default: same directory as each input)')
    parser.add_argument('--opaque', action='store_true',
                        help='Omit alpha mask for all inputs (use for solid backgrounds)')
    parser.add_argument('--opaque-list', nargs='+', metavar='NAME', default=[],
                        help='Comma-separated variable names to treat as opaque '
                             '(all others get a mask)')
    parser.add_argument('--resize', nargs=2, type=int, metavar=('W', 'H'),
                        help='Resize (with crop-to-fit) to W×H before converting')
    args = parser.parse_args()

    opaque_set = {n.lower().replace('-', '_') for n in args.opaque_list}

    errors = 0
    for raw_path in args.inputs:
        inp = Path(raw_path)
        if not inp.is_file():
            print(f'ERROR: not found: {inp}', file=sys.stderr)
            errors += 1
            continue

        var_name = inp.stem.replace('-', '_').replace(' ', '_')

        if args.output_dir:
            out_dir = Path(args.output_dir)
            out_dir.mkdir(parents=True, exist_ok=True)
            out = out_dir / (var_name + '.h')
        else:
            out = inp.with_suffix('.h')

        force_opaque = args.opaque or (var_name.lower() in opaque_set)

        # Decide whether to emit a mask:
        # • forced opaque → no mask
        # • otherwise → check if image actually has alpha; if not, skip mask
        try:
            img_check = Image.open(inp)
            has_alpha  = img_check.mode in ('RGBA', 'LA', 'PA') or \
                         (img_check.mode == 'P' and 'transparency' in img_check.info)
            img_check.close()
        except Exception as e:
            print(f'ERROR reading {inp}: {e}', file=sys.stderr)
            errors += 1
            continue

        emit_mask = has_alpha and not force_opaque
        resize    = tuple(args.resize) if args.resize else None

        try:
            convert(inp, out, var_name, emit_mask, resize)
        except Exception as e:
            print(f'ERROR converting {inp}: {e}', file=sys.stderr)
            errors += 1

    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
