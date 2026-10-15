"""Build-time font subset. Requires fontTools; not used by the application."""
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont

root = Path(__file__).resolve().parents[1]
chars = {chr(c) for c in range(0x20, 0x100)}
for lead in range(0xA1, 0xF8):
    for trail in range(0xA1, 0xFF):
        try:
            chars.update(bytes([lead, trail]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
for file in (root / "src").glob("*"):
    if file.is_file():
        chars.update(file.read_text(encoding="utf-8"))
for file in (root / "assets/official").rglob("*"):
    chars.update(file.stem)
chars.update("●＋−×…→←↑↓✓□℃°")
font = TTFont(root / "assets/fonts/NotoSansSC.ttf")
options = subset.Options()
options.name_IDs = ["*"]
options.name_legacy = True
options.name_languages = ["*"]
subsetter = subset.Subsetter(options=options)
subsetter.populate(unicodes={ord(c) for c in chars})
subsetter.subset(font)
names = {1: "Turing Screen Sans", 2: "Regular", 3: "TuringScreenSans-Regular-1.0",
         4: "Turing Screen Sans", 6: "TuringScreenSans-Regular",
         16: "Turing Screen Sans", 17: "Regular"}
for name in font["name"].names:
    if name.nameID in names:
        name.string = names[name.nameID].encode(name.getEncoding(), errors="replace")
font.save(root / "assets/fonts/TuringScreenSans.ttf")
