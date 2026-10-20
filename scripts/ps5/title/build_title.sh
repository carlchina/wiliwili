#!/usr/bin/env bash
# wiliwili - PS5 native title: turn the compiled objects into an installable
# homebrew folder (a real title, not a payload).
#
# The console only starts a title from a folder that holds a signed eboot.bin
# and a loader companion under sce_module/, and a title is linked as one static
# image: nothing may be loaded from a plain .so at run time. This script links
# the objects CMake already produced against the static OpenGL SDK, converts the
# result into the console's module format and assembles the folder.
#
# Required environment:
#   PS5_PAYLOAD_SDK       ps5-payload-sdk (prospero-lld, target/lib)
#   PS5_OPENGL_PREFIX     ps5-opengl SDK prefix (manifest.sha256, lib/libPS5OpenGL.a)
#   PS5_NATIVE_TOOL       ps5-native-tool built by scripts/ps5/title/build_native_tool.sh
#   PS5_BOILERPLATE       ps5-native-app-boilerplate checkout (tooling/native, runtime)
#   PS5_SDL2_PREFIX       SDL2 prefix built by scripts/ps5/title/build_sdl2.sh
# Optional:
#   PS5_TITLE_BUILD_DIR   CMake build directory (default cmake-build-ps5-title)
#   PS5_TITLE_OUT_DIR     output folder (default dist/<title id>)
#   PS5_TITLE_ID          title id (default PPSA99099)
#   PS5_CLANG             clang-18 for the target (default: clang-18 from PATH)
#   LLVM_CONFIG           llvm-config of the same LLVM (required by the SDK wrappers)
#   BUILD_JOBS            parallel compiles (default: all CPUs)

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
title_dir="$root/scripts/ps5/title"

: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the ps5-payload-sdk directory}"
: "${PS5_OPENGL_PREFIX:?set PS5_OPENGL_PREFIX to the ps5-opengl SDK prefix}"
: "${PS5_NATIVE_TOOL:?set PS5_NATIVE_TOOL to the ps5-native-tool binary}"
: "${PS5_BOILERPLATE:?set PS5_BOILERPLATE to a ps5-native-app-boilerplate checkout}"
: "${PS5_SDL2_PREFIX:?set PS5_SDL2_PREFIX to the SDL2 prefix built by scripts/ps5/title/build_sdl2.sh}"

if [[ -z ${LLVM_CONFIG:-} ]]; then
    # the SDK's prospero-lld/prospero-ar wrappers resolve their LLVM through it
    for candidate in /opt/homebrew/opt/llvm@18/bin/llvm-config /usr/lib/llvm-18/bin/llvm-config; do
        if [[ -x $candidate ]]; then
            LLVM_CONFIG=$candidate
            break
        fi
    done
fi
[[ -n ${LLVM_CONFIG:-} && -x $LLVM_CONFIG ]] || {
    echo "LLVM_CONFIG must point at the llvm-config of LLVM 18" >&2
    exit 2
}
export LLVM_CONFIG

if [[ -z ${PS5_CLANG:-} ]]; then
    PS5_CLANG=$(command -v clang-18 || true)
fi
[[ -n ${PS5_CLANG:-} && -x $PS5_CLANG ]] || {
    echo "clang-18 was not found; set PS5_CLANG" >&2
    exit 2
}
export PS5_CLANG

build_dir=${PS5_TITLE_BUILD_DIR:-$root/cmake-build-ps5-title}
title_id=${PS5_TITLE_ID:-PPSA99099}
out_dir=${PS5_TITLE_OUT_DIR:-$root/dist/$title_id}
work_dir=$(cd -- "$build_dir" && pwd)/title
link_txt=$build_dir/CMakeFiles/wiliwili.dir/link.txt
compiler="$PS5_BOILERPLATE/tooling/prospero-clang18"
jobs=${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)}
fself_magic=0x1D3D154F
# Loader/container constants: deliberately separate from the public application
# version in sce_sys/param.json.
module_sdk=0x02000009
companion_sdk=0x08050001

