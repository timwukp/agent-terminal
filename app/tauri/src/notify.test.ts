import { describe, expect, it, vi, beforeEach } from "vitest";

vi.mock("@tauri-apps/plugin-notification", () => ({
  isPermissionGranted: vi.fn(),
  requestPermission: vi.fn(),
  sendNotification: vi.fn(),
}));

import {
  isPermissionGranted,
  requestPermission,
  sendNotification,
} from "@tauri-apps/plugin-notification";
import { decideNotify, deliverNotification } from "./notify";

describe("decideNotify", () => {
  // The full truth table — four cases, and each one is a real scenario:
  it("focused: neither notify nor badge (the user is watching)", () => {
    expect(decideNotify(true, false)).toEqual({ notify: false, badge: false });
    expect(decideNotify(true, true)).toEqual({ notify: false, badge: false });
  });

  it("unfocused + unmuted: notify and badge", () => {
    expect(decideNotify(false, false)).toEqual({ notify: true, badge: true });
  });

  it("unfocused + muted: badge only — mute silences the pop-up, not the record", () => {
    expect(decideNotify(false, true)).toEqual({ notify: false, badge: true });
  });
});

describe("deliverNotification", () => {
  beforeEach(() => vi.clearAllMocks());

  it("sends with the session in the title and the last line as body", async () => {
    vi.mocked(isPermissionGranted).mockResolvedValue(true);
    expect(await deliverNotification("work", "✅ 42 tests passed")).toBe(true);
    expect(sendNotification).toHaveBeenCalledWith({
      title: "work — finished",
      body: "✅ 42 tests passed",
    });
  });

  it("filters the session name in the title, and leaves the body alone", async () => {
    // Two decisions in one assertion, both argued in SECURITY.md. The TITLE
    // is filtered because our own " — finished" sits downstream of a
    // daemon-supplied string: U+202E in the name reverses the suffix too, and
    // the notification stops saying which session finished. The BODY is not,
    // because it is one program's own output with no trusted text beside it
    // and no action attached — and because bidi marks and ZWJ are how
    // correct software renders RTL text and emoji. What a body can actually
    // carry is measured in terminal/screenLine.test.ts.
    vi.mocked(isPermissionGranted).mockResolvedValue(true);
    const body = "done\u202E hs.log";
    expect(await deliverNotification("proj\u202Egol.hs", body)).toBe(true);
    expect(sendNotification).toHaveBeenCalledWith({
      title: "projgol.hs — finished",
      body,
    });
  });

  it("marks an invisible character in the title instead of dropping it", async () => {
    vi.mocked(isPermissionGranted).mockResolvedValue(true);
    await deliverNotification("de\u200Bploy", "x");
    expect(sendNotification).toHaveBeenCalledWith({
      title: "de\uFFFDploy — finished",
      body: "x",
    });
  });

  it("asks for permission once when not yet granted", async () => {
    vi.mocked(isPermissionGranted).mockResolvedValue(false);
    vi.mocked(requestPermission).mockResolvedValue("granted");
    expect(await deliverNotification("work", "x")).toBe(true);
    expect(requestPermission).toHaveBeenCalledTimes(1);
  });

  it("denied permission: returns false and sends nothing", async () => {
    vi.mocked(isPermissionGranted).mockResolvedValue(false);
    vi.mocked(requestPermission).mockResolvedValue("denied");
    expect(await deliverNotification("work", "x")).toBe(false);
    expect(sendNotification).not.toHaveBeenCalled();
  });

  it("a throwing plugin degrades to false, not an unhandled rejection", async () => {
    // The unbundled-macOS-binary case: the plugin can reject at send time.
    vi.mocked(isPermissionGranted).mockResolvedValue(true);
    vi.mocked(sendNotification).mockImplementation(() => {
      throw new Error("no bundle");
    });
    expect(await deliverNotification("work", "x")).toBe(false);
  });
});
