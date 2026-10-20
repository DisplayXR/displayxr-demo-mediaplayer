#!/bin/bash
# Fail if any Mach-O under <dir> can resolve a library outside the bundle.
#
# Usage: ./check_bundle_paths.sh <dir-or-.app> [<dir-or-.app> ...]
#
# A shipped Mach-O must load ONLY what the .app carries. Three ways it can escape,
# each of which loads on the build box and misbehaves everywhere else:
#   1. an LC_RPATH that is not bundle-relative (@loader_path / @executable_path).
#      v1.14.2 shipped the binary with /opt/homebrew/lib and /tmp/openxr-install/lib
#      searched BEFORE @loader_path/lib, so on a Mac with Homebrew SDL3 / FFmpeg /
#      vulkan-loader the app silently loaded those instead of the bundled copies.
#   2. a dependency (or install id) under a build-box prefix (Homebrew, /usr/local,
#      /tmp, a home directory). Same rule as displayxr-runtime's
#      installer/macos/build_installer.sh "payload Mach-Os depend on build-box paths".
#   3. an invalid code signature — every install_name_tool edit invalidates it, and
#      Apple Silicon SIGKILLs the process at load.
# Also: every @rpath/<lib> dependency must name a file that is in the bundle.
#
# Read-only. Used by create_app_bundle.sh (on the fresh .app) and by CI (on the
# expanded .pkg payload).
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 <dir-or-.app> [...]" >&2
    exit 2
fi

BAD_PREFIX_RE='^(/opt/homebrew|/usr/local|/tmp|/private/tmp|/private/var/folders|/var/folders|/Users)/'
FAIL=""
COUNT=0

for ROOT in "$@"; do
    if [ ! -d "$ROOT" ]; then
        echo "Error: $ROOT is not a directory" >&2
        exit 2
    fi
    while IFS= read -r -d '' f; do
        file -b "$f" | grep -q 'Mach-O' || continue
        COUNT=$((COUNT + 1))
        rel="${f#"$ROOT"/}"

        # 1. rpaths
        while IFS= read -r rp; do
            [ -z "$rp" ] && continue
            case "$rp" in
                @loader_path|@loader_path/*|@executable_path|@executable_path/*) ;;
                *) FAIL="$FAIL
  $rel: LC_RPATH $rp" ;;
            esac
        done < <(otool -l "$f" | awk '/cmd LC_RPATH/{r=1} r && $1=="path"{print $2; r=0}' | sort -u)

        # 2. dependencies + install id (otool -L lists the id first for a dylib)
        while IFS= read -r dep; do
            [ -z "$dep" ] && continue
            if echo "$dep" | grep -qE "$BAD_PREFIX_RE"; then
                FAIL="$FAIL
  $rel: depends on $dep"
            fi
            case "$dep" in
                @rpath/*)
                    base="${dep#@rpath/}"
                    if [ -z "$(find "$ROOT" -name "$base" -print -quit)" ]; then
                        FAIL="$FAIL
  $rel: $dep is not in the bundle"
                    fi ;;
            esac
        done < <(otool -L "$f" | tail -n +2 | awk '{print $1}' | sort -u)

        # 3. signature
        if ! codesign --verify "$f" 2>/dev/null; then
            FAIL="$FAIL
  $rel: code signature invalid or missing"
        fi
    done < <(find "$ROOT" -type f -print0)
done

if [ "$COUNT" -eq 0 ]; then
    echo "Error: no Mach-O files found under $*" >&2
    exit 1
fi
if [ -n "$FAIL" ]; then
    echo "Error: payload Mach-Os depend on build-box paths (or are unsigned):$FAIL" >&2
    exit 1
fi
echo "PASS: $COUNT Mach-O(s) load only bundle-relative paths, all signatures valid."
