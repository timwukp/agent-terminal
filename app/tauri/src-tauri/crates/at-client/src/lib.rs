//! Connection management for the GUI client: socket path resolution,
//! HELLO/ATTACH state machines, output backpressure. Lands in PR3;
//! PR1 ships only the socket path logic so the crate compiles with a
//! real, tested unit.

use std::path::PathBuf;

/// Resolve the daemon's socket path exactly as the C client does
/// (src/client/attach.c): `$XDG_RUNTIME_DIR/agent-terminal/default.sock`
/// when XDG_RUNTIME_DIR is set and non-empty, else
/// `$HOME/.agent-terminal/run/default.sock`.
pub fn socket_path(xdg_runtime_dir: Option<&str>, home: &str) -> PathBuf {
    match xdg_runtime_dir {
        Some(dir) if !dir.is_empty() => PathBuf::from(dir)
            .join("agent-terminal")
            .join("default.sock"),
        _ => PathBuf::from(home)
            .join(".agent-terminal")
            .join("run")
            .join("default.sock"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xdg_wins_when_set() {
        assert_eq!(
            socket_path(Some("/run/user/501"), "/home/u"),
            PathBuf::from("/run/user/501/agent-terminal/default.sock")
        );
    }

    #[test]
    fn empty_xdg_falls_back_to_home() {
        assert_eq!(
            socket_path(Some(""), "/home/u"),
            PathBuf::from("/home/u/.agent-terminal/run/default.sock")
        );
    }

    #[test]
    fn missing_xdg_falls_back_to_home() {
        assert_eq!(
            socket_path(None, "/home/u"),
            PathBuf::from("/home/u/.agent-terminal/run/default.sock")
        );
    }
}
