#!/bin/sh
# registry-roundtrip.sh — the whole supply chain against a live cookbook.
#
# keygen -> register the key -> package -> sign -> publish -> procure ->
# verify -> unpack -> compile and link against the fetched dependency,
# plus the negatives that matter: a tampered artifact must be refused,
# --locked must refuse a drifted descriptor without touching the
# network, and the REGISTRY must refuse a signature that does not match
# the artifact it claims to cover.
#
# The unit suite cannot cover this. Every defect it found on 2026-08-20
# was invisible to 352 green tests, because each one lived in the seam
# between two components that the suite exercises separately:
#
#   - the signature was fetched under a name the publisher never writes
#   - the checksum sidecar was written and never uploaded
#   - the packaged archive contained no binary (.lib vs lib*.a on MinGW)
#   - procure never unpacked what it downloaded
#   - a retry after a rejected artifact reported success
#   - --locked fired after every download instead of before
#
# and on 2026-08-20 evening, one more of the same shape: the registry
# stored any bytes at all at a .sig path without looking at them, so the
# client-side gate above was the only thing checking anything.
#
# Usage:  tools/registry-roundtrip.sh [artifact-name]
#
# Needs a cookbook on 127.0.0.1:8080 (NOT localhost) with a 'publisher'
# credential for dev.iridium. Set NOW_BIN to test a specific binary.
# Everything else lives in a sandbox HOME; the real ~/.now is untouched.
# Full supply-chain round trip against a live cookbook, from scratch.
set -u
# Sandbox root. Override with ROUNDTRIP_DIR.
SB="${ROUNDTRIP_DIR:-${TMPDIR:-/tmp}/now-roundtrip}"
# Where the registry keeps its objects, so the tamper step can reach one.
# Override with COOKBOOK_HOME to match the server under test.
CB="${COOKBOOK_HOME:-$HOME/.cookbook}"
export PATH="$(ls -d '/c/Program Files/JetBrains/CLion '*/bin/mingw/bin | tail -1):$PATH"
export HOME="$SB/home"
export USERPROFILE="$SB/home"
export NOW_SIGNING_KEY="$SB/home/signing.key"
NOW="${NOW_BIN:-/c/Users/Iridium/Projects/Infra/now/target/bin/now.exe}"
REG=http://127.0.0.1:8080
GRP=dev.iridium
ART="${1:-widget}"
VER=1.0.0

rm -rf "$SB"; mkdir -p "$SB/home/.now" "$SB/producer" "$SB/consumer"

cat > "$SB/home/.now/credentials.pasta" <<'EOF'
{
  registries: [
    {
      url:      "http://127.0.0.1:8080",
      username: "publisher",
      token:    "changeme",
      method:   "token"
    }
  ]
}
EOF

step() { printf '\n== %s\n' "$1"; }

FAILURES=0
# check <what> <expected> <actual>. The steps below print plenty for a
# human to read; this exists so a regression announces itself instead of
# hiding in a sentence halfway down the output. That is not theoretical:
# registering a key made step 5 start failing and the run still looked
# like a wall of ordinary text.
check() {
    if [ "$2" = "$3" ]; then
        printf '   ok   %s (%s)\n' "$1" "$3"
    else
        printf '   FAIL %s: expected %s, got %s\n' "$1" "$2" "$3"
        FAILURES=$((FAILURES + 1))
    fi
}

step "1. producer project"
cd "$SB/producer" || exit 1
"$NOW" init c --group "$GRP" --artifact "$ART" >/dev/null 2>&1
rm -f src/test/c/*.c
cat > now.pasta <<EOF
{
  group:    "$GRP",
  artifact: "$ART",
  version:  "$VER",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "static", name: "$ART" },
  compile:  { warnings: ["Wall", "Wextra"] },
  deps:     []
}
EOF
cat > src/main/h/$ART.h <<'EOF'
#ifndef DEP_H
#define DEP_H
int widget_answer(void);
#endif
EOF
cat > src/main/c/main.c <<EOF
#include "$ART.h"

int widget_answer(void)
{
    return 42;
}
EOF

step "2. keygen"
PUBKEY=$("$NOW" keygen 2>&1 | sed -n 's/^public key: //p')
echo "   public key: $PUBKEY"
[ -n "$PUBKEY" ] || { echo "FAIL: no public key"; exit 1; }

step "3. auth, register the publisher key, publish"
"$NOW" auth:login --registry "$REG" --method token >/dev/null 2>&1 || { echo "FAIL: login"; exit 1; }
# Registering comes BEFORE publishing, and the order is not cosmetic.
# Once ANY key is registered for a group the registry checks every .sig
# for it, so a publisher whose key it has never seen is refused from
# that moment on. Publishing first worked only for as long as no key had
# ever been registered — which is exactly how this step passed twice and
# then started 404ing on the signature.
"$NOW" keys:register --registry "$REG" --group "$GRP" 2>&1 | sed 's/^/   /'
"$NOW" publish --repo "$REG" -v 2>&1 | sed 's/^/   /' 

step "4. consumer with require_signatures"
cd "$SB/consumer" || exit 1
"$NOW" init c --group "$GRP" --artifact app >/dev/null 2>&1
rm -f src/test/c/*.c
cat > now.pasta <<EOF
{
  group:    "$GRP",
  artifact: "app",
  version:  "1.0.0",
  langs:    ["c"],
  std:      "c11",
  output:   { type: "executable", name: "app" },
  repos:    [ { url: "$REG" } ],
  trust:    { require_signatures: true },
  deps:     [ { group: "$GRP", artifact: "$ART", version: "$VER" } ]
}
EOF
"$NOW" trust:add "$GRP" "$PUBKEY" "widget publisher" >/dev/null 2>&1

step "5. procure (expect: verified)"
"$NOW" procure -v 2>&1 | sed 's/^/   /'
echo "   exit=$?"
echo "   local repo:"
find "$SB/home/.now/repo" -type f -printf '     %8s  %f\n' 2>/dev/null

step "6. lock URL resolves?"
LOCKURL=$(sed -n 's/.*url: "\([^"]*\)".*/\1/p' now.lock.pasta 2>/dev/null | head -1)
JWT=$(sed -n 's/.*jwt: "\([^"]*\)".*/\1/p' "$SB/home/.now/tokens.pasta" | head -1)
echo "   $LOCKURL"
printf '   HTTP '; curl -s -m 8 -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $JWT" "$LOCKURL"

