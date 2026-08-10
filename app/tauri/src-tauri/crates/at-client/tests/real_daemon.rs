//! Integration tests against the REAL agent-terminald — no mock daemon
//! to drift (repo rule: verify against the real artifact). The daemon
//! binary must exist at ../../build/$BUILD/agent-terminald relative to
//! the repo root; tests are skipped with a loud message if absent, and
//! app-ci.yml builds it first so CI never skips.
//!
//! Isolation follows tests/integration/lib.sh: HOME = a mktemp dir,
//! XDG_RUNTIME_DIR unset, daemon in the foreground, killed by ITS pid
//! only (never by name — the production daemon may be running).

use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use at_client::{connect, Event};
use at_proto as proto;
use tokio::io::AsyncReadExt;
use tokio::process::{Child, Command};

fn daemon_bin() -> Option<PathBuf> {
    let build = std::env::var("BUILD").unwrap_or_else(|_| "release".into());
    // crate dir = app/tauri/src-tauri/crates/at-client → repo root is 5 up.
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(5)?
        .to_path_buf();
    let bin = root.join("build").join(build).join("agent-terminald");
    if !bin.exists() {
        // A skipped test still reports "ok", so a missing binary would
        // read as 9 passes that asserted nothing. CI sets
        // AT_REQUIRE_DAEMON=1 to turn that silent skip into a failure;
        // locally the skip stays legal (not everyone builds C first).
        assert!(
            std::env::var("AT_REQUIRE_DAEMON").is_err(),
            "AT_REQUIRE_DAEMON set but {} is missing — build the daemon first",
            bin.display()
        );
        return None;
    }
    Some(bin)
}

struct DaemonFixture {
    child: Child,
    home: tempfile::TempDir,
}

impl DaemonFixture {
    async fn start() -> Option<Self> {
        let bin = match daemon_bin() {
            Some(b) => b,
            None => {
                eprintln!("SKIP: agent-terminald not built (make all first)");
                return None;
            }
        };
        let home = tempfile::tempdir().expect("tempdir");
        let mut child = Command::new(&bin)
            .arg("-f")
            .arg("-v")
            .env_clear()
            .env("HOME", home.path())
            .env("PATH", "/usr/bin:/bin")
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .kill_on_drop(true) // kill by OUR pid on drop, never by name
            .spawn()
            .expect("spawn daemon");

        // Wait for the listen line on stderr (lib.sh's wait_for).
        let mut stderr = child.stderr.take().expect("stderr piped");
        let listening = tokio::time::timeout(Duration::from_secs(5), async {
            let mut acc = Vec::new();
            let mut buf = [0u8; 1024];
            loop {
                let n = stderr.read(&mut buf).await.ok()?;
                if n == 0 {
                    return None;
                }
                acc.extend_from_slice(&buf[..n]);
                if String::from_utf8_lossy(&acc).contains("listening on") {
                    return Some(());
                }
            }
        })
        .await;
        assert!(
            matches!(listening, Ok(Some(()))),
            "daemon never logged a listen line"
        );
        // Keep draining stderr so the daemon never blocks on a full pipe.
        tokio::spawn(async move {
            let mut sink = [0u8; 4096];
            while matches!(stderr.read(&mut sink).await, Ok(n) if n > 0) {}
        });
        Some(Self { child, home })
    }

    fn sock(&self) -> PathBuf {
        self.home
            .path()
            .join(".agent-terminal")
            .join("run")
            .join("default.sock")
    }

    async fn stop(mut self) {
        let _ = self.child.kill().await;
    }
}

/// Await the next event, bounded — a silent daemon must fail the test,
/// not hang CI.
async fn next_ev(rx: &mut at_client::EventRx) -> Event {
    tokio::time::timeout(Duration::from_secs(10), rx.recv())
        .await
        .expect("timed out waiting for daemon event")
        .expect("event stream closed unexpectedly")
}

