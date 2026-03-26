#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  install.sh — Build libshmipc for x86_64 and arm64-v8a and assemble a
#               ready-to-use dist/ tree:
#
#    dist/
#      include/shmipc/
#        shmipc.h          ← public C API
#        ShmConfig.h       ← error codes / macros
#      lib/
#        x86_64/
#          libshmipc.so   (or libshmipc.a with --static)
#        arm64-v8a/
#          libshmipc.so   (or libshmipc.a with --static)
#
#  Usage (from shmipc/ directory, inside WSL):
#    bash install.sh                 # shared libraries (default)
#    bash install.sh --static        # static libraries
#    bash install.sh --skip-x86      # arm64-v8a only
#    bash install.sh --skip-arm64    # x86_64 only
#
#  Environment variables:
#    ANDROID_NDK_HOME  path to Android NDK  (default: ~/android-ndk-r28b)
#    DIST              output directory      (default: ./dist)
#    BUILD_TYPE        Release | Debug       (default: Release)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/android-ndk-r28b}"
DIST="${DIST:-$SCRIPT_DIR/dist}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS=$(nproc 2>/dev/null || echo 4)

BUILD_SHARED=ON   # default: shared library
SKIP_X86=0
SKIP_ARM64=0

for arg in "$@"; do
    case "$arg" in
        --static)     BUILD_SHARED=OFF ;;
        --skip-x86)   SKIP_X86=1       ;;
        --skip-arm64) SKIP_ARM64=1     ;;
    esac
done

LIB_EXT="so" ; [ "$BUILD_SHARED" = "OFF" ] && LIB_EXT="a"

# ── helpers ─────────────────────────────────────────────────────────────────
bold()   { printf '\033[1m%s\033[0m\n' "$*"; }
step()   { bold "▶  $*"; }
ok()     { printf '\033[32m✔  %s\033[0m\n' "$*"; }
human()  { du -sh "$1" 2>/dev/null | cut -f1; }   # human-readable file size

# ── clean dist ──────────────────────────────────────────────────────────────
step "Preparing dist: $DIST  (type=lib${LIB_EXT})"
rm -rf "$DIST"
mkdir -p "$DIST/include/shmipc" \
         "$DIST/lib/x86_64"     \
         "$DIST/lib/arm64-v8a"

COMMON=(
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DSHMIPC_BUILD_SHARED="$BUILD_SHARED"
    -DSHMIPC_BUILD_EXAMPLES=OFF
    -DSHMIPC_BUILD_TESTS=OFF
)

# ── x86_64 ──────────────────────────────────────────────────────────────────
if [ "$SKIP_X86" -eq 0 ]; then
    step "Building x86_64 ($BUILD_TYPE)"
    B="$SCRIPT_DIR/build_x86"
    cmake -S "$SCRIPT_DIR" -B "$B" \
        "${COMMON[@]}" \
        -DCMAKE_INSTALL_PREFIX="$DIST" \
        -DCMAKE_INSTALL_LIBDIR="lib/x86_64"
    cmake --build "$B" -j"$JOBS" --target shmipc
    cmake --install "$B" --component lib

    LIB="$DIST/lib/x86_64/libshmipc.$LIB_EXT"
    # Strip debug symbols from shared library to minimise size
    if [ "$BUILD_SHARED" = "ON" ] && command -v strip &>/dev/null; then
        strip --strip-unneeded "$LIB"
    fi
    ok "x86_64  $(human "$LIB")  →  $LIB"
else
    echo "  (skipped x86_64)"
fi

# ── arm64-v8a ────────────────────────────────────────────────────────────────
if [ "$SKIP_ARM64" -eq 0 ]; then
    step "Building arm64-v8a ($BUILD_TYPE)"
    if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
        echo "ERROR: Android NDK not found at $NDK"
        echo "       Set ANDROID_NDK_HOME to the correct path."
        exit 1
    fi
    STRIP_TOOL="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

    B="$SCRIPT_DIR/build_arm64"
    cmake -S "$SCRIPT_DIR" -B "$B" \
        "${COMMON[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-21 \
        -DCMAKE_INSTALL_PREFIX="$DIST" \
        -DCMAKE_INSTALL_LIBDIR="lib/arm64-v8a"
    cmake --build "$B" -j"$JOBS" --target shmipc

    LIB_UNSTRIPPED="$B/libshmipc.$LIB_EXT"
    SIZE_BEFORE=$(du -sh "$LIB_UNSTRIPPED" 2>/dev/null | cut -f1)

    cmake --install "$B" --component lib

    LIB="$DIST/lib/arm64-v8a/libshmipc.$LIB_EXT"
    if [ "$BUILD_SHARED" = "ON" ] && [ -x "$STRIP_TOOL" ]; then
        "$STRIP_TOOL" --strip-unneeded "$LIB"
        SIZE_AFTER=$(du -sh "$LIB" 2>/dev/null | cut -f1)
        ok "arm64-v8a  before strip: $SIZE_BEFORE  →  after strip: $SIZE_AFTER"
    else
        ok "arm64-v8a  $(human "$LIB")  →  $LIB"
    fi
else
    echo "  (skipped arm64-v8a)"
fi

# ── headers (only need to do once) ──────────────────────────────────────────
step "Installing public headers"
cp "$SCRIPT_DIR/include/shmipc/shmipc.h"    "$DIST/include/shmipc/"
cp "$SCRIPT_DIR/include/shmipc/ShmConfig.h" "$DIST/include/shmipc/"
ok "Headers installed"

# ── summary ──────────────────────────────────────────────────────────────────
echo ""
bold "Install complete.  Tree:"
find "$DIST" -type f | sort | while read -r f; do
    printf "    %-55s  %s\n" "$f" "$(du -sh "$f" | cut -f1)"
done

echo ""
bold "Usage in Android CMakeLists.txt:"
cat <<'EOF'
    add_library(shmipc SHARED IMPORTED)
    set_target_properties(shmipc PROPERTIES
        IMPORTED_LOCATION  "${DIST}/lib/${ANDROID_ABI}/libshmipc.so"
        INTERFACE_INCLUDE_DIRECTORIES "${DIST}/include"
    )
    target_link_libraries(your_target PRIVATE shmipc)
EOF
