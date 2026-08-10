//! Build script. Beyond `tauri_build::build()`, this asserts the frontend
//! bundle is present and not older than its sources.
//!
//! Why this is not paranoia: `tauri::generate_context!` embeds `../dist`
//! at compile time, but `tauri-build` emits `rerun-if-changed` only for
//! `tauri.conf.json` and `capabilities/` — nothing for the frontend. So
//! cargo has no idea `dist/` is an input. Two measured consequences:
//!
//!   * With `dist/` deleted outright, `cargo build` still exited 0 and
//!     produced a binary referencing no bundled asset at all.
//!   * With `dist/` merely stale, `cargo build` paired new Rust commands
//!     with old JavaScript. That binary rejected 100% of keystrokes and
//!     read as a product bug ("typing does nothing"), not a build error.
//!
//! `beforeBuildCommand` in tauri.conf.json does not cover this: it is a
//! `tauri build` CLI feature, and the documented build path here — in the
//! README and in app-ci.yml's tauri-build job — is bare `cargo build`.
//! Nothing runs it. This does.

use std::fs;
use std::path::Path;
use std::time::SystemTime;

/// Frontend inputs, relative to this file. Kept explicit rather than
/// globbed so a new top-level input has to be added consciously.
const SOURCES: &[&str] = &[
    "../src",
    "../index.html",
    "../package.json",
    "../vite.config.ts",
];

fn main() {
    // Tell cargo the frontend is a build input, so editing a .tsx file
    // forces this script to re-run and the crate (with its embedding
    // macro) to recompile. Without these lines cargo caches the old
    // binary and the assertions below never get a chance to fire.
    for s in SOURCES {
        println!("cargo:rerun-if-changed={s}");
    }
    println!("cargo:rerun-if-changed=../dist");

    check_frontend();
    tauri_build::build();
}

fn check_frontend() {
    let dist = Path::new("../dist");
    if !dist.join("index.html").is_file() {
        fail("no frontend bundle: ../dist/index.html is missing");
    }

    // Strictly newer, so a source edited in the same second as the build
    // is not a failure — only a bundle that provably predates its input.
    let (dist_mtime, _) = newest(dist);
    for s in SOURCES {
        let (mtime, path) = newest(Path::new(s));
        if mtime > dist_mtime {
            fail(&format!(
                "stale frontend bundle: {path} is newer than ../dist"
            ));
        }
    }
}

fn fail(why: &str) -> ! {
    panic!(
        "{why}.\n\n\
         ../dist is embedded into this binary at compile time, so building\n\
         only the Rust side ships old (or no) JavaScript — which looks like\n\
         a broken app, not a broken build. Rebuild the frontend first:\n\n\
         \x20   cd app/tauri && npm ci && npm run build\n"
    )
}

/// Newest mtime under `path` (the file's own if it is a file), with the
/// path that carried it, for the error message. A missing path reports
/// the epoch so it cannot make anything look stale: a missing *source*
/// is not this script's business, and a missing dist is caught above.
fn newest(path: &Path) -> (SystemTime, String) {
    let meta = match fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(_) => return (SystemTime::UNIX_EPOCH, path.display().to_string()),
    };
    if !meta.is_dir() {
        // Test files are typechecked but never bundled, so they cannot
        // make dist stale — excluding them keeps the guard from crying
        // wolf when only a test changed.
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        if name.contains(".test.") {
            return (SystemTime::UNIX_EPOCH, path.display().to_string());
        }
        let t = meta.modified().unwrap_or(SystemTime::UNIX_EPOCH);
        return (t, path.display().to_string());
    }

    let mut best = (SystemTime::UNIX_EPOCH, path.display().to_string());
    let entries = match fs::read_dir(path) {
        Ok(e) => e,
        Err(_) => return best,
    };
    for entry in entries.flatten() {
        let child = newest(&entry.path());
        if child.0 > best.0 {
            best = child;
        }
    }
    best
}
