//! Connection management for the GUI client: socket path resolution and
//! the async connection (HELLO handshake, event stream, senders).
//!
//! The wire format lives in at-proto; this crate owns io and lifecycle.
//! Backpressure is structural: the reader task sends parsed events into
//! a bounded channel and awaits when it is full, so a slow consumer
//! stops the socket reads and the daemon's write buffer absorbs the
//! burst — the daemon already handles slow clients (the CLI is one).

mod conn;

pub use conn::{connect, Client, ClientError, Event, EventRx, HELLO_TIMEOUT};

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

/// socket_path from this process's environment.
pub fn default_socket_path() -> PathBuf {
    let xdg = std::env::var("XDG_RUNTIME_DIR").ok();
    let home = std::env::var("HOME").unwrap_or_default();
    socket_path(xdg.as_deref(), &home)
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
