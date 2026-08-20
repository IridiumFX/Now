#!/usr/bin/env bash
#
# install-wsl.sh — build a Linux `now` inside WSL and install it
#
# Runs INSIDE WSL. ../toolkit/now.exe is a Windows binary and cannot
# drive a toolchain that only exists on the Linux side, so peers who
# cross-compile there (Amy: RISC-V freestanding, clang + ld.lld) need a
# native build rather than a copy of ours.
#
# The previous arrangement was a one-time `cp -r` documented in a build
# guide. It had no way to notice the source had moved on, so a peer sat
# on a six-day-old `now` and reported a bug that had already been fixed
# — the report reads as a live defect until someone checks the date on
# the binary. This is that copy, made repeatable.
#
# Usage, from Git Bash on the Windows side:
#   MSYS_NO_PATHCONV=1 wsl.exe -- bash /mnt/c/Users/Iridium/Projects/Infra/now/tools/install-wsl.sh
# or from inside WSL:
#   bash /mnt/c/Users/Iridium/Projects/Infra/now/tools/install-wsl.sh
#
set -uo pipefail

SRC="${NOW_SRC:-/mnt/c/Users/Iridium/Projects/Infra/now}"
WORK="${NOW_WSL_BUILD:-$HOME/now-build}"
DEST="${NOW_WSL_PREFIX:-$HOME/.local/bin}"
# The installed filename. Defaults to `now` for a peer installing it for
# their own use; the release publisher overrides it so a Linux binary can
# sit beside a Windows one in a shared directory without either shadowing
# the other.
NAME="${NOW_WSL_NAME:-now}"

say() { printf '  %s\n' "$*"; }
die() { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

[ -f "$SRC/CMakeLists.txt" ] || die "no now source at $SRC (set NOW_SRC)"

echo "== source =="
say "from: $SRC"
say "work: $WORK"
# Stamp what was built. Without this there is no way to tell, from the
# installed binary alone, which revision it came from — which is the
# whole problem this script exists to solve.
REV="$(cd "$SRC" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
say "rev:  $REV"

echo "== copy to the Linux filesystem =="
# Building straight off /mnt/c goes through the 9p bridge and is several
# times slower; copying first costs seconds and saves minutes. Excludes
# keep Windows build output and git history out of the copy.
mkdir -p "$WORK" || die "cannot create $WORK"
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete \
          --exclude 'target/' --exclude 'build/' --exclude 'build-*/' \
          --exclude '.git/' --exclude '*.o' --exclude '*.exe' \
          "$SRC"/src "$SRC"/components "$SRC"/lib "$SRC"/CMakeLists.txt \
          "$WORK"/ || die "rsync failed"
else
    rm -rf "$WORK"/src "$WORK"/components "$WORK"/lib "$WORK"/CMakeLists.txt
    cp -r "$SRC"/src "$SRC"/components "$SRC"/lib "$SRC"/CMakeLists.txt "$WORK"/ \
        || die "copy failed"
    find "$WORK" -type d \( -name target -o -name '.git' \) -prune -exec rm -rf {} + 2>/dev/null
fi
say "copied"

echo "== build =="
cd "$WORK" || die "cannot enter $WORK"
# Static on purpose. CMake's default here is a shared libnow, which would
# have to be installed and found alongside the binary; a single file that
# can be copied anywhere is what a peer actually wants.
CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF"
cmake -S . -B build -G Ninja $CMAKE_ARGS >/dev/null 2>&1 \
    || cmake -S . -B build $CMAKE_ARGS >/dev/null 2>&1 \
    || die "cmake configure"
cmake --build build -j "$(nproc)" 2>&1 \
    | grep -E "error|Linking C executable" | tail -3 | sed 's/^/  /'

BIN="$(find "$WORK/build" -name now -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$BIN" ] || die "no binary produced"
say "built: $BIN"

echo "== verify =="
"$BIN" version >/dev/null 2>&1 || die "binary does not run"
say "version: $("$BIN" version 2>&1)"
for cmd in build test procure package keygen; do
    "$BIN" --help 2>&1 | grep -qE "^[[:space:]]+$cmd[[:space:]]" \
        || die "'$cmd' missing from help"
done
say "commands present"

echo "== install =="
mkdir -p "$DEST" || die "cannot create $DEST"
cp "$BIN" "$DEST/$NAME" || die "install failed"
chmod +x "$DEST/$NAME"
printf '%s\n' "$REV" > "$DEST/.${NAME}-revision"
say "installed: $DEST/$NAME  (rev $REV)"

# Make it resolve, rather than assume the distro defaults do it.
#
# Ubuntu's ~/.profile has a `[ -d ~/.local/bin ] && PATH=...` block, and
# on this machine it does not take effect even in a login shell — so
# `now` installed there was not found. Rather than diagnose someone
# else's profile, put the line somewhere we control, once.
MARKER='# added by now install-wsl.sh'
ensure_path_in() {
    local rc="$1"
    [ -e "$rc" ] || return 0
    grep -qF "$MARKER" "$rc" && return 0
    {
        printf '\n%s\n' "$MARKER"
        printf 'case ":$PATH:" in *":%s:"*) ;; *) PATH="%s:$PATH" ;; esac\n' \
               "$DEST" "$DEST"
        printf 'export PATH\n'
    } >> "$rc"
    say "PATH line added to $rc"
}
if [ -n "${NOW_WSL_NO_PATH:-}" ]; then
    say "PATH: left alone (NOW_WSL_NO_PATH set)"
else
case ":$PATH:" in
    *":$DEST:"*) say "on PATH: yes" ;;
    *)
        # .bashrc for interactive shells (how a build is actually run),
        # .profile for login shells. Ubuntu's .bashrc returns early when
        # non-interactive, so a `bash -c` script still needs the full
        # path — printed below.
        ensure_path_in "$HOME/.bashrc"
        ensure_path_in "$HOME/.profile"
        say "open a new shell, or: export PATH=\"$DEST:\$PATH\""
        ;;
esac
fi
say "absolute path (for scripts and non-interactive shells): $DEST/$NAME"

echo
echo "Re-run this after pulling now to stay current. To check what you"
echo "have:  cat $DEST/.${NAME}-revision"