step "7. tamper the stored archive, re-procure (expect: refused)"
OBJ=$(find "$CB/data/objects/central/dev/iridium/$ART/$VER" -name "*.basta" | head -1)
cp "$OBJ" "$SB/archive.orig"
printf 'XXXX' >> "$OBJ"
rm -rf "$SB/home/.now/repo"
"$NOW" procure 2>&1 | sed 's/^/   /'
"$NOW" procure >/dev/null 2>&1; echo "   exit=$?"

step "8. restore, re-procure (expect: verified again)"
cp "$SB/archive.orig" "$OBJ"
rm -rf "$SB/home/.now/repo"
"$NOW" procure 2>&1 | sed 's/^/   /'
"$NOW" procure >/dev/null 2>&1; echo "   exit=$?"

step "9. --locked with a drifted version (expect: refused)"
cp now.pasta now.pasta.bak
sed -i "s/artifact: \"$ART\", version: \"$VER\"/artifact: \"$ART\", version: \"9.9.9\"/" now.pasta
"$NOW" procure --locked 2>&1 | sed 's/^/   /'
"$NOW" procure --locked >/dev/null 2>&1; echo "   exit=$?"
cp now.pasta.bak now.pasta

step "10. build against the procured dependency"
rm -rf "$SB/home/.now/repo"
cat > src/main/c/main.c <<EOF
#include <stdio.h>
#include "$ART.h"

int main(void)
{
    printf("%d\n", widget_answer());
    return 0;
}
EOF
"$NOW" build -j 32 2>&1 | tail -4 | sed 's/^/   /'

step "11. registry refuses a signature that does not match"
cd "$SB/producer" || exit 1
JWT=$(sed -n 's/.*jwt: "\([^"]*\)".*/\1/p' "$SB/home/.now/tokens.pasta" | head -1)

# The .sig has to name a stem the registry actually holds, or this
# measures "cannot check" rather than "will not verify".
printf 'not really an archive' > "$SB/fake.basta"
check "archive accepted" 201 "$(curl -s -m 8 -o /dev/null -w '%{http_code}' -X PUT \
  -H "Authorization: Bearer $JWT" --data-binary @"$SB/fake.basta" \
  "$REG/artifact/dev/iridium/$ART/9.9.0/$ART-9.9.0-noarch.basta")"

head -c 64 /dev/urandom > "$SB/fake.sig"
check "forged signature refused" 400 "$(curl -s -m 8 -o /dev/null -w '%{http_code}' -X PUT \
  -H "Authorization: Bearer $JWT" --data-binary @"$SB/fake.sig" \
  "$REG/artifact/dev/iridium/$ART/9.9.0/$ART-9.9.0-noarch.sig")"
check "and not stored" 404 "$(curl -s -m 8 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $JWT" \
  "$REG/artifact/dev/iridium/$ART/9.9.0/$ART-9.9.0-noarch.sig")"

step "12. a real signature still publishes with the key registered"
# This is the positive half of step 12 and the stronger measurement of
# the two: the key is registered, so the registry now checks every .sig
# for this group. A 400 here would mean the signature `now` produced did
# not verify under cookbook's verifier — two implementations that share
# no code disagreeing about the same 64 bytes. Success means they agree.
cd "$SB/producer" || exit 1
sed -i "s/version:  \"$VER\"/version:  \"1.0.1\"/" now.pasta
"$NOW" publish --repo "$REG" 2>&1 | sed 's/^/   /'
# Ask for it under the name `now package` actually wrote, rather than
# guessing the triple — guessing it is how require_signatures came to be
# unsatisfiable in the first place.
SIGFILE=$(ls "$SB/producer/target/pkg" 2>/dev/null | grep '1\.0\.1.*\.sig$' | head -1)
check "genuine signature stored" 200 "$(curl -s -m 8 -o /dev/null -w '%{http_code}' \
  -H "Authorization: Bearer $JWT" \
  "$REG/artifact/dev/iridium/$ART/1.0.1/$SIGFILE")"

printf '\n== summary\n'
if [ "$FAILURES" -eq 0 ]; then
    echo "   all checked steps passed"
else
    echo "   $FAILURES checked step(s) FAILED"
fi
exit "$FAILURES"
