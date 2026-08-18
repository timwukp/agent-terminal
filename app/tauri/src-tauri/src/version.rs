//! Build identity, answering "which build is this?" from the screen.
//!
//! The semver alone cannot: 0.1.0 named both the pre- and post-
//! deep-history bundles, and telling them apart required comparing the
//! running process's txt inode against the file on disk. `semver` is the
//! release train (tauri.conf.json / Cargo.toml, bumped per behaviour
//! change); `build` is `tools/version.sh`'s tree identity — the same
//! string `agent-terminal version` prints for the C binaries — stamped
//! at compile time by build.rs, so an edited tree can never impersonate
//! a release and no two different trees share a name.

use serde::Serialize;

#[derive(Serialize)]
pub struct AppVersion {
    pub semver: &'static str,
    pub build: &'static str,
}

#[tauri::command]
pub fn app_version() -> AppVersion {
    AppVersion {
        semver: env!("CARGO_PKG_VERSION"),
        build: env!("AT_BUILD_IDENTITY"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_is_stamped_and_not_the_fallback() {
        let v = app_version();
        assert!(!v.semver.is_empty());
        // This test runs from a git checkout (dev machine or CI's
        // actions/checkout), where version.sh always has a real answer;
        // "unknown" here means build.rs failed to run it and the badge
        // would ship a stamp that identifies nothing.
        assert_ne!(v.build, "unknown");
        assert!(!v.build.is_empty());
    }
}
