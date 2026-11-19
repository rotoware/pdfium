# pdfium-ffi

PDFium, with the pdfking patches (commits on this branch) and `glue/pdfking.cpp` (the `PDFKING_*` primitives), as Rust declarations over a prebuilt `libpdfium.a`. Depending on it is all a crate needs: its `build.rs` downloads the `libpdfium.a` of the target from this version's release and links it, nothing of pdfium is compiled.

Targets: `wasm32-unknown-emscripten`, `wasm64-unknown-emscripten` (memory64, a json target spec), `aarch64-apple-darwin`.

- emscripten: linked with the consumer's emcc, which must be the version the release was built with (emsdk 7.0.0); setjmp/longjmp are wasm SjLj, as rustc's emscripten targets link
- mac: C++ and the CoreFoundation/CoreGraphics/AppKit frameworks are linked too

## Building the libraries

From a gclient synced checkout, depot_tools and emcc on PATH, on a mac (the native target):

```bash
pdfium-ffi/build.sh [targets...]
```

writes `out/release/<target>/libpdfium.a` and `out/release/pdfium-<target>.tar.gz`. Emscripten is a secondary toolchain upstream: the wasm libraries are `//:pdfium_wasm`, `:pdfium` in `//build/toolchain/wasm:wasm`, from the host's default toolchain.

- `PDFIUM_LIB_DIR=<checkout>/out/release` links a consumer against them instead of the release
## Releasing

`main` is upstream pdfium, `master` the pdfking commits on top of it. Bump `version` in `Cargo.toml` and merge `master` into `release`: CI (`.github/workflows/release.yml`) builds every target and publishes the release `pdfium-ffi-v<version>`. Consumers pin that tag (`git = "https://github.com/rotoware/pdfium", tag = "pdfium-ffi-v<version>"`). Run by hand, the workflow only builds.
