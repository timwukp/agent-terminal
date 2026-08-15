// Theme selection. Three states, not a two-way toggle: "System" has to
// remain selectable AFTER a manual choice, because following the OS is a
// standing instruction (ux-spec.md), not a one-time copy of its current
// value. A toggle can express "light" and "dark" but not "whatever the
// laptop does at sunset".

import { useState } from "react";
import { applyPref, loadPref, theme, type ThemePref } from "./theme";

const PREFS: readonly ThemePref[] = ["system", "light", "dark"];
const LABELS: Record<ThemePref, string> = {
  system: "System",
  light: "Light",
  dark: "Dark",
};

export default function ThemeSwitch() {
  // Read once: the stored value only changes through this control.
  const [pref, setPref] = useState<ThemePref>(() => loadPref());

  return (
    <label
      style={{
        display: "flex",
        alignItems: "center",
        gap: 6,
        marginTop: 6,
        paddingTop: 6,
        borderTop: `1px solid ${theme.border}`,
        fontSize: 11,
        color: theme.textMuted,
      }}
    >
      Theme
      <select
        value={pref}
        onChange={(e) => {
          const next = e.target.value as ThemePref;
          setPref(next);
          applyPref(next);
        }}
        style={{
          flex: 1,
          minWidth: 0,
          fontSize: 11,
          padding: "2px 4px",
          borderRadius: 4,
          background: theme.raised,
          color: theme.text,
          border: `1px solid ${theme.raisedBorder}`,
        }}
      >
        {PREFS.map((p) => (
          <option key={p} value={p}>
            {LABELS[p]}
          </option>
        ))}
      </select>
    </label>
  );
}
