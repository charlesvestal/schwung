use std::path::{Path, PathBuf};
use std::process::Command;
use std::fs;
use std::io::Write;

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

/// Get platform-specific SSH binary paths
pub fn get_ssh_paths() -> SshPaths {
    #[cfg(target_os = "windows")]
    {
        // Use bundled OpenSSH binaries on Windows
        let resource_dir = tauri::api::path::resource_dir(&tauri::generate_context!())
            .expect("Failed to get resource dir");

        SshPaths {
            ssh: resource_dir.join("bin/ssh.exe"),
            ssh_keygen: resource_dir.join("bin/ssh-keygen.exe"),
            scp: resource_dir.join("bin/scp.exe"),
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        // Use system binaries on macOS/Linux
        SshPaths {
            ssh: PathBuf::from("/usr/bin/ssh"),
            ssh_keygen: PathBuf::from("/usr/bin/ssh-keygen"),
            scp: PathBuf::from("/usr/bin/scp"),
        }
    }
}

pub struct SshPaths {
    pub ssh: PathBuf,
    pub ssh_keygen: PathBuf,
    pub scp: PathBuf,
}

/// Find existing SSH key or return None
pub fn find_ssh_key() -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    let ssh_dir = home.join(".ssh");

    // Check for ableton_move key first
    let ableton_key = ssh_dir.join("ableton_move.pub");
    if ableton_key.exists() && ssh_dir.join("ableton_move").exists() {
        return Some(ableton_key);
    }

    // Check for default keys
    for key_name in &["id_ed25519", "id_rsa", "id_ecdsa"] {
        let pubkey = ssh_dir.join(format!("{}.pub", key_name));
        let privkey = ssh_dir.join(key_name);
        if pubkey.exists() && privkey.exists() {
            return Some(pubkey);
        }
    }

    None
}

/// Generate new SSH key pair
pub fn generate_ssh_key() -> Result<PathBuf, String> {
    let home = dirs::home_dir()
        .ok_or("Cannot determine home directory")?;
    let ssh_dir = home.join(".ssh");
    let key_path = ssh_dir.join("ableton_move");
    let pubkey_path = ssh_dir.join("ableton_move.pub");

    // Ensure .ssh directory exists
    fs::create_dir_all(&ssh_dir)
        .map_err(|e| format!("Failed to create .ssh directory: {}", e))?;

    // Set .ssh directory permissions (0700 on Unix)
    #[cfg(unix)]
    {
        let mut perms = fs::metadata(&ssh_dir)
            .map_err(|e| format!("Failed to read .ssh permissions: {}", e))?
            .permissions();
        perms.set_mode(0o700);
        fs::set_permissions(&ssh_dir, perms)
            .map_err(|e| format!("Failed to set .ssh permissions: {}", e))?;
    }

    // Generate key
    let paths = get_ssh_paths();
    let output = Command::new(&paths.ssh_keygen)
        .args(&[
            "-t", "ed25519",
            "-N", "",  // No passphrase
            "-f", key_path.to_str().unwrap(),
            "-C", &format!("move-installer@{}", hostname::get().unwrap_or_default().to_string_lossy()),
        ])
        .output()
        .map_err(|e| format!("Failed to run ssh-keygen: {}", e))?;

    if !output.status.success() {
        return Err(format!("ssh-keygen failed: {}", String::from_utf8_lossy(&output.stderr)));
    }

    // Set private key permissions (0600 on Unix)
    #[cfg(unix)]
    {
        let mut perms = fs::metadata(&key_path)
            .map_err(|e| format!("Failed to read key permissions: {}", e))?
            .permissions();
        perms.set_mode(0o600);
        fs::set_permissions(&key_path, perms)
            .map_err(|e| format!("Failed to set key permissions: {}", e))?;
    }

    // TODO: Set ACLs on Windows
    #[cfg(target_os = "windows")]
    {
        // Best-effort Windows ACL setting
        // Use winapi to restrict to current user
    }

    Ok(pubkey_path)
}

/// Read public key file
pub fn read_pubkey(path: &Path) -> Result<String, String> {
    fs::read_to_string(path)
        .map_err(|e| format!("Failed to read public key: {}", e))
}

/// Test SSH connectivity to Move
pub fn test_ssh_connection(hostname: &str) -> Result<bool, String> {
    let paths = get_ssh_paths();

    let output = Command::new(&paths.ssh)
        .args(&[
            "-o", "ConnectTimeout=3",
            "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "LogLevel=ERROR",
            &format!("ableton@{}", hostname),
            "true",
        ])
        .output()
        .map_err(|e| format!("Failed to run ssh: {}", e))?;

    Ok(output.status.success())
}

/// Write SSH config entry for Move
pub fn write_ssh_config() -> Result<(), String> {
    let home = dirs::home_dir()
        .ok_or("Cannot determine home directory")?;
    let config_path = home.join(".ssh/config");

    let config_entry = r#"
# Added by Move Everything Installer
Host move
  HostName move.local
  User ableton
  IdentityFile ~/.ssh/ableton_move
  IdentitiesOnly yes
"#;

    // Check if entry already exists
    if config_path.exists() {
        let content = fs::read_to_string(&config_path)
            .map_err(|e| format!("Failed to read SSH config: {}", e))?;

        if content.contains("Host move") {
            // Entry already exists
            return Ok(());
        }
    }

    // Append entry
    let mut file = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&config_path)
        .map_err(|e| format!("Failed to open SSH config: {}", e))?;

    file.write_all(config_entry.as_bytes())
        .map_err(|e| format!("Failed to write SSH config: {}", e))?;

    Ok(())
}
