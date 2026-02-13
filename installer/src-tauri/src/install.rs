use serde::{Deserialize, Serialize};
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Release {
    pub tag_name: String,
    pub name: String,
    pub assets: Vec<Asset>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Asset {
    pub name: String,
    pub browser_download_url: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Module {
    pub id: String,
    pub name: String,
    pub description: String,
    pub author: String,
    pub component_type: String,
    pub github_repo: String,
    pub asset_name: String,
    pub min_host_version: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub requires: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ModuleCatalog {
    catalog_version: u32,
    host: HostInfo,
    modules: Vec<Module>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct HostInfo {
    name: String,
    github_repo: String,
    asset_name: String,
    latest_version: String,
    download_url: String,
    min_host_version: String,
}

/// Fetch latest Move Anything release from GitHub (only v* tags, skips installer-* tags)
pub async fn fetch_latest_release() -> Result<Release, String> {
    let url = "https://api.github.com/repos/charlesvestal/move-anything/releases?per_page=10";

    let client = reqwest::Client::new();
    let response = client
        .get(url)
        .header("User-Agent", "move-installer")
        .send()
        .await
        .map_err(|e| format!("Failed to fetch releases: {}", e))?;

    if !response.status().is_success() {
        return Err(format!("GitHub API error: {}", response.status()));
    }

    let releases: Vec<Release> = response
        .json()
        .await
        .map_err(|e| format!("Failed to parse releases JSON: {}", e))?;

    // Find the first release whose tag starts with "v" (skip installer-* etc.)
    releases
        .into_iter()
        .find(|r| r.tag_name.starts_with('v'))
        .ok_or_else(|| "No v* release found".to_string())
}

/// Fetch module catalog from GitHub
pub async fn fetch_module_catalog() -> Result<Vec<Module>, String> {
    let url = "https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json";

    let client = reqwest::Client::new();
    let response = client
        .get(url)
        .header("User-Agent", "move-installer")
        .send()
        .await
        .map_err(|e| format!("Failed to fetch catalog: {}", e))?;

    if !response.status().is_success() {
        return Err(format!("Failed to fetch catalog: {}", response.status()));
    }

    let catalog: ModuleCatalog = response
        .json()
        .await
        .map_err(|e| format!("Failed to parse catalog JSON: {}", e))?;

    Ok(catalog.modules)
}

/// Download file from URL to local path
pub async fn download_file(url: &str, dest_path: &Path) -> Result<(), String> {
    let client = reqwest::Client::new();
    let response = client
        .get(url)
        .header("User-Agent", "move-installer")
        .send()
        .await
        .map_err(|e| format!("Download failed: {}", e))?;

    if !response.status().is_success() {
        return Err(format!("Download error: {}", response.status()));
    }

    let bytes = response
        .bytes()
        .await
        .map_err(|e| format!("Failed to read response: {}", e))?;

    // Ensure parent directory exists
    if let Some(parent) = dest_path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Failed to create directory: {}", e))?;
    }

    std::fs::write(dest_path, bytes)
        .map_err(|e| format!("Failed to write file: {}", e))?;

    Ok(())
}

/// Upload file to Move via SCP
pub fn scp_upload(local_path: &Path, remote_path: &str, hostname: &str) -> Result<(), String> {
    let paths = crate::ssh::get_ssh_paths();

    let remote = format!("ableton@{}:{}", hostname, remote_path);

    let output = Command::new(&paths.scp)
        .args(&[
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "LogLevel=ERROR",
            local_path.to_str().unwrap(),
            &remote,
        ])
        .output()
        .map_err(|e| format!("Failed to run scp: {}", e))?;

    if !output.status.success() {
        return Err(format!("scp failed: {}", String::from_utf8_lossy(&output.stderr)));
    }

    Ok(())
}

/// Execute SSH command on Move
pub fn ssh_exec(hostname: &str, command: &str) -> Result<String, String> {
    let paths = crate::ssh::get_ssh_paths();

    let output = Command::new(&paths.ssh)
        .args(&[
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "LogLevel=ERROR",
            &format!("ableton@{}", hostname),
            command,
        ])
        .output()
        .map_err(|e| format!("Failed to run ssh: {}", e))?;

    if !output.status.success() {
        return Err(format!("SSH command failed: {}", String::from_utf8_lossy(&output.stderr)));
    }

    Ok(String::from_utf8_lossy(&output.stdout).to_string())
}

/// Validate tarball structure
pub fn validate_tarball(tarball_path: &Path) -> Result<(), String> {
    // Check if file exists
    if !tarball_path.exists() {
        return Err(format!("Tarball not found: {}", tarball_path.display()));
    }

    // Check if it's a valid tar.gz file by attempting to list contents
    let output = Command::new("tar")
        .args(&["-tzf", tarball_path.to_str().unwrap()])
        .output()
        .map_err(|e| format!("Failed to validate tarball: {}", e))?;

    if !output.status.success() {
        return Err("Invalid tarball format".to_string());
    }

    Ok(())
}

/// Install main Move Anything package
pub async fn install_main_package(
    tarball_path: &Path,
    hostname: &str,
    progress_callback: Option<Box<dyn Fn(String) + Send>>,
) -> Result<(), String> {
    let send_progress = |msg: &str| {
        if let Some(ref cb) = progress_callback {
            cb(msg.to_string());
        }
    };

    send_progress("Validating tarball...");
    validate_tarball(tarball_path)?;

    send_progress("Uploading to Move...");
    scp_upload(tarball_path, "/tmp/move-anything.tar.gz", hostname)?;

    send_progress("Creating backup of original Move binary...");
    ssh_exec(hostname, "sudo cp /opt/move/Move /opt/move/MoveOriginal 2>/dev/null || true")?;

    send_progress("Extracting files...");
    ssh_exec(hostname, "cd /tmp && tar -xzf move-anything.tar.gz")?;

    send_progress("Installing Move Everything...");
    ssh_exec(hostname, "sudo /tmp/move-anything/install.sh")?;

    send_progress("Cleaning up...");
    ssh_exec(hostname, "rm -rf /tmp/move-anything /tmp/move-anything.tar.gz")?;

    send_progress("Installation complete!");
    Ok(())
}

/// Install external module
pub async fn install_module(
    module_id: &str,
    tarball_path: &Path,
    component_type: &str,
    hostname: &str,
    progress_callback: Option<Box<dyn Fn(String) + Send>>,
) -> Result<(), String> {
    let send_progress = |msg: &str| {
        if let Some(ref cb) = progress_callback {
            cb(msg.to_string());
        }
    };

    send_progress(&format!("Validating {} tarball...", module_id));
    validate_tarball(tarball_path)?;

    send_progress(&format!("Uploading {} to Move...", module_id));
    let remote_tarball = format!("/tmp/{}-module.tar.gz", module_id);
    scp_upload(tarball_path, &remote_tarball, hostname)?;

    send_progress(&format!("Extracting {}...", module_id));
    ssh_exec(hostname, &format!("cd /tmp && tar -xzf {}", remote_tarball))?;

    // Determine module install directory based on component_type
    let module_subdir = match component_type {
        "sound_generator" => "sound_generators",
        "audio_fx" => "audio_fx",
        "midi_fx" => "midi_fx",
        "utility" => "utilities",
        "overtake" => "overtake",
        "featured" => "", // featured modules go in root (like chain)
        "system" => "",   // system modules go in root (like store)
        _ => "other",     // fallback
    };

    let base_path = "/data/UserData/move-anything/modules";
    let install_path = if module_subdir.is_empty() {
        format!("{}/{}", base_path, module_id)
    } else {
        format!("{}/{}/{}", base_path, module_subdir, module_id)
    };

    send_progress(&format!("Installing {} to {}...", module_id, install_path));

    // Create parent directory if needed
    if !module_subdir.is_empty() {
        ssh_exec(hostname, &format!("mkdir -p {}/{}", base_path, module_subdir))?;
    }

    // Remove old version if exists
    ssh_exec(hostname, &format!("rm -rf {}", install_path))?;

    // Move module to install location
    ssh_exec(hostname, &format!("mv /tmp/{} {}", module_id, install_path))?;

    send_progress(&format!("Cleaning up {}...", module_id));
    ssh_exec(hostname, &format!("rm -f {}", remote_tarball))?;

    send_progress(&format!("{} installed successfully!", module_id));
    Ok(())
}
