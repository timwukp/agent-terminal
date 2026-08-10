// GUI client for agent-terminal. The webview shell renders the layout;
// session io flows through src/session.rs (binary IPC channel) and
// session management through src/control.rs (short-lived connections).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod control;
mod session;
mod templates;

fn main() {
    tauri::Builder::default()
        .manage(session::SessionState::default())
        .invoke_handler(tauri::generate_handler![
            session::attach_session,
            session::stdin_data,
            session::resize,
            session::select_pane,
            session::zoom_toggle,
            session::split_pane,
            session::close_pane,
            session::detach,
            control::list_sessions,
            control::new_session,
            control::kill_session,
            templates::list_templates,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
