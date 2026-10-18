# PS5 native title

This directory builds wiliwili as a **native PS5 title**: a folder below
`/data/homebrew/` that a folder-title loader such as ShadowMountPlus picks up and
shows on the home screen like any installed application. It is not a payload and
not a package.

```
scripts/ps5/title/
  build_title.sh        link, convert and sign the module; assemble the folder
  build_sdl2.sh         build the static SDL2 the title links
  build_native_tool.sh  build ps5-native-tool, the module converter
  fetch_deps.sh         fetch and verify every pinned dependency
  deploy.sh             publish the folder to a console over FTP
  runtime_shims.c       startup side shims (log file, umask, terminal stubs)
  libc_shims.c          the libc entry points a title does not export
  locale_shims.c        the locale and rune entry points (vendored, see below)
  emutls.c              emulated TLS support (vendored, see below)
  app_heap.c            the application heap on top of the runtime module (vendored)
  app-symbols.map       exports of the linked image
  ps5-pie.ld            linker script of the image
  param.json            title metadata (id, name, memory)
  patches/              changes applied to the third party sources
```

## Why a title, and why a folder

A payload runs beside the system and can load shared objects the way a normal
program does. A title does not: the loader refuses an image that asks for a plain
`.so` (`CE-100008-9` on the packages that were tried), and a package that is not
signed with retail keys is refused before it starts. The folder form sidesteps
both problems. Everything is linked into one static image, the only dynamic
dependencies are the system modules the console serves to any application, and the
image is wrapped in a container a development loader accepts.

What ends up on the console:

```
/data/homebrew/PPSA99099/
  eboot.bin              the signed module (a folder title has no .self suffix)
  sce_module/libc.prx    the loader companion, a clean-room runtime module
  sce_sys/param.json     title metadata, read when the folder is scanned
  sce_sys/icon0.png      the icon of the home screen entry
  resources/             fonts, layouts, translations, images
```

The application reads its assets from `/app0/resources/`, the mount point of the
folder it was started from. Its configuration and its log stay in
`/data/wiliwili/` (`wiliwili_config.json`, `wiliwili-title.log`).

## Build

The whole pipeline is four commands. `fetch_deps.sh` downloads about 700 MB
(the payload SDK, the OpenGL SDK, two source checkouts and SDL2) into
`~/.local/opt`, the rest builds.

```bash
# 1. pinned dependencies: payload SDK, ps5-opengl v1.0.2 (+ its SDK release),
#    the native app boilerplate, SDL2 at the pinned commit
scripts/ps5/title/fetch_deps.sh

# 2. the module converter, and the static SDL2 (audio enabled)
scripts/ps5/title/build_native_tool.sh
scripts/ps5/title/build_sdl2.sh

# 3. compile the application with the payload toolchain (about 15 minutes),
#    then link, convert and sign it
cmake -B cmake-build-ps5-title -DCMAKE_BUILD_TYPE=Release -DPLATFORM_PS5=ON \
      -DPS5_NATIVE_TITLE=ON -DBRLS_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_BATCH_SIZE=16
cmake --build cmake-build-ps5-title --target wiliwili.title -j"$(sysctl -n hw.ncpu)"

# 4. the folder is in dist/PPSA99099, publish it and reload ShadowMountPlus
scripts/ps5/title/deploy.sh -h <console>
```

`PS5_NATIVE_TITLE=ON` changes what CMake produces in two ways: the resources
path becomes `/app0/resources/`, and the build grows a `wiliwili.title` target
that runs `build_title.sh` on the objects CMake compiled. The objects themselves
are the same ones the payload build produces, which is why the two share the
compiler settings.

The scripts accept the paths of everything they need, so a CI job can keep its
own layout:

| Variable | Default | Meaning |
| --- | --- | --- |
| `PS5_PAYLOAD_SDK` | — | ps5-payload-sdk (cross compiler, `prospero-lld`, system stubs) |
| `PS5_OPENGL_PREFIX` | — | ps5-opengl SDK prefix (static GL, AGC stubs) |
| `PS5_OPENGL_CHECKOUT` | — | ps5-opengl source checkout (`build_sdl2.sh`) |
| `PS5_NATIVE_TOOL` | — | `ps5-native-tool`, the module converter |
| `PS5_BOILERPLATE` | — | ps5-native-app-boilerplate checkout |
| `PS5_SDL2_PREFIX` | — | static SDL2 prefix |
| `PS5_SDL_SOURCE` | — | SDL2 git checkout (`build_sdl2.sh`) |
| `PS5_TITLE_BUILD_DIR` | `cmake-build-ps5-title` | CMake build directory to link |
| `PS5_TITLE_OUT_DIR` | `dist/<title id>` | where the folder is assembled |
| `PS5_TITLE_ID` | `PPSA99099` | title id of the folder |
| `PS5_TITLE_HEAP_SIZE` | `0x10000000` | heap the loader reserves (`build_native_tool.sh`) |
| `PS5_CLANG` | `clang-18` from `PATH` | host clang of LLVM 18 |
| `LLVM_CONFIG` | detected | `llvm-config` of the same LLVM |

