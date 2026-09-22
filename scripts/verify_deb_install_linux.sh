#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Install-verify the mediaplayer .deb on a CLEAN Ubuntu (#76). Run it as root
# inside a pristine ubuntu:<release> container — CI does this for 22.04, 24.04
# and 26.04; locally:
#
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:22.04 \
#     ./scripts/verify_deb_install_linux.sh dist/displayxr-mediaplayer_*_amd64.deb \
#                                           /path/to/displayxr-runtime_*_amd64.deb
#
# Fails if:
#   * apt cannot resolve the package's Depends from that release's archive
#     (installed with --no-install-recommends: Depends alone must be enough);
#   * `ldd -r` on the installed binary or any bundled lib reports a missing
#     library, symbol or symbol version (e.g. a glibc floor above the release);
#   * a system libSDL3 / libav* is linked instead of the static / bundled copy;
#   * a Recommends / Suggests name does not exist on that release.
# Does NOT run the app: CI has no display or GPU (on-screen checks are manual).
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: $0 <mediaplayer.deb> [displayxr-runtime.deb]" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "error: run as root in a throwaway container." >&2; exit 2; }

DEBS=()
for d in "$@"; do DEBS+=("$(readlink -f "$d")"); done
. /etc/os-release
echo "==> $PRETTY_NAME: installing ${DEBS[*]##*/}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# apt resolves each local .deb's Depends from the archive; an unknown package
# name or an unsatisfiable version (libc6 (>= 2.38) on 22.04, ...) fails here.
apt-get install -y -qq --no-install-recommends "${DEBS[@]}"

APPDIR=/usr/lib/displayxr-demos/mediaplayer
BIN="$APPDIR/mediaplayer_handle_vk_linux"
[ -x "$BIN" ] || { echo "error: $BIN not installed." >&2; exit 1; }
dpkg-query -W -f='==> installed ${Package} ${Version}\nDepends: ${Depends}\n' displayxr-mediaplayer

fail=0
for elf in "$BIN" $(find "$APPDIR" -maxdepth 1 -type f -name 'lib*.so*'); do
  out="$(ldd -r "$elf" 2>&1)" || true
  if echo "$out" | grep -E 'not found|undefined symbol|version .* not found'; then
    echo "error: unresolved dependency in $elf" >&2; fail=1
  fi
done
out="$(ldd "$BIN")"
echo "$out" | sed 's/^/    /'
if echo "$out" | grep -E 'libSDL3'; then
  echo "error: binary links a shared libSDL3 (must be static)." >&2; fail=1
fi
if echo "$out" | grep -E 'lib(av|sw)[a-z]+\.so' | grep -v " => $APPDIR/"; then
  echo "error: FFmpeg resolved from outside $APPDIR (must be the bundled copy)." >&2; fail=1
fi

# SDL3 dlopen()s X11 (the window binding needs it) — declared in Depends, so it
# must be present even with --no-install-recommends.
for so in libX11.so.6 libXext.so.6; do
  ldconfig -p | grep -q "$so" || { echo "error: $so missing although declared in Depends." >&2; fail=1; }
done

# Every Recommends / Suggests alternative must name a real package here.
for field in Recommends Suggests; do
  for pkg in $(dpkg-query -W -f="\${$field}" displayxr-mediaplayer | tr ',|' '\n\n' | sed 's/(.*)//; s/ //g' | sed '/^$/d'); do
    if apt-cache show "$pkg" >/dev/null 2>&1; then
      echo "    $field $pkg: available"
    else
      echo "error: $field '$pkg' does not exist on $PRETTY_NAME." >&2; fail=1
    fi
  done
done

[ "$fail" = 0 ] || { echo "==> FAIL on $PRETTY_NAME" >&2; exit 1; }
echo "==> PASS on $PRETTY_NAME"
