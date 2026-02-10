mod auth;
mod cookie_storage;
mod device;

use auth::AuthClient;
use device::{discover_move, validate_device, MoveDevice};

#[tauri::command]
async fn find_device() -> Result<MoveDevice, String> {
    discover_move().await
}

#[tauri::command]
async fn validate_device_at(base_url: String) -> Result<bool, String> {
    validate_device(&base_url).await
}

#[tauri::command]
async fn submit_auth_code(base_url: String, code: String) -> Result<String, String> {
    let client = AuthClient::new(base_url);
    let cookie = client.submit_code(&code).await?;
    cookie_storage::save_cookie(&cookie.value)?;
    Ok(cookie.value)
}

#[tauri::command]
async fn submit_ssh_key_with_auth(base_url: String, pubkey: String) -> Result<(), String> {
    let client = AuthClient::new(base_url);
    client.submit_ssh_key(&pubkey).await
}

#[tauri::command]
fn get_saved_cookie() -> Result<Option<String>, String> {
    cookie_storage::load_cookie()
}

#[tauri::command]
fn clear_saved_cookie() -> Result<(), String> {
    cookie_storage::delete_cookie()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
  tauri::Builder::default()
    .setup(|app| {
      if cfg!(debug_assertions) {
        app.handle().plugin(
          tauri_plugin_log::Builder::default()
            .level(log::LevelFilter::Info)
            .build(),
        )?;
      }
      Ok(())
    })
    .invoke_handler(tauri::generate_handler![
      find_device,
      validate_device_at,
      submit_auth_code,
      submit_ssh_key_with_auth,
      get_saved_cookie,
      clear_saved_cookie
    ])
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
