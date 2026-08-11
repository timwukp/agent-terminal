// Notification policy and delivery (design: app/design/notifications.md).
//
// Policy is a pure function so the wiring tests can enumerate it; delivery
// is the thin impure edge. Split this way because the failure that matters
// — notifying the person who is already looking, or staying silent for the
// one who walked away — is a policy bug, and policy must be testable
// without an OS notification center in the loop.

import {
  isPermissionGranted,
  requestPermission,
  sendNotification,
} from "@tauri-apps/plugin-notification";

export interface NotifyDecision {
  /** Show an OS notification. */
  notify: boolean;
  /** Mark the session's sidebar row. Independent of `notify`: the badge
   * also covers the delivery-failed path (unbundled dev binaries on macOS
   * cannot post to the notification center), so it is set whenever the
   * turn completed unwatched, muted or not — a mute silences the pop-up,
   * not the record that something finished. */
  badge: boolean;
}

/** A turn finished (bell or idle). Decide what to surface.
 * Focused windows never notify: the user is already watching. */
export function decideNotify(windowFocused: boolean, sessionMuted: boolean): NotifyDecision {
  if (windowFocused) return { notify: false, badge: false };
  return { notify: !sessionMuted, badge: true };
}

/** Deliver an OS notification; returns false when permission is denied or
 * delivery throws (the caller already set the badge, so failure here
 * degrades silently by design). */
export async function deliverNotification(session: string, lastLine: string): Promise<boolean> {
  try {
    let granted = await isPermissionGranted();
    if (!granted) granted = (await requestPermission()) === "granted";
    if (!granted) return false;
    sendNotification({
      title: `${session} — finished`,
      // The last non-empty screen line, so the notification says WHAT
      // finished, read from the GUI's own buffer rather than the wire.
      body: lastLine || undefined,
    });
    return true;
  } catch {
    return false;
  }
}
