#!/bin/sh
# Tests the Linux launcher's C++ runtime choice (resources/linux/studyboard.in) without real
# runtimes: a fake install prefix (in a directory whose name has spaces) with a stub
# executable that reports LD_LIBRARY_PATH and its arguments and exits with a chosen status,
# a fake bundled libstdc++ and a fake `ldconfig` on PATH naming a fake system libstdc++.
#
# Usage: test_linux_launcher.sh <configured launcher> <runtime dir relative to the prefix>
set -eu

launcher=$1
runtime_dir=$2
unset LD_LIBRARY_PATH STUB_EXIT || true
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
prefix="$scratch/Study Board \$HOME \`x\`"
mkdir -p "$prefix/bin" "$prefix/$runtime_dir" "$scratch/fake bin"
cp -- "$launcher" "$prefix/bin/studyboard"
chmod +x "$prefix/bin/studyboard"
cat > "$prefix/bin/studyapp" <<'EOF'
#!/bin/sh
echo "path=${LD_LIBRARY_PATH:-}"
for argument in "$@"; do echo "arg=$argument"; done
exit "${STUB_EXIT:-0}"
EOF
chmod +x "$prefix/bin/studyapp"
printf 'junk GLIBCXX_3.4.9 GLIBCXX_3.4.32 GLIBCXX_3.4.30 junk' > "$prefix/$runtime_dir/libstdc++.so.6"

failures=0
fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}

# Makes the fake system libstdc++ provide up to GLIBCXX_3.4.<level>.
system_runtime() {
    printf 'GLIBCXX_3.4.1 GLIBCXX_3.4.%s' "$1" > "$scratch/system libstdc++.so.6"
    printf '#!/bin/sh\nprintf "\\tlibstdc++.so.6 (libc6,x86-64) => %s\\n"\n' \
        "$scratch/system libstdc++.so.6" > "$scratch/fake bin/ldconfig"
    chmod +x "$scratch/fake bin/ldconfig"
}

run() {
    PATH="$scratch/fake bin:$PATH" STUDYBOARD_LAUNCHER_DEBUG=1 "$prefix/bin/studyboard" "$@" \
        2> "$scratch/stderr"
}

# Case A: a newer system runtime serves the process; LD_LIBRARY_PATH is left alone.
system_runtime 33
export LD_LIBRARY_PATH=/keep
output=$(run --self-test)
[ "$output" = "path=/keep
arg=--self-test" ] || fail "newer system runtime: $output"
grep -q "C++ runtime: system (bundled GLIBCXX_3.4.32, system GLIBCXX_3.4.33" "$scratch/stderr" ||
    fail "newer system runtime: $(cat "$scratch/stderr")"
unset LD_LIBRARY_PATH

# Same level: the system's.
system_runtime 32
output=$(run)
[ "$output" = "path=" ] || fail "equal runtimes: $output"

# Case B: an older system runtime: the bundled one comes first, the caller's path after it.
system_runtime 30
export LD_LIBRARY_PATH=/keep
output=$(run)
[ "$output" = "path=$prefix/$runtime_dir:/keep" ] || fail "older system runtime: $output"
grep -q "C++ runtime: bundled" "$scratch/stderr" || fail "older: $(cat "$scratch/stderr")"
unset LD_LIBRARY_PATH
output=$(run)
[ "$output" = "path=$prefix/$runtime_dir" ] || fail "older, no caller path: $output"

# Arguments with spaces and shell characters arrive unchanged; the exit status is the
# application's.
system_runtime 33
output=$(run "a b" '$HOME' '*' "") || true
[ "$output" = 'path=
arg=a b
arg=$HOME
arg=*
arg=' ] || fail "arguments: $output"
status=0
export STUB_EXIT=7
run > /dev/null || status=$?
unset STUB_EXIT
[ "$status" = 7 ] || fail "exit status $status, expected 7"

# Started through a symbolic link (as desktop integrations do): the prefix still resolves.
ln -s "$prefix/bin/studyboard" "$scratch/link to studyboard"
if [ -L "$scratch/link to studyboard" ]; then # (not where ln -s copies, as on MSYS)
    output=$(PATH="$scratch/fake bin:$PATH" "$scratch/link to studyboard" linked 2>/dev/null)
    [ "$output" = "path=
arg=linked" ] || fail "symbolic link: $output"
fi

# AppImage layout: the launcher as AppRun at the AppDir's root, the rest under usr/.
appdir="$scratch/App Dir"
mkdir -p "$appdir/usr"
cp -R -- "$prefix/bin" "$prefix/$(echo "$runtime_dir" | cut -d/ -f1)" "$appdir/usr/"
cp -- "$launcher" "$appdir/AppRun"
chmod +x "$appdir/AppRun"
system_runtime 30
output=$(PATH="$scratch/fake bin:$PATH" "$appdir/AppRun" 2>/dev/null)
[ "$output" = "path=$appdir/usr/$runtime_dir" ] || fail "AppRun: $output"

if [ "$failures" -ne 0 ]; then
    echo "$failures launcher check(s) failed"
    exit 1
fi
echo "launcher checks passed"
