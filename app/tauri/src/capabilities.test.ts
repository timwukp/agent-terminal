// The Tauri capability file is an ACL, and both of its failure modes are
// silent. Grant too little and the call fails inside the plugin's own
// wrapper with nothing on screen — that is how `notification:default` came
// to be there in the first place. Grant too much and nothing happens at
// all, which is precisely why an over-broad grant survives review.
//
// So this test derives the required set from the source that does the
// calling and compares it to the file, in both directions.
//
// Files are pulled in through import.meta.glob rather than node:fs on
// purpose: the app has no @types/node and does not need one, and the glob
// is resolved by the same bundler that builds the app.
import { describe, expect, it } from "vitest";

const SOURCES = import.meta.glob("./**/*.{ts,tsx}", {
  query: "?raw",
  eager: true,
  import: "default",
}) as Record<string, string>;

const CAPABILITY = import.meta.glob("../src-tauri/capabilities/default.json", {
  query: "?raw",
  eager: true,
  import: "default",
}) as Record<string, string>;

/** Every notification API this app may import, mapped to the ACL
 * permission that makes it work.
 *
 * Read out of tauri-plugin-notification 2.3.3 itself, because the mapping
 * is not guessable from the JS names. `init()` in the crate's src/lib.rs
 * registers exactly three commands — is_permission_granted,
 * request_permission, notify — and only the first is reached by an
 * `invoke()` in the plugin's dist-js. `requestPermission()` and
 * `sendNotification()` go through `window.Notification`, which the plugin
 * REPLACES from an injected init script (its src/init-iife.js), so
 * grepping the JS package for invoke() calls under-counts them. */
const COMMAND_FOR_API: Record<string, string> = {
  isPermissionGranted: "notification:allow-is-permission-granted",
  requestPermission: "notification:allow-request-permission",
  sendNotification: "notification:allow-notify",
};

/** Names imported from the notification plugin anywhere in src/, tests
 * excluded — a mock in a test file is not a call. */
function importedNotificationApis(): { apis: Set<string>; scanned: number } {
  const apis = new Set<string>();
  let scanned = 0;
  for (const [path, text] of Object.entries(SOURCES)) {
    if (/\.test\.tsx?$/.test(path)) continue;
    scanned++;
    const re = /import\s*\{([^}]*)\}\s*from\s*["']@tauri-apps\/plugin-notification["']/g;
    for (const m of text.matchAll(re)) {
      for (const raw of m[1].split(",")) {
        const name = raw.trim().split(/\s+as\s+/)[0].trim();
        if (name) apis.add(name);
      }
    }
  }
  return { apis, scanned };
}

describe("notification capability grants", () => {
  const raw = Object.values(CAPABILITY)[0];
  const { apis, scanned } = importedNotificationApis();

  it("read the files it is reasoning about", () => {
    // Guards everything below: a scan that reached no source, or a
    // capability file that did not load, would make every comparison
    // vacuously true.
    expect(scanned).toBeGreaterThan(10);
    expect(apis.size).toBeGreaterThan(0);
    expect(raw).toBeTruthy();
  });

  const granted: string[] = JSON.parse(raw).permissions;
  const notification = granted.filter((p) => p.startsWith("notification:"));

  it("knows what every imported API needs", () => {
    // A new import with no entry above must fail here rather than pass by
    // being invisible to the comparison.
    expect([...apis].filter((a) => !(a in COMMAND_FOR_API))).toEqual([]);
  });

  it("grants exactly the commands the app calls", () => {
    const required = [...apis].map((a) => COMMAND_FOR_API[a]).sort();
    expect(notification.slice().sort()).toEqual(required);
  });

  it("does not take the plugin's default permission set", () => {
    // notification:default is all 16 of the plugin's permissions, 13 of
    // which name commands this build does not even register.
    expect(notification).not.toContain("notification:default");
  });
});
