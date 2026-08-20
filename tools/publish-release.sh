#!/usr/bin/env bash
#
# publish-release.sh — put BOTH `now` binaries in the shared toolkit, and
# record what was published.
#
# There are two `now` binaries and they are not interchangeable. A
# Windows now.exe cannot drive a cross-toolchain that only exists on the
# Linux side, so a peer building a freestanding RISC-V kernel under WSL
# gets nothing at all from an updated ../toolkit/now.exe. That was the
# actual state after three release candidates: every Windows project on
# this machine had rc6 and the one team that reported the defect being
# fixed was still running rc4, because nobody had run install-wsl.sh.
#
# So both are published to one place a peer can look at:
#
#   toolkit/now.exe          Windows x64, on the user PATH — what every
#                            Windows project here gets by default
#   toolkit/now-linux-x64    Linux x64, static — copy it wherever you
#                            want it; it depends on nothing
#   toolkit/now-RELEASE.md   what the two are, and when
#
# Publishing is deliberately NOT installing into anyone's environment.
# Peers who cross-compile take a new binary at the start of a session,
# not the end, because swapping a compiler mid-session turns one clean
# measurement into two variables. This makes the latest one available and
# lets them choose the moment.
#
# Usage, from Git Bash on the Windows side:
#   tools/publish-release.sh [--skip-tests]
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="$(cd "$REPO/../.." && pwd)/toolkit"
SKIP="${1:-}"

say()  { printf '  %s\n' "$*"; }
die()  { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }

[ -d "$TOOLKIT" ] || die "no toolkit directory at $TOOLKIT"

REV="$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY="$(cd "$REPO" && git status --porcelain 2>/dev/null | wc -l)"

echo "== publishing now =="
say "repo: $REPO"
say "rev:  $REV"
if [ "$DIRTY" -ne 0 ]; then
    # Refuse rather than warn. A published binary whose revision stamp
    # does not describe it is the exact failure this whole arrangement
    # exists to prevent — a peer reported a bug that was already fixed
    # because nothing could tell them what they were running.
    die "working tree has $DIRTY uncommitted change(s); commit before publishing"
fi

echo
echo "== 1/2  Windows =="
bash "$REPO/tools/install-toolkit.sh" ${SKIP:+$SKIP} 2>&1 | sed 's/^/  /' \
    || die "windows build/install failed"
[ -f "$TOOLKIT/now.exe" ] || die "no now.exe in $TOOLKIT"
WIN_VER="$("$TOOLKIT/now.exe" version 2>&1)"

echo
echo "== 2/2  Linux (built inside WSL) =="
if ! command -v wsl.exe >/dev/null 2>&1; then
    say "wsl.exe not found — skipping the Linux binary"
    say "the Windows binary above is published; re-run where WSL exists"
    LIN_VER="(not built: no WSL on this host)"
else
    # NOW_WSL_NAME keeps it from colliding with now.exe; NOW_WSL_NO_PATH
    # stops the peer-install script from putting a publication directory
    # on anyone's PATH or editing their dotfiles.
    MSYS_NO_PATHCONV=1 wsl.exe -- \
        env NOW_WSL_PREFIX=/mnt/c/Users/Iridium/Projects/toolkit \
            NOW_WSL_NAME=now-linux-x64 \
            NOW_WSL_NO_PATH=1 \
        bash /mnt/c/Users/Iridium/Projects/Infra/now/tools/install-wsl.sh \
        2>&1 | sed 's/^/  /' || die "linux build/install failed"
    [ -f "$TOOLKIT/now-linux-x64" ] || die "no now-linux-x64 in $TOOLKIT"
    LIN_VER="$(MSYS_NO_PATHCONV=1 wsl.exe -- \
        /mnt/c/Users/Iridium/Projects/toolkit/now-linux-x64 version 2>&1)"
fi

echo
echo "== record =="
STAMP="$TOOLKIT/now-RELEASE.md"
{
    printf '# now — published binaries\n\n'
    printf 'Written by `now/tools/publish-release.sh`. Do not edit by hand;\n'
    printf 're-run the script.\n\n'
    printf '| | file | version | built |\n'
    printf '|---|---|---|---|\n'
    printf '| Windows x64 | `now.exe` | %s | %s |\n' \
           "$WIN_VER" "$(date -u '+%Y-%m-%d %H:%M UTC')"
    printf '| Linux x64 (static) | `now-linux-x64` | %s | %s |\n\n' \
           "$LIN_VER" "$(date -u '+%Y-%m-%d %H:%M UTC')"
    printf 'Source revision: `%s`\n\n' "$REV"
    printf -- '- `now.exe` is on the user PATH, so every Windows project on this\n'
    printf -- '  machine picks it up with no action. If you depend on that, the\n'
    printf -- '  version above is what you now have.\n'
    printf -- '- `now-linux-x64` is static and depends on nothing. Copy it where\n'
    printf -- '  you want it; nothing here installs into your environment.\n'
    printf -- '- A Windows `now.exe` cannot drive a toolchain that only exists on\n'
    printf -- '  the Linux side. If you cross-compile under WSL, the Linux binary\n'
    printf -- '  is the one you need.\n'
    printf -- '- Announcements go to `~/.claude/mailbox/topic-infra-releases.md`.\n'
} > "$STAMP"
say "wrote $STAMP"

echo
say "windows: $WIN_VER"
say "linux:   $LIN_VER"
echo
echo "Published. Announce it on topic-infra-releases.md — a binary nobody"
echo "knows about is the same as one that was never built."
