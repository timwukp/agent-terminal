// The version line the About box should have been: the release train
// plus the tree identity the build was made from, so "which build am I
// running?" is answerable from the screen instead of by comparing the
// process's txt inode against the file on disk.
import { useEffect, useState } from "react";
import type { AppVersion, VersionApi } from "./versionApi";
import { theme } from "./theme";

export default function VersionBadge({ api }: { api: VersionApi }) {
  const [v, setV] = useState<AppVersion | null>(null);
  useEffect(() => {
    let alive = true;
    api.appVersion().then(
      (res) => {
        if (alive) setV(res);
      },
      // No IPC (plain-browser dev, or a backend too old to answer):
      // render nothing rather than a stamp that identifies nothing.
      () => {},
    );
    return () => {
      alive = false;
    };
  }, [api]);
  if (!v) return null;
  return (
    <div
      title="version (build tree identity)"
      style={{
        fontSize: 9,
        color: theme.textMuted,
        paddingTop: 6,
        userSelect: "text",
      }}
    >
      v{v.semver} ({v.build})
    </div>
  );
}