#[tokio::test]
async fn hello_reports_daemon_identity() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (client, _rx) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect");
    assert_eq!(client.hello.ver, proto::PROTO_VERSION);
    let pid = client.hello.daemon_pid.expect("daemon sends pid");
    assert_eq!(pid, d.child.id().expect("child pid"));
    assert_eq!(client.hello.generation, Some(0));
    let flags = client.hello.server_flags.expect("daemon sends flags");
    assert_ne!(flags & proto::SERVER_CAP_PANES, 0, "panes advertised");
    client.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn new_session_snapshot_output_stdin_roundtrip() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect");

    // cat for deterministic echo; the READY prefix proves the child is
    // running with the pty slave open — bytes written to the master
    // before that are discarded (measured: an immediate stdin_data after
    // NEW_SESSION never echoes; the same bytes 1.5 s later do).
    c.send(
        &proto::new_session(80, 24, "t", &["/bin/sh", "-c", "echo READY; exec /bin/cat"]).unwrap(),
    )
    .await
    .unwrap();

    // Creation attaches us: first a snapshot of the (blank) screen.
    let (cols, rows) = loop {
        match next_ev(&mut rx).await {
            Event::Snapshot { cols, rows, .. } => break (cols, rows),
            Event::Layout(_) => continue,
            other => panic!("expected snapshot first, got {other:?}"),
        }
    };
    assert_eq!((cols, rows), (80, 24));

    // Wait for the child's own READY before typing at it.
    let mut pre = Vec::new();
    loop {
        match next_ev(&mut rx).await {
            Event::Output(b) => {
                pre.extend_from_slice(&b);
                if String::from_utf8_lossy(&pre).contains("READY") {
                    break;
                }
            }
            Event::Layout(_) => continue,
            other => panic!("expected READY output, got {other:?}"),
        }
    }

    // Type into the pty; cat echoes; the tee delivers it as OUTPUT.
    c.send(&proto::stdin_data(b"MARKER-42\n")).await.unwrap();
    let mut seen = Vec::new();
    loop {
        match next_ev(&mut rx).await {
            Event::Output(b) => {
                seen.extend_from_slice(&b);
                if String::from_utf8_lossy(&seen).contains("MARKER-42") {
                    break;
                }
            }
            Event::Layout(_) => continue,
            other => panic!("expected output, got {other:?}"),
        }
    }
    c.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn list_sessions2_shows_created_session() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    // Control connection creates a detachable session…
    let (mut c1, mut rx1) = connect(&d.sock(), 0).await.expect("connect 1");
    c1.send(&proto::new_session(100, 30, "listed", &["/bin/cat"]).unwrap())
        .await
        .unwrap();
    // …wait until it exists (snapshot = daemon confirmed).
    loop {
        if let Event::Snapshot { .. } = next_ev(&mut rx1).await {
            break;
        }
    }

    // A second connection (the GUI sidebar pattern) lists it.
    let (mut c2, mut rx2) = connect(&d.sock(), 0).await.expect("connect 2");
    c2.send(&proto::list_sessions2()).await.unwrap();
    let list = loop {
        if let Event::SessionList(l) = next_ev(&mut rx2).await {
            break l;
        }
    };
    assert_eq!(list.len(), 1);
    let e = &list[0];
    assert_eq!(e.name, "listed");
    assert_eq!((e.view_cols, e.view_rows), (100, 30));
    assert!(e.alive);
    assert_eq!(e.nclients, 1); // c1 is attached
    assert_eq!(e.npanes, Some(1));
    assert_eq!(e.zoomed, Some(false));

    c1.shutdown().await;
    c2.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn kill_session_answers_with_a_list_without_it() {
    // The sidebar's kill button relies on two things: the answer to
    // KILL_SESSION is a fresh session list (not an ack), and the killed
    // name is gone from it — that list is what refreshes the rows.
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c1, mut rx1) = connect(&d.sock(), 0).await.expect("connect 1");
    c1.send(&proto::new_session(80, 24, "doomed", &["/bin/cat"]).unwrap())
        .await
        .unwrap();
    loop {
        if let Event::Snapshot { .. } = next_ev(&mut rx1).await {
            break;
        }
    }
    // A second session proves the kill is targeted, not a clear-all.
    c1.shutdown().await;
    let (mut c2, mut rx2) = connect(&d.sock(), 0).await.expect("connect 2");
    c2.send(&proto::new_session(80, 24, "keeper", &["/bin/cat"]).unwrap())
        .await
        .unwrap();
    loop {
        if let Event::Snapshot { .. } = next_ev(&mut rx2).await {
            break;
        }
    }
    c2.shutdown().await;

    let (mut c3, mut rx3) = connect(&d.sock(), 0).await.expect("connect 3");
    c3.send(&proto::kill_session("doomed").unwrap())
        .await
        .unwrap();
    let list = loop {
        match next_ev(&mut rx3).await {
            Event::SessionList(l) => break l,
            Event::Err { code, msg } => panic!("kill failed: {code} {msg}"),
            _ => {}
        }
    };
    let names: Vec<&str> = list.iter().map(|e| e.name.as_str()).collect();
    assert_eq!(
        names,
        ["keeper"],
        "kill answers with the surviving sessions"
    );

    c3.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn multi_client_both_receive_output() {
    // The CLI-coexistence property the GUI depends on: two clients
    // attached to one session both get every OUTPUT frame.
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c1, mut rx1) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect 1");
    c1.send(
        &proto::new_session(
            80,
            24,
            "shared",
            &["/bin/sh", "-c", "echo READY; exec /bin/cat"],
        )
        .unwrap(),
    )
    .await
    .unwrap();
    // Snapshot = attached; READY = the child is actually reading the pty.
    let mut pre = Vec::new();
    loop {
        match next_ev(&mut rx1).await {
            Event::Output(b) => {
                pre.extend_from_slice(&b);
                if String::from_utf8_lossy(&pre).contains("READY") {
                    break;
                }
            }
            _ => continue,
        }
    }

    let (mut c2, mut rx2) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect 2");
    c2.send(&proto::attach(80, 24, 0, "shared").unwrap())
        .await
        .unwrap();
    loop {
        if let Event::Snapshot { .. } = next_ev(&mut rx2).await {
            break;
        }
    }

    // c2 types; BOTH clients must see the echo.
    c2.send(&proto::stdin_data(b"BOTH-SEE-THIS\n"))
        .await
        .unwrap();
    for (who, rx) in [("c1", &mut rx1), ("c2", &mut rx2)] {
        let mut seen = Vec::new();
        loop {
            match next_ev(rx).await {
                Event::Output(b) => {
                    seen.extend_from_slice(&b);
                    if String::from_utf8_lossy(&seen).contains("BOTH-SEE-THIS") {
                        break;
                    }
                }
                Event::Layout(_) => continue,
                other => panic!("{who}: expected output, got {other:?}"),
            }
        }
    }
    c1.shutdown().await;
    c2.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn attach_missing_session_is_err_no_session() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), 0).await.expect("connect");
    c.send(&proto::attach(80, 24, 0, "ghost").unwrap())
        .await
        .unwrap();
    match next_ev(&mut rx).await {
        Event::Err { code, msg } => {
            assert_eq!(code, proto::ERR_NO_SESSION);
            assert!(msg.contains("no such session"), "msg was: {msg}");
        }
        other => panic!("expected ERR, got {other:?}"),
    }
    c.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn split_delivers_layout_and_click_focus_works() {
    // The GUI pane path end-to-end: split over the wire, LAYOUT arrives
    // with two rects, SELECT_PANE by id (mode 0 — the click) re-focuses.
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect");
    c.send(&proto::new_session(120, 40, "panes", &["/bin/cat"]).unwrap())
        .await
        .unwrap();
    loop {
        if let Event::Snapshot { .. } = next_ev(&mut rx).await {
            break;
        }
    }

    c.send(&proto::split_pane(false, proto::PANE_ACTIVE))
        .await
        .unwrap();
    let layout = loop {
        match next_ev(&mut rx).await {
            Event::Layout(l) if l.panes.len() == 2 => break l,
            Event::Layout(_) | Event::Output(_) | Event::Snapshot { .. } => continue,
            other => panic!("unexpected: {other:?}"),
        }
    };
    // Side-by-side split of 120 cols: two panes, distinct x, full height.
    assert_ne!(layout.panes[0].x, layout.panes[1].x);
    assert_eq!(layout.panes[0].rows, 40);
    let first = layout.panes.iter().find(|p| p.x == 0).expect("left pane");
    // The new pane got focus (daemon behavior); click back to the left
    // pane by id — the layout that follows must have it active.
    c.send(&proto::select_pane(proto::SelectMode::ById, first.id))
        .await
        .unwrap();
    let after = loop {
        match next_ev(&mut rx).await {
            Event::Layout(l) => break l,
            Event::Output(_) => continue,
            other => panic!("unexpected: {other:?}"),
        }
    };
    assert_eq!(after.active_id, first.id, "click-to-focus routed by id");
    c.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn ping_pong_roundtrip() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), 0).await.expect("connect");
    c.send(&proto::ping(0xDEAD_BEEF_CAFE_F00D)).await.unwrap();
    match next_ev(&mut rx).await {
        Event::Pong(n) => assert_eq!(n, 0xDEAD_BEEF_CAFE_F00D),
        other => panic!("expected pong, got {other:?}"),
    }
    c.shutdown().await;
    d.stop().await;
}

