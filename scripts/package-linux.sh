#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output="dist/turing-screen-qt-0.2.0-linux-x64"
test ! -e "$output" || test "${1:-}" = --refresh || { echo 'Release directory already exists; use --refresh to update it' >&2; exit 1; }
mkdir -p "$output/lib" "$output/plugins/platforms" "$output/plugins/imageformats" "$output/licenses"
cp build/linux/turing-screen-qt "$output/"
cp -au assets docs "$output/"
cp README.md LICENSE THIRD_PARTY.md "$output/"
cp LICENSE "$output/licenses/Qt-GPL-3.0.txt"
for library in Qt6Core Qt6Gui Qt6Widgets Qt6Network Qt6DBus Qt6XcbQpa Qt6WaylandClient; do
    test ! -f "/usr/lib/lib$library.so.6" || cp -Lu "/usr/lib/lib$library.so.6" "$output/lib/"
done
for kind in wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client wayland-windowmanager-integration tls networkinformation; do
    test ! -d "/usr/lib/qt6/plugins/$kind" || cp -a "/usr/lib/qt6/plugins/$kind" "$output/plugins/"
done
for library in /usr/lib/libpulse.so.0 /usr/lib/pulseaudio/libpulsecommon-*.so; do
    test ! -f "$library" || cp -Lu "$library" "$output/lib/"
done
for library in /usr/lib/libicu{uc,i18n,data}.so.[0-9]* /usr/lib/libdouble-conversion.so.[0-9]* /usr/lib/libstdc++.so.6 /usr/lib/libgcc_s.so.1; do
    [[ "${library##*/}" =~ \.so\.[0-9]+$ ]] || continue
    test ! -f "$library" || cp -Lu "$library" "$output/lib/"
done
for plugin in libqxcb.so libqwayland.so libqwayland-egl.so libqoffscreen.so; do
    test ! -f "/usr/lib/qt6/plugins/platforms/$plugin" || cp "/usr/lib/qt6/plugins/platforms/$plugin" "$output/plugins/platforms/"
done
cp /usr/lib/qt6/plugins/imageformats/libqjpeg.so "$output/plugins/imageformats/"
for package in qt6-base qt6-wayland icu double-conversion gcc-libs libpulse pulseaudio; do
    test ! -d "/usr/share/licenses/$package" || cp -a "/usr/share/licenses/$package" "$output/licenses/"
done
printf '[Paths]\nPrefix=.\nPlugins=plugins\n' > "$output/qt.conf"
cat > "$output/run.sh" <<'SH'
#!/usr/bin/env sh
set -eu
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$app_dir/plugins"
exec "$app_dir/turing-screen-qt" "$@"
SH
chmod +x "$output/run.sh" "$output/turing-screen-qt"
{ uname -m; getconf GNU_LIBC_VERSION; ldd --version | head -n 1; } > "$output/BUILD-ENVIRONMENT.txt"
tar -I 'gzip -1' -cf "$output.tar.gz" -C dist "$(basename "$output")"
ls -lh "$output.tar.gz"
