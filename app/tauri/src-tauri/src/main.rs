// GUI client for agent-terminal. The webview shell renders the layout;
// session io flows through src/session.rs (binary IPC channel) and
// session management through src/control.rs (short-lived connections).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod control;
mod hooks;
mod idle;
mod panels;
mod session;
mod templates;
mod usage;

fn main() {
    tauri::Builder::default()
        // Notifications: the webview asks permission and sends; macOS
        // shows them only for a real .app bundle, so an unbundled debug
        // binary degrades to the sidebar badge (docs/UAT.md GUI-16).
        .plugin(tauri_plugin_notification::init())
        .manage(session::SessionState::default())
        .manage(usage::UsageState::default())
        .manage(hooks::HooksState::default())
        .manage(hooks::HookLogState::default())
        .manage(panels::PanelStreamState::default())
        .manage(control::SessionWatchState::default())
        .invoke_handler(tauri::generate_handler![
            session::attach_session,
            session::stdin_data,
            session::resize,
            session::select_pane,
            session::scrollback_req,
            session::zoom_toggle,
            session::split_pane,
            session::close_pane,
            session::detach,
            control::list_sessions,
            control::new_session,
            control::kill_session,
            control::session_watch,
            control::session_watch_stop,
            templates::list_templates,
            usage::usage_snapshot,
            hooks::hooks_snapshot,
            hooks::read_hook_script,
            hooks::hook_log_snapshot,
            panels::panel_stream,
            panels::panel_stream_stop,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
