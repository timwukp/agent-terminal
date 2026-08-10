// GUI client for agent-terminal. The webview shell renders the layout;
// session io flows through src/session.rs (binary IPC channel).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod session;

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
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
