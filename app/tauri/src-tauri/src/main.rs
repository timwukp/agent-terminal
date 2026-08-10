// GUI client for agent-terminal. PR1 is scaffold only: the window opens and
// renders the frontend shell; protocol wiring lands with at-proto/at-client.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    tauri::Builder::default()
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
