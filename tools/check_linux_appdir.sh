#!/bin/sh
# Checks a Linux AppDir made by linuxdeploy (.github/workflows/package.yml) before it is
# packed: the launcher is AppRun, the C++ runtime is only in lib/studyboard-cxxrt (never on
# the executable's library path), the Qt modules and plugins the application needs are
# there, no link is broken and no binary refers to a directory of the build machine.
# Problems are printed as "AppDir: ..." lines; the exit status is 1 if there are any.
#
# Usage: check_linux_appdir.sh <AppDir>
set -eu

appdir=$1
usr="$appdir/usr"
problems=0
problem() {
    echo "AppDir: $*"
    problems=$((problems + 1))
}

[ -x "$usr/bin/studyapp" ] || problem "usr/bin/studyapp is missing"
[ -x "$usr/bin/studyboard" ] || problem "usr/bin/studyboard (launcher) is missing"
cmp -s "$appdir/AppRun" "$usr/bin/studyboard" || problem "AppRun is not the launcher"
grep -q '^Exec=studyboard' "$usr/share/applications/studyboard.desktop" ||
    problem "the desktop entry does not start the launcher"

for runtime in libstdc++.so.6 libgcc_s.so.1; do
    [ -f "$usr/lib/studyboard-cxxrt/$runtime" ] || problem "lib/studyboard-cxxrt/$runtime is missing"
    [ ! -e "$usr/lib/$runtime" ] || problem "lib/$runtime would override the system's runtime"
done

for module in Core Gui Widgets OpenGL OpenGLWidgets Pdf PrintSupport Svg DBus XcbQpa; do
    [ -e "$usr/lib/libQt6$module.so.6" ] || problem "Qt module $module is missing"
done
for plugin in platforms/libqxcb.so platforms/libqoffscreen.so \
    xcbglintegrations/libqxcb-glx-integration.so imageformats/libqjpeg.so \
    imageformats/libqsvg.so; do
    [ -e "$usr/plugins/$plugin" ] || problem "Qt plugin $plugin is missing"
done

broken=$(find "$appdir" -xtype l)
[ -z "$broken" ] || problem "broken links: $broken"

# RPATH/RUNPATH entries must be relative ($ORIGIN); absolute ones point at this machine.
find "$usr" -type f \( -name '*.so*' -o -path '*/bin/studyapp' \) | while read -r file; do
    if paths=$(readelf -d "$file" 2>/dev/null | sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p'); then
        for entry in $(echo "$paths" | tr ':' ' '); do
            case "$entry" in
            '$ORIGIN'*) ;;
            *) echo "AppDir: $file has the library path $entry" ;;
            esac
        done
    fi
done > "$appdir/../rpath-problems.txt"
if [ -s "$appdir/../rpath-problems.txt" ]; then
    cat "$appdir/../rpath-problems.txt"
    problems=$((problems + 1))
fi
rm -f "$appdir/../rpath-problems.txt"

if [ "$problems" -ne 0 ]; then
    exit 1
fi
echo "AppDir checks passed"
