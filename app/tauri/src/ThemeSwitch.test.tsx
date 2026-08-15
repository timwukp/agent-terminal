// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { cleanup, fireEvent, render, screen } from "@testing-library/react";
import ThemeSwitch from "./ThemeSwitch";
import { loadPref, savePref } from "./theme";

beforeEach(() => {
  // The control reads the OS through matchMedia; jsdom's answer is "dark".
  vi.stubGlobal(
    "matchMedia",
    vi.fn(() => ({
      matches: false,
      media: "(prefers-color-scheme: light)",
      addEventListener: () => {},
      removeEventListener: () => {},
    })),
  );
});

afterEach(() => {
  cleanup();
  window.localStorage.clear();
  delete document.documentElement.dataset.theme;
  vi.unstubAllGlobals();
});

const select = () => screen.getByRole("combobox") as HTMLSelectElement;

describe("ThemeSwitch", () => {
  it("offers system as a first-class choice, not just light and dark", () => {
    // A two-way toggle cannot express "follow the OS from now on", which
    // is the default this app ships with.
    render(<ThemeSwitch />);
    expect([...select().options].map((o) => o.value)).toEqual(["system", "light", "dark"]);
  });

  it("shows the stored preference on mount", () => {
    savePref("light");
    render(<ThemeSwitch />);
    expect(select().value).toBe("light");
  });

  it("applies AND persists the choice", () => {
    render(<ThemeSwitch />);
    fireEvent.change(select(), { target: { value: "light" } });
    expect(document.documentElement.dataset.theme).toBe("light");
    expect(loadPref()).toBe("light");
  });

  it("choosing system resolves through the OS rather than storing a color", () => {
    // Mutation: store the resolved name instead of "system" and the app
    // stops following the OS after the first restart — invisible in a
    // session that never changes appearance.
    render(<ThemeSwitch />);
    fireEvent.change(select(), { target: { value: "system" } });
    expect(loadPref()).toBe("system");
    expect(document.documentElement.dataset.theme).toBe("dark");
  });

  it("has an accessible name", () => {
    render(<ThemeSwitch />);
    expect(screen.getByLabelText(/theme/i)).toBe(select());
  });
});