[[ -x $compiler ]] || {
    echo "compiler wrapper not found: $compiler" >&2
    exit 2
}
[[ -f $link_txt ]] || {
    echo "'$link_txt' is missing; configure and build the target first" >&2
    exit 2
}
[[ -x $PS5_NATIVE_TOOL ]] || {
    echo "ps5-native-tool not found: $PS5_NATIVE_TOOL" >&2
    exit 2
}
[[ -s $PS5_SDL2_PREFIX/lib/libSDL2.a ]] || {
    echo "SDL2 archive not found in '$PS5_SDL2_PREFIX/lib'" >&2
    exit 2
}
[[ -s $PS5_PAYLOAD_SDK/bin/prospero-lld ]] || {
    echo "'$PS5_PAYLOAD_SDK' is not a ps5-payload-sdk" >&2
    exit 2
}

# The OpenGL SDK is the only GL implementation a title can link: the console has
# no libGL.so of its own for an application to load.
(
    cd "$PS5_OPENGL_PREFIX"
    sha256sum --check --strict --quiet manifest.sha256
) || {
    echo "ps5-opengl SDK manifest verification failed: $PS5_OPENGL_PREFIX" >&2
    exit 2
}
for required in lib/libPS5OpenGL.a lib/libSceAgc.so lib/libSceAgcDriver.so; do
    [[ -s $PS5_OPENGL_PREFIX/$required ]] || {
        echo "ps5-opengl SDK is missing $required" >&2
        exit 2
    }
done

mkdir -p "$work_dir" "$work_dir/obj"

echo "==> [title] Compiling the native startup, heap and shims"
compile() {
    local standard=$1 source=$2 output=$3
    "$compiler" -std="$standard" -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
        -c "$source" -o "$output"
}
compile c++20 "$PS5_BOILERPLATE/tooling/native/app_crt.cpp" "$work_dir/obj/app_crt.o"
compile c++20 "$PS5_BOILERPLATE/tooling/native/app_cpp_runtime.cpp" \
    "$work_dir/obj/app_cpp_runtime.o"
# Every allocation of the executable goes through this heap; the linker routes
# the whole malloc family into it (see --wrap below).
compile c11 "$title_dir/app_heap.c" "$work_dir/obj/app_heap.o"
compile c11 "$title_dir/runtime_shims.c" "$work_dir/obj/runtime_shims.o"
# Linked right after the log shim so its constructor installs the handlers once
# stdout is the log file.
compile c++20 "$title_dir/crash_report.cpp" "$work_dir/obj/crash_report.o"
compile c11 "$title_dir/locale_shims.c" "$work_dir/obj/locale_shims.o"
compile c11 "$title_dir/libc_shims.c" "$work_dir/obj/libc_shims.o"
# The statically linked OpenGL implementation keeps its context in emulated TLS,
# which the console does not implement.
compile c11 "$title_dir/emutls.c" "$work_dir/obj/emutls.o"

# The pinned libc++ archives record a POSIX pthread dependency. The clean-room
# runtime supplies those imports, so a valid local archive is enough for lld to
# resolve the dependency without dragging in a second runtime.
"$PS5_PAYLOAD_SDK/bin/prospero-ar" rcs "$work_dir/libpthread.a" \
    "$work_dir/obj/app_cpp_runtime.o"

# The title cannot load libScePosixForWebKit, so everything that resolves
# through it is supplied statically instead. A private copy of the SDK's own
# libc archive fills the holes. Members that would define a symbol the shims
# above already own get that symbol weakened, so the shim keeps owning it
# while every other definition in the member still satisfies references.
app_libc="$work_dir/libc-app.a"
cp -- "$PS5_PAYLOAD_SDK/target/lib/libc.a" "$app_libc"
shim_objects="$work_dir/obj/runtime_shims.o $work_dir/obj/libc_shims.o \
$work_dir/obj/locale_shims.o"
/opt/homebrew/opt/llvm@18/bin/llvm-nm --defined-only $shim_objects 2>/dev/null |
    sed 's/.* //' | sort -u > "$work_dir/libc-patch-shims.txt"
/opt/homebrew/opt/llvm@18/bin/llvm-nm --defined-only \
    "$PS5_PAYLOAD_SDK/target/lib/libc.a" 2>/dev/null |
    sed 's/.* //' | sort -u > "$work_dir/libc-patch-all.txt"
