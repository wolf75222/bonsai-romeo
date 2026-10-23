#!/usr/bin/env bash
# Builds bonsai-romeo and installs it in ~/.local/bin (or $PREFIX/bin).
#
#   git clone https://github.com/wolf75222/bonsai-romeo.git ~/bonsai-romeo && ~/bonsai-romeo/install.sh
#
# Run it again after a `git pull` to update. Works for any ROMEO user, on a login node.

set -euo pipefail

SRC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-$SRC_DIR/build}
PREFIX=${PREFIX:-$HOME/.local}

# cmake isn't installed system-wide on ROMEO: take the one from Spack (x64cpu, like the login nodes)
if ! command -v cmake >/dev/null 2>&1; then
    type romeo_load_x64cpu_env >/dev/null 2>&1 || source /etc/profile.d/spack_bash_functions.sh 2>/dev/null || true

    if type romeo_load_x64cpu_env >/dev/null 2>&1; then
        romeo_load_x64cpu_env >/dev/null 2>&1 || true
        CMAKE_PREFIX=$(spack find --format '{prefix}' cmake 2>/dev/null | tail -n 1 || true)
        [[ -n $CMAKE_PREFIX ]] && export PATH="$CMAKE_PREFIX/bin:$PATH"
    fi
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found (on ROMEO: romeo_load_x64cpu_env && spack load cmake)" >&2
    exit 1
fi

echo "==> Building with $(cmake --version | head -n 1) and $(${CXX:-g++} --version | head -n 1)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || echo 4)"

mkdir -p "$PREFIX/bin"
install -m 755 "$BUILD_DIR/bonsai-romeo" "$PREFIX/bin/bonsai-romeo"
echo "==> Installed $PREFIX/bin/bonsai-romeo"

case ":$PATH:" in
    *":$PREFIX/bin:"*) echo "==> Run it with: bonsai-romeo   (or bonsai-romeo --quota)" ;;
    *) echo "==> $PREFIX/bin isn't in your PATH. Add this line to ~/.bashrc, then open a new shell:"
       echo "    export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
