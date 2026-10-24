"""Build a small renamed Nerd Fonts icon subset. Requires fontTools only at build time."""
import argparse
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
ICONS = {
    "cpu": 0xF0EE0, "gauge": 0xF04C5, "temperature": 0xF050F, "memory": 0xF035B,
    "gpu": 0xF0379, "download": 0xF01DA, "upload": 0xF0552, "clock": 0xF0150,
    "calendar": 0xF0E17, "volume": 0xF057E, "disk": 0xF02CA, "fan": 0xF0210,
    "power": 0xF06D5, "frequency": 0xF095B,
    "nixos": 0xF313,
    "network": 0xF059F, "network-legacy": 0xF06F3,
    "windows": 0xF05B3, "linux": 0xF033D, "apple": 0xF0035,
    "weather-sunny": 0xF0599, "weather-partly-cloudy": 0xF0595, "weather-cloudy": 0xF0590,
    "weather-fog": 0xF0591, "weather-rainy": 0xF0597, "weather-freezing": 0xF067F,
    "weather-snowy": 0xF0598, "weather-showers": 0xF0596, "weather-thunderstorm": 0xF067E,
    "weather-unknown": 0xF0F2F,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="JetBrainsMono Nerd Font Regular TTF (Nerd Fonts 3.5)")
    args = parser.parse_args()
    font = TTFont(args.source)
    font.recalcTimestamp = False
    missing = set(ICONS.values()) - font.getBestCmap().keys()
    if missing:
        raise ValueError("Missing icon codepoints: " + ", ".join(hex(c) for c in missing))
    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes={0x20, *ICONS.values()})
    subsetter.subset(font)
    names = {1: "Turing Screen Symbols", 2: "Regular", 3: "TuringScreenSymbols-Regular-1.0",
             4: "Turing Screen Symbols", 6: "TuringScreenSymbols-Regular",
             16: "Turing Screen Symbols", 17: "Regular", 21: "Turing Screen Symbols", 22: "Regular"}
    for name in font["name"].names:
        if name.nameID in names:
            name.string = names[name.nameID].encode(name.getEncoding(), errors="replace")
    font.save(ROOT / "assets/fonts/TuringScreenSymbols.ttf")
    license_text = (ROOT / "assets/fonts/OFL.txt").read_text(encoding="utf-8")
    license_text = license_text[license_text.index("This Font Software"):]
    (ROOT / "assets/fonts/TuringScreenSymbols-OFL.txt").write_text(
        "Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)\n\n"
        "Nerd Fonts portions: Copyright (c) 2014 Ryan L McIntyre (https://ryanlmcintyre.com).\n\n"
        + license_text, encoding="utf-8")


if __name__ == "__main__":
    main()
