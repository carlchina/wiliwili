#!/usr/bin/env bash
# Cross compile libdav1d for the PS5 payload environment and install it into the
# ps5-payload-sdk sysroot, so that FFmpeg and mpv can decode AV1.
#
# Requirements:
#   - PS5_PAYLOAD_SDK points to the ps5-payload-sdk directory
#   - meson, ninja, nasm and LLVM 18 toolchain
#
# Usage:
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk scripts/ps5/build_dav1d.sh

set -euo pipefail

DAV1D_VERSION=1.5.1

if [[ -z "${PS5_PAYLOAD_SDK:-}" ]]; then
    echo "Please set PS5_PAYLOAD_SDK to the ps5-payload-sdk directory" >&2
    exit 1
fi

if [[ -z "${LLVM_CONFIG:-}" ]]; then
    for candidate in /opt/homebrew/opt/llvm@18/bin/llvm-config /usr/lib/llvm-18/bin/llvm-config; do
        if [[ -x "${candidate}" ]]; then
            LLVM_CONFIG="${candidate}"
            break
        fi
    done
    if [[ -z "${LLVM_CONFIG:-}" ]]; then
        LLVM_CONFIG=$(command -v llvm-config-18 || command -v llvm-config || true)
    fi
fi

if [[ -n "${LLVM_CONFIG:-}" && -x "${LLVM_CONFIG}" ]]; then
    export LLVM_CONFIG
    llvm_bindir="$("${LLVM_CONFIG}" --bindir 2>/dev/null || dirname "${LLVM_CONFIG}")"
    export PATH="${llvm_bindir}:${PATH}"
fi

export PATH="${PS5_PAYLOAD_SDK}/bin:${PATH}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="${WORK_DIR:-${HOME}/.cache/wiliwili-ps5}"

mkdir -p "${WORK_DIR}"
cd "${WORK_DIR}"

if [[ ! -d "dav1d-${DAV1D_VERSION}" ]]; then
    echo "Downloading dav1d ${DAV1D_VERSION}" >&2
    curl --location --fail --retry 3 --retry-delay 2 --output "dav1d-${DAV1D_VERSION}.tar.gz" \
        "https://github.com/videolan/dav1d/archive/refs/tags/${DAV1D_VERSION}.tar.gz"
    tar -xzf "dav1d-${DAV1D_VERSION}.tar.gz"
fi

cd "dav1d-${DAV1D_VERSION}"
rm -rf build-ps5

if [[ -x "${PS5_PAYLOAD_SDK}/bin/prospero-meson" ]]; then
    MESON="${PS5_PAYLOAD_SDK}/bin/prospero-meson setup"
else
    MESON="meson setup --cross-file=${PS5_PAYLOAD_SDK}/toolchain/prospero.ini"
fi

${MESON} build-ps5 \
    --buildtype=release \
    --default-library=static \
    -Denable_tools=false \
    -Denable_tests=false

JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
ninja -C build-ps5 -j"${JOBS}"

TARGET="${PS5_PAYLOAD_SDK}/target/user/homebrew"
DESTDIR="${PS5_PAYLOAD_SDK}/target" ninja -C build-ps5 install

# Ensure dav1d.pc is in lib/pkgconfig as well as libdata/pkgconfig
mkdir -p "${TARGET}/lib/pkgconfig"
if [[ -f "${TARGET}/libdata/pkgconfig/dav1d.pc" ]]; then
    cp -f "${TARGET}/libdata/pkgconfig/dav1d.pc" "${TARGET}/lib/pkgconfig/dav1d.pc"
fi

echo "libdav1d ${DAV1D_VERSION} has been installed into ${TARGET}" >&2