The pipeline needs LLVM 18 (`brew install llvm@18`, `apt install llvm-18
clang-18 lld-18`) and a C++20 host compiler for the converter. It runs on macOS
and Linux; nothing needs a console until deployment.

## Deploy

```bash
scripts/ps5/title/deploy.sh -h 192.168.1.50        # or hostname/PS5_HOST
scripts/ps5/title/deploy.sh -h <console> --undeploy
```

The script writes every file to a hidden name beside its destination and renames
it once it arrived, `eboot.bin` and `sce_sys/param.json` last, so an interrupted
upload never leaves a folder that the loader would try to start. It speaks to
`ftpsrv` in both of its common shapes (the write unlock of the payload one, the
anonymous login of the other).

Afterwards the console has to rescan `/data/homebrew`: reload the
ShadowMountPlus payload or restart. The title then appears on the home screen,
and starting it is a normal application launch.

## Reading a failure

The application's own output has no reader, so `runtime_shims.c` redirects stdout
and stderr into `/data/wiliwili/wiliwili-title.log` before `main()` runs. That
file and the kernel log (the loader prints its own errors, for example
`Need_sceKernelSys`) are the two places to look:

```bash
curl --silent ftp://<console>:2121/data/wiliwili/wiliwili-title.log   # application log
nc -w 3 <console> 3232                                                # kernel log (klogsrv)
```

Deleting `/data/homebrew/PPSA99099/` and reloading the loader removes the home
screen entry again.

## What is borrowed, and under which licence

The title needs three things the payload build does not: a static OpenGL
implementation, a module format writer and a runtime module. They come from
other GPL-3.0-or-later projects, which wiliwili's licence is compatible with.
Everything is pinned, and the two files that are compiled into the title are
kept, with their notices, in this directory:

| Source | Used for | Local copy |
| --- | --- | --- |
| [blackbearreloaded/ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) v1.0.2, GPL-3.0-or-later | static OpenGL (`libPS5OpenGL.a` + mesa), the SDL2 integration | `locale_shims.c` (verbatim), `app_heap.c` (verbatim), `patches/ps5-opengl-sdl2-static.patch` |
| [drakmor/ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate) `470695e`, GPL-3.0-or-later | `ps5-native-tool`, `app_crt.cpp`, `app_cpp_runtime.cpp`, `ps5-pie.ld`, `app-symbols.map`, `runtime/libc.prx` | `build_native_tool.sh` applies one heap-size change |
| LLVM compiler-rt, Apache-2.0 WITH LLVM-exception | emulated TLS (`__emutls_get_address`) | `emutls.c` |

`app_crt.cpp`, `app_cpp_runtime.cpp`, `ps5-pie.ld` and `app-symbols.map` are read
from the boilerplate checkout at build time; only the files above are copies
inside this repository.

## Known limitations

* **Regular expressions.** The console's libc has no `regcomp`. mpv only uses it
  for `--sub-filter-regex-*`, which wiliwili never sets, so the option is
  unavailable and nothing else changes.
* **`libkernel_sys` is avoided.** Ten POSIX wrappers live only in that system
  module. They are implemented in `libc_shims.c` on top of the calls libkernel
  does serve (`open`, `unlink`, `chmod`, `getpid`), and `readlink`, `symlink` and
  `link` are routed to the console's own POSIX layer in `libScePosixForWebKit`,
  so the title imports nothing only system software has.
* **Process creation.** `fork` reports `ENOSYS`. A title runs one image; mpv only
  forks for external tools, which wiliwili does not use.
* **Hardware video decode.** Not available to a homebrew title. The video is
  decoded on the CPU, which is what the PS5 defaults already reflect.
* **Audio** goes through SDL's PS5 backend. If the console's audio output is
  busy, that is a log line, not a failure to start.
