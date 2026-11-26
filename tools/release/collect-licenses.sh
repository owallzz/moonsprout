#!/usr/bin/env bash
# Collects the license notices of everything a release package contains into <dest>:
# THIRD-PARTY-NOTICES.txt (an index) plus one file per component.
#   tools/release/collect-licenses.sh <build dir> <dest dir>
# Run from the repository root after the build: the fetched dependencies' own LICENSE files are
# taken from <build dir>/_deps, so the list matches what this platform's build actually used.
# Prebuilt packages without a license file (Dawn, nod, DXC) are covered by packaging/licenses/.
set -euo pipefail
build=$1 dest=$2
mkdir -p "$dest"
index="$dest/THIRD-PARTY-NOTICES.txt"

add() { # $1 component name, $2 license file, $3 one-line description
    cp "$2" "$dest/$1.txt"
    printf '%-28s %s (%s.txt)\n' "$1" "$3" "$1" >> "$index"
}

cat > "$index" <<'EOF'
Moonsprout third-party notices

Moonsprout itself is released under CC0 1.0 (LICENSE.txt). The program includes or is built
from the components below, each under its own license; the full texts are in this folder.

Portions of this software are copyright (C) The FreeType Project (www.freetype.org).
All rights reserved.

libjuice is used under the Mozilla Public License 2.0; its source code is available at
https://github.com/paullouisageneau/libjuice (version 1.7.4).

dxil.dll (Windows) is the DirectX Shader Compiler validator by Microsoft, redistributed with
Dawn under the Microsoft license terms of https://github.com/microsoft/DirectXShaderCompiler.

SQLite (Aurora's pipeline cache) is in the public domain. The launcher's interface framework is
adapted from Dusklight's, released under CC0 1.0.

EOF

add aurora extern/aurora/LICENSE "GameCube SDK runtime over SDL3 and WebGPU"
add borealis extern/borealis/LICENSE "launcher framework, config, disc checks"
add dawn packaging/licenses/dawn.txt "WebGPU implementation"
add nod packaging/licenses/nod.txt "disc image reader"
add directx-shader-compiler packaging/licenses/directx-shader-compiler.txt "shader compiler used by Dawn on Windows"
add musl-libm port/libm/COPYRIGHT "math functions (port/libm)"
# Linux builds with the vendored libcurl link OpenSSL statically (CMakeLists.txt, OPENSSL_USE_STATIC_LIBS).
if grep -q '^OPENSSL_SSL_LIBRARY:FILEPATH=.*\.a$' "$build/CMakeCache.txt" 2>/dev/null; then
    add openssl packaging/licenses/openssl.txt "TLS for the lobby connection (libssl, libcrypto)"
fi
for f in port/launcher/res/fonts/licenses/*; do
    n=$(basename "$f"); n=${n%.txt}
    add "font-$n" "$f" "launcher font"
done

# Dependencies fetched by CMake. Test-only ones are skipped.
for src in "$build"/_deps/*-src; do
    [ -d "$src" ] || continue
    name=$(basename "$src"); name=${name%-src}; name=${name#mppm_}
    case "$name" in googletest | *_prebuilt) continue ;; esac
    lic=$(find "$src" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' \) | sort | head -1)
    if [ -z "$lic" ]; then
        echo "note: no license file in $src" >&2
        continue
    fi
    add "$name" "$lic" "built from source"
    if [ "$name" = freetype ] && [ -f "$src/docs/FTL.TXT" ]; then
        cp "$src/docs/FTL.TXT" "$dest/freetype-FTL.txt"
    fi
done
echo "collected $(ls "$dest" | wc -l) files into $dest"