weaken_member() {
    local member=$1
    local archive=$2 object flags symbol
    object="$work_dir/libc-patch/$member"
    mkdir -p "$(dirname -- "$object")"
    (cd "$work_dir" &&
        "$PS5_PAYLOAD_SDK/bin/prospero-ar" x "$archive" "$member" &&
        mv -f "$member" "$object") >/dev/null 2>&1 || return 0
    flags=()
    while IFS= read -r symbol; do
        [[ $symbol =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        grep -qxF "$symbol" "$work_dir/libc-patch-shims.txt" &&
            grep -qxF "$symbol" "$work_dir/libc-patch-all.txt" &&
            flags+=(--weaken-symbol="$symbol")
    done < "$work_dir/libc-patch-all.txt"
    flags=($(printf '%s\n' "${flags[@]}" | sort -u))
    if ((${#flags[@]} > 0)); then
        /opt/homebrew/opt/llvm@18/bin/llvm-objcopy "${flags[@]}" "$object"
        "$PS5_PAYLOAD_SDK/bin/prospero-ar" d "$archive" "$member"
        (cd "$work_dir/libc-patch" &&
            "$PS5_PAYLOAD_SDK/bin/prospero-ar" r "$archive" "$member")
    fi
}
mkdir -p "$work_dir/libc-patch"
rm -rf -- "$work_dir"/libc-patch/*
for member in syscalls.o gmtime.o dlfcn.o no-locale.o syslog.o; do
    weaken_member "$member" "$app_libc"
done
# mman.o maps its syscalls through internal "kernel_*" names that the public
# SDK stubs do not export; deleting it keeps mprotect/mmap as plain imports,
# which resolved against the real modules in every console run so far.
"$PS5_PAYLOAD_SDK/bin/prospero-ar" d "$app_libc" mman.o 2>/dev/null || true
# dlfcn.o drags the internal "__dlopen" helper in; the dynamic loader family
# stays a plain (unused) import instead, as before libc was linked.
"$PS5_PAYLOAD_SDK/bin/prospero-ar" d "$app_libc" dlfcn.o 2>/dev/null || true

# The OpenGL SDK's import libraries are handed to the converter as well, so the
# driver's imports resolve to the console's own modules.
group="$work_dir/libps5opengl-group.a"
{
    printf '/* Generated by scripts/ps5/title/build_title.sh; do not edit. */\n'
    printf 'SEARCH_DIR("%s")\n' "$PS5_OPENGL_PREFIX/lib" "$PS5_PAYLOAD_SDK/target/lib"
    # The runtime entry is only reachable through the driver's dispatch table.
    printf 'EXTERN(ps5_agc_gate2_run)\n'
    printf 'GROUP (\n'
    printf '  "%s"\n' "$PS5_OPENGL_PREFIX/lib/libPS5OpenGL.a" \
        "$PS5_PAYLOAD_SDK/target/lib/libunwind.a" \
        "$PS5_PAYLOAD_SDK/target/lib/libc++abi.a" \
        "$PS5_PAYLOAD_SDK/target/lib/libc++.a"
    printf ')\n'
} > "$group"

# The CMake link line describes the payload: everything from the sysroot is
# dynamic there. A title replaces the two libraries the console cannot provide
# (OpenGL and SDL2) with their static builds and imports the platform modules
# from the SDK stubs instead of named libraries. The file is only read, CMake
# keeps owning it.
echo "==> [title] Reading the CMake link line of $link_txt"
objects=()
archives=()
read -r -a tokens < "$link_txt"
index=0
while (( index < ${#tokens[@]} )); do
    token=${tokens[index]}
    # CMake records the compiler driver in front of the link line; passing it to
    # the linker would have it read a shell script as a linker script.
    if (( index == 0 )); then
        ((index += 1))
        continue
    fi
    case $token in
        -o)
            ((index += 1))
            ;;
        -pthread | -lGL | -lSce*)
            : # provided by the stubs and the OpenGL group below
            ;;
        -lSDL2main)
            : # a title supplies its own entry point
            ;;
        -lsamplerate)
            : # the static SDL2 build resamples in its own code
            ;;
        -lSDL2)
            archives+=("$PS5_SDL2_PREFIX/lib/libSDL2.a")
            ;;
        -Wl,-rpath,*)
            : # a title has no library directory to search
            ;;
        -D* | -O* | -std=* | --sysroot=* | -W* | -f* | -m* | -g*)
            : # compiler driver flags; the objects are already built
            ;;
        *.o)
            objects+=("$token")
            ;;
        *)
            # only real link inputs: options, archive paths (absolute or relative)
            # or the SDL2 override added above
            if [[ $token == -* || $token == /* || $token == *.a || $token == *.so ]]; then
                archives+=("$token")
            fi
            ;;
    esac
    ((index += 1))
done
(( ${#objects[@]} > 0 )) || {
    echo "no object files in '$link_txt'" >&2
    exit 2
}

echo "==> [title] Linking the static image"
cd "$build_dir"
# Two families are wrapped. The allocation family is replaced by app_heap.o, a
# private heap on top of the runtime module. readlink()/symlink()/link() are
# routed to the console's own POSIX layer (libScePosixForWebKit), which is the
# only module that offers them to a title.
"$PS5_PAYLOAD_SDK/bin/prospero-lld" -T "$title_dir/ps5-pie.ld" --eh-frame-hdr \
    --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign \
    --wrap=malloc_usable_size --wrap=fopen \
    --wrap=readlink --wrap=symlink --wrap=link --wrap=fcntl \
    --version-script "$title_dir/app-symbols.map" \
    --exclude-libs=ALL -L "$work_dir" \
    -e _start --error-limit=0 -o "$work_dir/llvm-pie.elf" \
    "$work_dir/obj/app_crt.o" "$work_dir/obj/app_cpp_runtime.o" \
    "$work_dir/obj/app_heap.o" "$work_dir/obj/runtime_shims.o" \
    "$work_dir/obj/crash_report.o" \
    "$work_dir/obj/locale_shims.o" "$work_dir/obj/libc_shims.o" \
    "$work_dir/obj/emutls.o" "${objects[@]}" \
    --start-group "$group" "${archives[@]}" \
    "$app_libc" --end-group \
    --as-needed "$PS5_OPENGL_PREFIX/lib/libSceAgc.so" \
    "$PS5_OPENGL_PREFIX/lib/libSceAgcDriver.so" \
    "$PS5_PAYLOAD_SDK"/target/lib/*.so

echo "==> [title] Converting and signing"
"$PS5_NATIVE_TOOL" link --in "$work_dir/llvm-pie.elf" --out "$work_dir/eboot.elf" \
    --stub-dir "$PS5_PAYLOAD_SDK/target/lib" \
    --stub "$PS5_OPENGL_PREFIX/lib/libSceAgc.so" \
    --stub "$PS5_OPENGL_PREFIX/lib/libSceAgcDriver.so" \
    --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

echo "==> [title] Assembling $out_dir"
rm -rf -- "$out_dir"
mkdir -p "$out_dir/sce_sys" "$out_dir/sce_module"
"$PS5_NATIVE_TOOL" self --sign --in "$work_dir/eboot.elf" --out "$out_dir/eboot.bin" \
    --magic "$fself_magic"
cp "$title_dir/param.json" "$out_dir/sce_sys/param.json"
[[ ! -f "$root/scripts/ps5/sce_sys/icon0.png" ]] ||
    cp "$root/scripts/ps5/sce_sys/icon0.png" "$out_dir/sce_sys/icon0.png"

# The loader-visible companion module: a contract plus the heap API table.
if [[ ! -f "$PS5_BOILERPLATE/runtime/libc.prx" ]]; then
    echo "'$PS5_BOILERPLATE/runtime/libc.prx' is missing" >&2
    exit 2
fi
(
    cd "$PS5_BOILERPLATE/runtime"
    sha256sum --check --strict libc.prx.sha256
)
cp "$PS5_BOILERPLATE/runtime/libc.prx" "$out_dir/sce_module/libc.prx"
"$PS5_NATIVE_TOOL" self --inspect --file "$out_dir/sce_module/libc.prx" > /dev/null

# A title reads its assets from the folder it was installed from.
cp -a "$root/resources" "$out_dir/resources"
# Finder leaves these behind in a source tree, they have no place on a console
find "$out_dir" -name .DS_Store -delete

"$PS5_NATIVE_TOOL" self --inspect --file "$out_dir/eboot.bin"
printf 'Native title: %s\n' "$out_dir"
