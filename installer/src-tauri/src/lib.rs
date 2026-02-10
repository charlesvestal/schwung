mod device;

use device::{discover_move, validate_device, MoveDevice};

#[tauri::command]
async fn find_device() -> Result<MoveDevice, String> {
    discover_move().await
}

#[tauri::command]
async fn validate_device_at(base_url: String) -> Result<bool, String> {
    validate_device(&base_url).await
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
    .invoke_handler(tauri::generate_handler![find_device, validate_device_at])
    .run(tauri::generate_context!())
    .expect("error while running tauri application");
}
