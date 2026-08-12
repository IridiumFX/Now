#!/usr/bin/env bash
#
# install-toolkit.sh — build now and install it to ../toolkit/now.exe
#
# ../toolkit is on the user PATH, so whatever sits there is the `now`
# every project on this machine gets by default. Updating it by hand
# meant it drifted behind the source tree, and the failure mode is
# quiet: a peer reports a bug that was fixed days ago, and the report
# looks like a live defect until someone checks the binary's date.
#
# Verifies before it installs, and installs nothing if a check fails —
# a half-updated toolkit is worse than a stale one.
#
# Usage:  tools/install-toolkit.sh [--skip-tests]
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="$(cd "$REPO/.." && pwd)/../toolkit"
TOOLKIT="$(cd "$(dirname "$TOOLKIT")" 2>/dev/null && pwd)/toolkit" || TOOLKIT="$REPO/../../toolkit"
SKIP_TESTS=0
[ "${1:-}" = "--skip-tests" ] && SKIP_TESTS=1

say()  { printf '  %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

# The toolchain is bundled in CLion and the version moves; never pin it.
# The junction is preferred when present because a CLion upgrade only
# needs it repointed, but resolve dynamically if it is missing.
if [ -x /c/Users/Iridium/toolchain/mingw/bin/gcc.exe ]; then
    MINGW=/c/Users/Iridium/toolchain/mingw/bin
else
    MINGW="$(ls -d '/c/Program Files/JetBrains/CLion '*/bin/mingw/bin 2>/dev/null | tail -1)"
fi
CL="$(ls -d '/c/Program Files/JetBrains/CLion '* 2>/dev/null | tail -1)"
[ -n "$MINGW" ] || die "no mingw toolchain found"
export PATH="$MINGW:$CL/bin/cmake/win/x64/bin:$CL/bin/ninja/win/x64:$PATH"

echo "== toolchain =="
say "gcc:   $(gcc --version | head -1)"
say "where: $MINGW"

echo "== build (static release) =="
cd "$REPO" || die "cannot enter $REPO"
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF >/dev/null 2>&1 \
      || die "cmake configure"
# CMake uses an explicit source list while `now build` walks the tree, so
# this is also the check that catches a vendored source added upstream
# and not registered here — it shows up only as an undefined reference.
cmake --build build/release --target now_cli -j "$(nproc 2>/dev/null || echo 8)" 2>&1 \
    | grep -E "error|Linking C executable" | tail -3 | sed 's/^/  /'
BIN="$REPO/build/release/bin/now.exe"
[ -f "$BIN" ] || die "no binary produced"

echo "== verify =="
VER="$("$BIN" version 2>&1)" || die "binary does not run"
say "version: $VER"

# Must be self-contained: ../toolkit has no libnow.dll beside it, and a
# binary that needs one fails at load with nothing useful to say.
if objdump -p "$BIN" 2>/dev/null | grep -qi libnow; then
    die "binary depends on libnow.dll — build is not static"
fi
say "self-contained: yes (no libnow.dll import)"

# Anchored on the command column: a loose grep here once "found" a
# `sign` command that does not exist, by matching the word "signing" in
# keygen's description. Signing happens inside `now package`.
for cmd in build test procure package publish keygen verify ci; do
    "$BIN" --help 2>&1 | grep -qE "^[[:space:]]+$cmd[[:space:]]" \
        || die "'$cmd' missing from help"
done
say "commands present"

if [ "$SKIP_TESTS" -eq 0 ]; then
    echo "== self-test =="
    "$BIN" test -j "$(nproc 2>/dev/null || echo 8)" 2>&1 \
        | grep -E "tests passed|FAIL" | tail -2 | sed 's/^/  /'
    "$BIN" test -j "$(nproc 2>/dev/null || echo 8)" >/dev/null 2>&1 \
        || die "self-test failed — not installing"
fi

echo "== install =="
[ -d "$TOOLKIT" ] || die "toolkit directory not found at $TOOLKIT"
# A running now.exe cannot be overwritten on Windows; this is also why
# `now` cannot rebuild its own installed binary in place.
taskkill //F //IM now.exe >/dev/null 2>&1
cp "$BIN" "$TOOLKIT/now.exe" || die "copy to $TOOLKIT failed"

# Prove what landed is what we built, rather than trusting the copy.
A="$(md5sum "$BIN"            | cut -d' ' -f1)"
B="$(md5sum "$TOOLKIT/now.exe" | cut -d' ' -f1)"
[ "$A" = "$B" ] || die "installed binary does not match built binary"
say "installed: $TOOLKIT/now.exe"
say "md5:       $B"
say "reports:   $("$TOOLKIT/now.exe" version)"

echo
echo "Done. This binary is Windows-only. Peers who cross-compile under"
echo "WSL cannot use it — a Windows now.exe cannot drive a toolchain that"
echo "only exists on the Linux side. For them:  tools/install-wsl.sh"
