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
import { displayName } from "./displayName";

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
      // The name is filtered, and the reason is the " — finished" after it:
      // this is trusted text sitting downstream of a string the daemon
      // supplied, and one U+202E in the name reverses the suffix along with
      // it, leaving a notification that no longer says which session
      // finished. displayName also keeps two look-alike names looking
      // different, which is what makes the notification worth reading.
      title: `${displayName(session)} — finished`,
      // The last non-empty screen line, so the notification says WHAT
      // finished, read from the GUI's own buffer rather than the wire.
      //
      // NOT filtered, deliberately (SECURITY.md, and measured in
      // terminal/screenLine.test.ts). The body is one program's own output
      // with no trusted text beside it and no action attached to it, so
      // reordering it produces a misleading sentence — which that program
      // could equally write in plain ASCII. Filtering would cost real
      // output: bidi marks are how correctly-written software renders mixed
      // RTL text, and ZWJ and variation selectors are how it prints emoji.
      body: lastLine || undefined,
    });
    return true;
  } catch {
    return false;
  }
}
