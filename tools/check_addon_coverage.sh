#!/bin/sh
# check_addon_coverage.sh - hold three hand-maintained tables to a fourth that
# cannot lie: the directory listing.
#
# Every add-on source in addons/ must be (a) built by addons/CMakeLists.txt, so it is
# installable, (b) in tools/amalgamate.sh's table, so it is obtainable without a
# clone, and (c) documented in addons/README.md, so anyone knows it exists.
#
# This exists because three of the seven add-ons - pool, xport and orch - had drifted
# out of addons/README.md entirely and nothing noticed. A table maintained by hand
# beside a directory that grows is a table that goes stale; this is the check that
# makes staleness fail instead of accumulate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rc=0

for f in "$ROOT"/addons/gptps_*.c; do
    b=$(basename "$f")
    n=${b%.c}

    # gptps_*_plugin.c files are dlopen'd BINARY plug-ins, not linkable modules:
    # they are built as CMake MODULE targets, ship as .so rather than as a library,
    # and are deliberately absent from the amalgamation (you load them, not compile
    # them in). Only the module tier is checked here.
    case "$b" in *_plugin.c) continue ;; esac

    grep -q "SOURCE ${b}" "$ROOT/addons/CMakeLists.txt" \
        || { echo "ERROR: $b is not built by addons/CMakeLists.txt (so it cannot be installed)"; rc=1; }
    grep -q "|${b}|" "$ROOT/tools/amalgamate.sh" \
        || { echo "ERROR: $b is missing from tools/amalgamate.sh (so it cannot be obtained without a clone)"; rc=1; }
    grep -q "$n" "$ROOT/addons/README.md" \
        || { echo "ERROR: $n is undocumented in addons/README.md"; rc=1; }
done

if [ "$rc" = 0 ]; then
    echo "OK: every add-on is built, amalgamated and documented"
fi
exit $rc
