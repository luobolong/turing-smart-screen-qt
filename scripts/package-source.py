"""Create a source archive without build outputs or local test data."""
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
output = root / "dist/turing-screen-qt-0.2.0-source.zip"
output.parent.mkdir(exist_ok=True)
entries = [root / name for name in ["CMakeLists.txt", "README.md", "LICENSE", "THIRD_PARTY.md",
                                     ".clang-format", ".gitignore"]]
for directory in ["src", "tests", "scripts", "assets", "docs"]:
    entries.extend(sorted(p for p in (root / directory).rglob("*")
                          if p.is_file() and "__pycache__" not in p.parts))
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for path in entries:
        info = zipfile.ZipInfo("turing-screen-qt-0.2.0-source/" + path.relative_to(root).as_posix())
        info.date_time = (2026, 10, 15, 0, 0, 0)
        info.create_system = 3
        info.external_attr = (0o100755 if path.suffix == ".sh" else 0o100644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, path.read_bytes())
print(output, output.stat().st_size)
