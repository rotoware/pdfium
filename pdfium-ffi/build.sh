#!/bin/sh
# libpdfium.a (pdfium + glue/pdfking.cpp) for each target pdfium-ffi links:
# out/release/<rust target>/libpdfium.a, PDFIUM_LIB_DIR=out/release for
# build.rs, and out/release/pdfium-<rust target>.tar.gz, the release assets
# it downloads otherwise.
#
# Needs a gclient synced checkout, depot_tools and emcc on PATH; the native
# target a mac host. GN_ARGS is added to the native args, e.g. a hermetic SDK:
#   GN_ARGS='use_system_xcode=false mac_sdk_official_version="26.1"'
# Targets as arguments, all of them by default.
set -eu
cd "$(dirname "$0")/.."

ARGS='is_debug=false treat_warnings_as_errors=false pdf_use_skia=false pdf_enable_xfa=false pdf_enable_v8=false is_component_build=false clang_use_chrome_plugins=false pdf_is_standalone=true use_debug_fission=false use_custom_libcxx=false use_sysroot=false pdf_is_complete_lib=true pdf_use_partition_alloc=false symbol_level=0'

pack() { # <rust target> <libpdfium.a>
  mkdir -p "out/release/$1"
  cp "$2" "out/release/$1/libpdfium.a"
  tar -czf "out/release/pdfium-$1.tar.gz" -C "out/release/$1" libpdfium.a
  echo "out/release/pdfium-$1.tar.gz"
}

# emscripten is a secondary toolchain (//:pdfium_wasm); EMCC_CFLAGS reaches
# every compile: wasm SjLj, as rustc's emscripten targets link
wasm() { # <rust target> <emcc flags>
  gn gen "out/$1" --args="$ARGS use_glib=false use_system_xcode=true emscripten_path=\"$(dirname "$(command -v emcc)")\""
  EMCC_CFLAGS="$2" ninja -C "out/$1" pdfium_wasm
  pack "$1" "out/$1/wasm/obj/libpdfium.a"
}

native() { # <rust target> <target_os> <target_cpu>
  gn gen "out/$1" --args="$ARGS is_clang=true target_os=\"$2\" target_cpu=\"$3\" ${GN_ARGS:-}"
  ninja -C "out/$1" pdfium
  pack "$1" "out/$1/obj/libpdfium.a"
}

[ $# -gt 0 ] || set -- wasm32-unknown-emscripten wasm64-unknown-emscripten aarch64-apple-darwin
for target in "$@"; do
  case $target in
    wasm32-unknown-emscripten) wasm "$target" "-sSUPPORT_LONGJMP=wasm" ;;
    wasm64-unknown-emscripten) wasm "$target" "-sSUPPORT_LONGJMP=wasm -sMEMORY64" ;;
    aarch64-apple-darwin) native "$target" mac arm64 ;;
    *) echo "unknown target $target" >&2 && exit 1 ;;
  esac
done
