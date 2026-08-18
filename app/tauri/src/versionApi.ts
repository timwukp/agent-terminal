// Build-identity source for the panel footer, behind an interface so
// tests inject fixtures (the usageApi pattern). Shapes mirror
// src-tauri/src/version.rs — the Rust side is the source of truth.
//
// Why this exists: a semver alone cannot distinguish two builds (0.1.0
// named both the pre- and post-deep-history bundles); `build` is
// tools/version.sh's tree identity, stamped at compile time.

import { invoke } from "@tauri-apps/api/core";

export interface AppVersion {
  semver: string;
  build: string;
}

export interface VersionApi {
  appVersion(): Promise<AppVersion>;
}

export class TauriVersionApi implements VersionApi {
  appVersion(): Promise<AppVersion> {
    return invoke<AppVersion>("app_version");
  }
}
