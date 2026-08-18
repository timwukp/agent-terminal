// @vitest-environment jsdom
import { afterEach, describe, expect, it } from "vitest";
import { act, cleanup, render } from "@testing-library/react";
import VersionBadge from "./VersionBadge";
import type { VersionApi } from "./versionApi";

afterEach(cleanup);

const flush = () => act(async () => {});

describe("VersionBadge", () => {
  it("renders exactly what the backend reports — semver AND build stamp", async () => {
    const api: VersionApi = {
      appVersion: async () => ({
        semver: "9.9.9",
        build: "abcdef123456-dirty.12345678",
      }),
    };
    const { container } = render(<VersionBadge api={api} />);
    await flush();
    // The whole point of the badge is that the stamp is visible, not
    // just the semver — two builds once shared "0.1.0" while behaving
    // differently, and only the tree identity could tell them apart.
    expect(container.textContent).toBe("v9.9.9 (abcdef123456-dirty.12345678)");
  });

  it("renders nothing when the backend cannot answer", async () => {
    const api: VersionApi = {
      appVersion: () => Promise.reject(new Error("no ipc")),
    };
    const { container } = render(<VersionBadge api={api} />);
    await flush();
    expect(container.textContent).toBe("");
  });
});
