//! Claude Code transcript watcher (design: app/design/claude-panel.md).
//! Lands in PR6; PR1 ships only the cwd→project-slug mapping the
//! correlation design depends on.

/// Map a working directory to Claude Code's project slug: every `/`
/// becomes `-`. `/opt/proj` → `-opt-proj`.
pub fn project_slug(cwd: &str) -> String {
    cwd.replace('/', "-")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slug_replaces_every_slash() {
        assert_eq!(project_slug("/opt/proj"), "-opt-proj");
    }

    #[test]
    fn root_is_single_dash() {
        assert_eq!(project_slug("/"), "-");
    }
}