#[tokio::test]
async fn session_exit_is_reported() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), 0).await.expect("connect");
    // A child that exits immediately with a distinctive status.
    c.send(&proto::new_session(80, 24, "brief", &["/bin/sh", "-c", "exit 7"]).unwrap())
        .await
        .unwrap();
    loop {
        match next_ev(&mut rx).await {
            Event::SessionExited(status) => {
                // The daemon stores WEXITSTATUS (or 128+signal) before
                // serializing — session.c — so the wire carries 7, not a
                // raw wait(2) status.
                assert_eq!(status, 7);
                break;
            }
            _ => continue,
        }
    }
    c.shutdown().await;
    d.stop().await;
}

/// Output flood: a burst through the daemon must arrive complete and
/// in order through the bounded event queue (backpressure, not loss).
#[tokio::test]
async fn flood_arrives_complete_and_ordered() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };
    let (mut c, mut rx) = connect(&d.sock(), 0).await.expect("connect");
    // seq prints 1..=2000, one per line — ~9 KB of ordered output.
    c.send(
        &proto::new_session(80, 24, "flood", &["/bin/sh", "-c", "seq 1 2000; /bin/cat"]).unwrap(),
    )
    .await
    .unwrap();
    let mut all = Vec::new();
    let deadline = tokio::time::Instant::now() + Duration::from_secs(15);
    loop {
        let ev = tokio::time::timeout_at(deadline, rx.recv())
            .await
            .expect("flood did not complete in time")
            .expect("stream closed");
        if let Event::Output(b) = ev {
            all.extend_from_slice(&b);
            if String::from_utf8_lossy(&all).contains("\n2000") {
                break;
            }
        }
    }
    let text = String::from_utf8_lossy(&all);
    // Ordered spot checks across the run (the screen is 24 rows, but the
    // raw tee carries every byte; scrollback owns history).
    let (p1, p2, p3) = (
        text.find("\n500\r").expect("500 present"),
        text.find("\n1500\r").expect("1500 present"),
        text.find("\n2000").expect("2000 present"),
    );
    assert!(p1 < p2 && p2 < p3, "output out of order");
    c.shutdown().await;
    d.stop().await;
}

