"""Build a renamed ASCII JetBrains Mono Bold subset for the dashboard clock."""
import argparse
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="JetBrainsMono Nerd Font Bold TTF")
    args = parser.parse_args()
    font = TTFont(args.source, recalcTimestamp=False)
    options = subset.Options()
    options.hinting = True
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=range(0x20, 0x7F))
    subsetter.subset(font)
    names = {1: "Turing Screen Mono", 2: "Bold", 3: "TuringScreenMono-Bold-1.0",
             4: "Turing Screen Mono Bold", 6: "TuringScreenMono-Bold",
             16: "Turing Screen Mono", 17: "Bold", 21: "Turing Screen Mono", 22: "Bold"}
    for name in font["name"].names:
        if name.nameID in names:
            name.string = names[name.nameID].encode(name.getEncoding(), errors="replace")
    font.save(ROOT / "assets/fonts/TuringScreenMono.ttf")
    (ROOT / "assets/fonts/TuringScreenMono-OFL.txt").write_text(
        (ROOT / "assets/fonts/TuringScreenSymbols-OFL.txt").read_text(encoding="utf-8"), encoding="utf-8")


if __name__ == "__main__":
    main()
