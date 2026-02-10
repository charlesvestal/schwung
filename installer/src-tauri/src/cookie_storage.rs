use keyring::Entry;

const SERVICE_NAME: &str = "move-installer";
const COOKIE_KEY: &str = "auth-cookie";

/// Save auth cookie to platform keychain
pub fn save_cookie(cookie_value: &str) -> Result<(), String> {
    let entry = Entry::new(SERVICE_NAME, COOKIE_KEY)
        .map_err(|e| format!("Failed to access keychain: {}", e))?;

    entry.set_password(cookie_value)
        .map_err(|e| format!("Failed to save cookie: {}", e))
}

/// Load auth cookie from platform keychain
pub fn load_cookie() -> Result<Option<String>, String> {
    let entry = Entry::new(SERVICE_NAME, COOKIE_KEY)
        .map_err(|e| format!("Failed to access keychain: {}", e))?;

    match entry.get_password() {
        Ok(cookie) => Ok(Some(cookie)),
        Err(keyring::Error::NoEntry) => Ok(None),
        Err(e) => Err(format!("Failed to load cookie: {}", e)),
    }
}

/// Delete saved cookie from keychain
pub fn delete_cookie() -> Result<(), String> {
    let entry = Entry::new(SERVICE_NAME, COOKIE_KEY)
        .map_err(|e| format!("Failed to access keychain: {}", e))?;

    entry.delete_password()
        .map_err(|e| format!("Failed to delete cookie: {}", e))
}