/// The reported bug: keys pressed the moment a session opens vanished, so
/// the user retyped them. Two causes were client-side (xterm never
/// focused; sends before ATTACH resolved were dropped), but this pins the
/// protocol half: bytes written straight after ATTACH — with no wait for
/// a snapshot — must still reach a child that is already running.
///
/// Distinct from new_session_snapshot_output_stdin_roundtrip, which waits
/// for the child's READY first: here the child is long since started and
/// the *attach* is what races, which is what a GUI session-switch does.
#[tokio::test]
async fn stdin_right_after_attach_is_not_lost() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };

    // First connection creates the session and waits for the child to
    // hold the pty slave open.
    let (mut creator, mut crx) = connect(&d.sock(), 0).await.expect("connect");
    creator
        .send(
            &proto::new_session(
                80,
                24,
                "race",
                &["/bin/sh", "-c", "echo READY; exec /bin/cat"],
            )
            .unwrap(),
        )
        .await
        .unwrap();
    let mut pre = Vec::new();
    loop {
        match next_ev(&mut crx).await {
            Event::Output(b) => {
                pre.extend_from_slice(&b);
                if String::from_utf8_lossy(&pre).contains("READY") {
                    break;
                }
            }
            _ => continue,
        }
    }

    // Second connection attaches and types immediately — no snapshot wait.
    let (mut c, mut rx) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect");
    c.send(&proto::attach(80, 24, 0, "race").unwrap())
        .await
        .unwrap();
    c.send(&proto::stdin_data(b"NORACE-7\n")).await.unwrap();

    let mut seen = Vec::new();
    loop {
        match next_ev(&mut rx).await {
            Event::Output(b) => {
                seen.extend_from_slice(&b);
                if String::from_utf8_lossy(&seen).contains("NORACE-7") {
                    break;
                }
            }
            Event::Snapshot { blob, .. } => {
                // A snapshot may arrive first and may already contain the
                // echo; either path counts as delivered.
                seen.extend_from_slice(&blob);
                if String::from_utf8_lossy(&seen).contains("NORACE-7") {
                    break;
                }
            }
            Event::Layout(_) => continue,
            other => panic!("expected output/snapshot, got {other:?}"),
        }
    }

    c.shutdown().await;
    creator.shutdown().await;
    d.stop().await;
}

