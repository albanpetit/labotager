#!/usr/bin/env python3
"""
Convertisseur d'image vers tableau RGB565 pour ST7789 (320x240)

Usage :
    python3 tools/convert_image.py <chemin_image> [--output src/image.h]

Dépendances :
    pip install Pillow

Exemple :
    python3 tools/convert_image.py tools/photo.jpg
    python3 tools/convert_image.py tools/logo.png --output src/image.h
"""

import sys
import argparse
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Erreur : Pillow est requis.")
    print("Installez-le avec : pip install Pillow")
    sys.exit(1)

TARGET_W = 320
TARGET_H = 240


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    """Convertit un pixel RGB888 (3x8 bits) en RGB565 (16 bits)."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert(input_path: str, output_path: str) -> None:
    src = Path(input_path)
    if not src.exists():
        print(f"Erreur : fichier introuvable : {src}")
        sys.exit(1)

    print(f"Ouverture de {src.name}...")
    img = Image.open(src).convert("RGB")

    # Recadrage pour conserver les proportions, puis redimensionnement
    img_ratio = img.width / img.height
    target_ratio = TARGET_W / TARGET_H

    if img_ratio > target_ratio:
        # Image trop large : recadrage gauche/droite
        new_w = int(img.height * target_ratio)
        offset = (img.width - new_w) // 2
        img = img.crop((offset, 0, offset + new_w, img.height))
    elif img_ratio < target_ratio:
        # Image trop haute : recadrage haut/bas
        new_h = int(img.width / target_ratio)
        offset = (img.height - new_h) // 2
        img = img.crop((0, offset, img.width, offset + new_h))

    img = img.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    print(f"Image redimensionnée : {TARGET_W}x{TARGET_H}")

    pixels = []
    for y in range(TARGET_H):
        for x in range(TARGET_W):
            r, g, b = img.getpixel((x, y))
            pixels.append(rgb888_to_rgb565(r, g, b))

    total = len(pixels)
    print(f"Conversion RGB565 : {total} pixels ({total * 2} octets)")

    out = Path(output_path)
    with open(out, "w") as f:
        f.write("#pragma once\n\n")
        f.write("// ------------------------------------------------------------\n")
        f.write(f"// Données image RGB565 pour affichage ST7789 {TARGET_W}x{TARGET_H}\n")
        f.write(f"// Source : {src.name}\n")
        f.write("// Généré automatiquement par tools/convert_image.py\n")
        f.write("// ------------------------------------------------------------\n\n")
        f.write("#define IMAGE_DATA image_data\n\n")
        f.write(f"const uint16_t image_data[{TARGET_W} * {TARGET_H}] = {{\n")

        for i, px in enumerate(pixels):
            if i % 16 == 0:
                f.write("  ")
            f.write(f"0x{px:04X},")
            if (i + 1) % 16 == 0:
                f.write("\n")

        f.write("\n};\n")

    print(f"Fichier généré : {out}")
    print(f"-> Décommentez '#include \"image.h\"' dans src/main.cpp")


def main():
    parser = argparse.ArgumentParser(description="Convertit une image en tableau RGB565 pour ST7789")
    parser.add_argument("input", help="Chemin de l'image source (PNG, JPG, BMP...)")
    parser.add_argument("--output", default="src/image.h", help="Fichier de sortie (défaut : src/image.h)")
    args = parser.parse_args()

    convert(args.input, args.output)


if __name__ == "__main__":
    main()
