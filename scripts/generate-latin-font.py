"""Build small, renamed Inter Regular/SemiBold subsets for the English dashboard."""
import argparse
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Inter 4.1 Inter.ttc collection")
    args = parser.parse_args()
    characters = set(range(0x20, 0x250)) | set(range(0x300, 0x370)) | {0x2013, 0x2014, 0x2026}
    for index, style in ((0, "Regular"), (12, "SemiBold")):
        font = TTFont(args.source, fontNumber=index, recalcTimestamp=False)
        options = subset.Options()
        options.hinting = True
        options.name_IDs = ["*"]
        options.name_legacy = True
        options.name_languages = ["*"]
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(unicodes=characters)
        subsetter.subset(font)
        names = {1: "Turing Screen Latin", 2: style, 3: "TuringScreenLatin-" + style + "-1.0",
                 4: "Turing Screen Latin " + style, 6: "TuringScreenLatin-" + style,
                 16: "Turing Screen Latin", 17: style, 21: "Turing Screen Latin", 22: style}
        for name in font["name"].names:
            if name.nameID in names:
                name.string = names[name.nameID].encode(name.getEncoding(), errors="replace")
        font.save(ROOT / ("assets/fonts/TuringScreenLatin-" + style + ".ttf"))
    license_text = (ROOT / "assets/fonts/OFL.txt").read_text(encoding="utf-8")
    license_text = license_text[license_text.index("This Font Software"):]
    (ROOT / "assets/fonts/TuringScreenLatin-OFL.txt").write_text(
        "Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)\n\n"
        + license_text, encoding="utf-8")


if __name__ == "__main__":
    main()