/// A viewer must not reflow the session it opens. The GUI attaches with
/// cols/rows 0 for this reason: measured on the real daemon, attaching
/// with the window's own size took a live 111x54 session down to 93x48,
/// reflowing the TUI running inside it — and because geometry is durable
/// session state, it stayed wrong after the viewer left.
///
/// Pins both halves of the sentinel: geometry is untouched, and the
/// attach still succeeds (a SNAPSHOT arrives, not ERR_BAD_REQUEST), which
/// is what makes 0 usable as "adopt the session's size".
#[tokio::test]
async fn attach_with_zero_dims_adopts_size_without_resizing() {
    let Some(d) = DaemonFixture::start().await else {
        return;
    };

    let (mut creator, mut crx) = connect(&d.sock(), 0).await.expect("connect");
    creator
        .send(&proto::new_session(97, 41, "geo", &["/bin/cat"]).unwrap())
        .await
        .unwrap();
    loop {
        match next_ev(&mut crx).await {
            Event::Snapshot { cols, rows, .. } => {
                assert_eq!((cols, rows), (97, 41), "session starts at its own size");
                break;
            }
            Event::Layout(_) => continue,
            other => panic!("expected snapshot, got {other:?}"),
        }
    }

    // The viewer attaches with 0x0 and must be handed 97x41 back.
    let (mut viewer, mut vrx) = connect(&d.sock(), proto::CLIENT_CAP_PANES)
        .await
        .expect("connect");
    viewer
        .send(&proto::attach(0, 0, 0, "geo").unwrap())
        .await
        .unwrap();
    loop {
        match next_ev(&mut vrx).await {
            Event::Snapshot { cols, rows, .. } => {
                assert_eq!(
                    (cols, rows),
                    (97, 41),
                    "viewer must adopt the session's size, not impose 0x0"
                );
                break;
            }
            Event::Layout(_) => continue,
            Event::Err { code, msg } => panic!("attach rejected: {msg} (code {code})"),
            other => panic!("expected snapshot, got {other:?}"),
        }
    }

    // And the session itself is unchanged, as seen by a third client.
    let (mut lister, mut lrx) = connect(&d.sock(), 0).await.expect("connect");
    lister.send(&proto::list_sessions2()).await.unwrap();
    // No loop: a control connection's first event is the list itself,
    // with no layout/output traffic to skip past.
    match next_ev(&mut lrx).await {
        Event::SessionList(entries) => {
            let geo = entries.iter().find(|e| e.name == "geo").expect("listed");
            assert_eq!(
                (geo.view_cols, geo.view_rows),
                (97, 41),
                "a 0x0 attach must not resize the session"
            );
        }
        other => panic!("expected session list, got {other:?}"),
    }

    lister.shutdown().await;
    viewer.shutdown().await;
    creator.shutdown().await;
    d.stop().await;
}
