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

    # gptps_remote is built and tested but deliberately NOT distributed - shipping the
    # wire codec is what makes its format permanent. See the note in amalgamate.sh.
    case "$b" in
      gptps_remote.c) ;;
      *) grep -q "|${b}|" "$ROOT/tools/amalgamate.sh" \
            || { echo "ERROR: $b is missing from tools/amalgamate.sh (so it cannot be obtained without a clone)"; rc=1; } ;;
    esac

    # Anchored on a real section heading, not a bare substring: "## <name> " only
    # matches a section, so a passing mention in someone else's paragraph - or the
    # name appearing inside a longer one - cannot fake documentation.
    grep -q "^## ${n#gptps_} " "$ROOT/addons/README.md" \
        || { echo "ERROR: $n has no '## ' section in addons/README.md"; rc=1; }
done

# Fourth table: every add-on the amalgamator emits must be listed in release.yml's
# literal files: block, or the release publishes fewer assets than SHA256SUMS names -
# and the job reports success. This check exists because the guard inside release.yml
# compared two values derived from the SAME glob, so it could never fire.
for a in $(sh "$ROOT/tools/amalgamate.sh" --list); do
    for ext in c h; do
        grep -q "dist/gptps_${a}\.${ext}" "$ROOT/.github/workflows/release.yml" \
            || { echo "ERROR: gptps_${a}.${ext} is not in release.yml's files: block (it would not be published)"; rc=1; }
    done
done
grep -q "addons.tar.gz" "$ROOT/.github/workflows/release.yml" \
    && grep -A40 'files: |' "$ROOT/.github/workflows/release.yml" | grep -q "addons.tar.gz" \
    || { echo "ERROR: the add-ons tarball is built but never listed in files:"; rc=1; }

if [ "$rc" = 0 ]; then
    echo "OK: every add-on is built, amalgamated and documented"
fi
exit $rc
