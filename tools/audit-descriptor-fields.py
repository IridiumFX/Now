#!/usr/bin/env python3
r"""Re-verify now_pom.c's dead-field lists against the code, both ways.

`k_dead_nested` in now_pom.c drives the "parsed but has no effect"
warning. Its comment says "Verified dead 2026-08-12 - check before
adding", and until 2026-08-24 that check was a person reading greps.
This makes it a command.

WHY IT MATTERS. A descriptor field can be in one of four states, and
only two of them tell the user the truth:

  parsed and read          works                      (silent, correct)
  not parsed at all        "unknown key"              (correct)
  parsed, never read, listed dead
                           "parsed but has no effect" (correct)
  parsed, never read, NOT listed
                           SILENCE                    <- the bug

The last one is why `inherit_defaults` sat inert in a peer's workspace
for weeks, and why `sources.defines`, `sources.env` and `tests.include`
were found on 2026-08-24 - the last of those had just been written into
the spec as working, on the strength of `tests:` and `sources:` sharing
a loader. They share a PARSER. They do not share readers.

The reverse is checked too: a field on the dead list that something now
reads means the list is stale and the tool is warning about a feature
that works.

    python tools/audit-descriptor-fields.py

Exits non-zero if either list disagrees with the code, so it can gate.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NOW = os.path.dirname(HERE)
POM_H = os.path.join(NOW, "src", "main", "h", "internal", "now_pom.h")
POM_C = os.path.join(NOW, "src", "main", "c", "now_pom.c")

# Descriptor block -> (struct that holds it, member path on NowProject).
# Two blocks sharing a struct is the whole point: `sources` and `tests`
# are both NowSources and are read by different code.
BLOCKS = [
    ("sources", "NowSources", "sources."),
    ("tests",   "NowSources", "tests."),
    ("output",  "NowOutput",  "output."),
    ("compile", "NowCompile", "compile."),
    ("link",    "NowLink",    "link."),
]

# Members that are container bookkeeping or derived internally, not
# descriptor fields a user can write.
NOT_FIELDS = {"items", "count", "capacity", "cap", "dir_is_default"}


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def project_sources():
    out = []
    for root, dirs, files in os.walk(NOW):
        low = root.lower()
        if any(s in low for s in (os.sep + "lib" + os.sep, os.sep + "build" + os.sep,
                                  os.sep + "target" + os.sep, os.sep + ".git",
                                  os.sep + "specs")):
            continue
        for f in files:
            if f.endswith((".c", ".h")):
                out.append(os.path.join(root, f))
    return out


def struct_fields(hdr, typename):
    """Members of one typedef. Must not span an intervening typedef -
    a greedy match made NowOutput appear to own NowCompile's fields."""
    m = re.search(r"typedef struct \{((?:(?!typedef struct)[\s\S])*?)\}\s*"
                  + re.escape(typename) + r"\s*;", hdr)
    if not m:
        return []
    fields = []
    for line in m.group(1).splitlines():
        mm = re.match(r"\s{4}(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\**\s*"
                      r"([a-z_][a-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;", line)
        if mm and mm.group(1) not in NOT_FIELDS:
            fields.append(mm.group(1))
    return fields


def enclosing_function(text, pos):
    """Name of the function a byte offset falls in, best effort."""
    head = text[:pos]
    hits = re.findall(r"^(?:static\s+)?[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;]*?\)\s*\{",
                      head, re.M)
    return hits[-1] if hits else ""


# A read inside one of these is NOT a use of the field's meaning: they
# hash the descriptor to decide whether to rebuild. `link.script_body`
# is hashed and never emitted, so editing it forces a relink that
# produces an identical binary - which makes it dead as a feature and
# correctly listed, while looking "read" to a naive grep.
HASH_FUNCS = {"link_flags_hash", "compile_flags_hash"}


def dead_lists(pom):
    """block name -> set of fields declared dead for that block."""
    out = {}
    for m in re.finditer(r"k_dead_(\w+)\[\]\s*=\s*\{(.*?)\};", pom, re.S):
        out[m.group(1)] = set(re.findall(r'"([^"]+)"', m.group(2)))
    return out


def main():
    hdr, pom = read(POM_H), read(POM_C)
    corpus = {}
    for p in project_sources():
        base = os.path.basename(p).lower()
        if base in ("now_pom.c", "now_test.c"):
            continue          # the loader itself, and the suite
        corpus[p] = read(p)

    declared = dead_lists(pom)
    problems = 0

    print("field                          state          list       verdict")
    print("-" * 70)
    for block, typename, prefix in BLOCKS:
        dead = declared.get(block, set())
        for f in struct_fields(hdr, typename):
            pat = re.compile(r"(?:->|\.)" + re.escape(prefix) + re.escape(f) + r"\b")
            reads = hashed = 0
            for t in corpus.values():
                for m in pat.finditer(t):
                    if enclosing_function(t, m.start()) in HASH_FUNCS:
                        hashed += 1
                    else:
                        reads += 1
            listed = f in dead
            name = "%s.%s" % (block, f)
            if reads == 0 and not listed:
                print("  %-28s never read     unlisted   SILENT - add to k_dead_%s"
                      % (name, block))
                problems += 1
            elif reads > 0 and listed:
                print("  %-28s read %-9d listed     STALE - remove from k_dead_%s"
                      % (name, reads, block))
                problems += 1
            elif reads == 0 and hashed > 0 and listed:
                print("  %-28s hashed only    listed     ok, but a relink it "
                      "cannot change" % name)

    print("-" * 70)
    if problems == 0:
        print("k_dead_nested agrees with the code.")
        return 0
    print("%d disagreement(s). A field that is parsed, stored and never read"
          % problems)
    print("must be on its block's dead list, or the user is told nothing.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
