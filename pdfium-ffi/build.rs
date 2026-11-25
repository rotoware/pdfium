// libpdfium.a (pdfium + glue/pdfking.cpp) for the target, as build.sh makes
// it: PDFIUM_LIB_DIR/<target> (build.sh's out/release), else the release
// asset of this version
use std::env;
use std::path::PathBuf;
use std::process::Command;

const RELEASES: &str = "https://github.com/rotoware/pdfium/releases/download";

fn main() {
    let target = env::var("TARGET").unwrap();
    println!("cargo:rerun-if-env-changed=PDFIUM_LIB_DIR");
    let dir = match env::var("PDFIUM_LIB_DIR") {
        Ok(dir) => PathBuf::from(dir).join(&target),
        Err(_) => download(&target),
    };
    println!("cargo:rerun-if-changed={}", dir.join("libpdfium.a").display());
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=static=pdfium");

    // PDFium is C++
    match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "macos" => {
            println!("cargo:rustc-link-lib=c++");
            println!("cargo:rustc-link-lib=framework=CoreFoundation");
            println!("cargo:rustc-link-lib=framework=CoreGraphics");
            println!("cargo:rustc-link-lib=framework=AppKit");
        }
        "linux" => {
            println!("cargo:rustc-link-lib=stdc++");
            println!("cargo:rustc-link-lib=m");
        }
        "emscripten" => {
            println!("cargo:rustc-link-lib=c++");
            println!("cargo:rustc-link-lib=c++abi");
        }
        _ => {}
    }
}

// pdfium-<target>.tar.gz of the release pdfium-ffi-v<version>, into OUT_DIR
fn download(target: &str) -> PathBuf {
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    if out.join("libpdfium.a").exists() {
        return out;
    }
    let url = format!(
        "{RELEASES}/pdfium-ffi-v{}/pdfium-{target}.tar.gz",
        env::var("CARGO_PKG_VERSION").unwrap()
    );
    let status = Command::new("sh")
        .arg("-c")
        .arg(r#"curl -sSfL -o "$2/pdfium.tar.gz" "$1" && tar -xzf "$2/pdfium.tar.gz" -C "$2""#)
        .args(["sh", &url])
        .arg(&out)
        .status()
        .expect("sh");
    assert!(status.success(), "no libpdfium.a at {url}: set PDFIUM_LIB_DIR");
    out
}
