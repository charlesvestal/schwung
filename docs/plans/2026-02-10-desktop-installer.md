# Desktop Installer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a cross-platform desktop installer (macOS/Windows) using Tauri that automates SSH setup and Move Everything installation.

**Architecture:** Tauri app with Rust backend for system operations (SSH, HTTP, file management) and web frontend for UI. Backend exposes IPC commands for device discovery, authentication, SSH key management, and installation. Frontend manages multi-step state machine.

**Tech Stack:** Tauri, Rust (reqwest, mdns-sd, keyring), HTML/CSS/JS frontend, bundled OpenSSH binaries for Windows

---

## Prerequisites: Phase 1 - API Discovery

**This phase must be completed manually before implementation begins.**

### Task 1.1: Reverse-Engineer Move Auth API

**Manual Steps:**

1. **Open browser DevTools** (Network tab)
2. **Navigate to** `http://move.local/`
3. **Document code prompt:**
   - Capture initial request/response
   - Note: URL, HTTP method, response body structure
   - Screenshot the code entry form

4. **Enter 6-digit code** from Move screen
5. **Document code submission:**
   - Capture POST request in Network tab
   - Note: Endpoint URL (e.g., `/auth`, `/verify`, etc.)
   - Note: Request payload format (JSON? Form data? Query params?)
   - Note: Response status code and headers
   - Capture `Set-Cookie` header with `Ableton-Challenge-Response-Token`

6. **Document cookie details:**
   - Cookie name: `Ableton-Challenge-Response-Token`
   - Cookie value format
   - Path attribute
   - Expiry time
   - Other attributes (HttpOnly, Secure, SameSite)

7. **Test authenticated key submission:**
   - With cookie in place, open `/development/ssh`
   - Paste SSH public key and submit
   - Capture POST request in Network tab
   - Note: Request payload format (key parameter name, content-type)
   - Note: Required headers beyond Cookie
   - Note: Success response (status code, body)

8. **Test unauthenticated key submission:**
   - Clear cookies in DevTools
   - Try POST to `/development/ssh` without auth
   - Confirm 401/403 response
   - Document error response format

**Create API Documentation:**

Create file: `docs/move-auth-api.md`

```markdown
# Move Authentication API

## Code Submission

**Endpoint:** `POST http://move.local/[ENDPOINT]`

**Request:**
```http
[Captured request format]
```

**Response (Success):**
```http
HTTP/1.1 200 OK
Set-Cookie: Ableton-Challenge-Response-Token=[value]; Path=/; Expires=[date]
```

**Response (Invalid Code):**
```http
[Captured error response]
```

## SSH Key Submission

**Endpoint:** `POST http://move.local/development/ssh`

**Request:**
```http
[Captured request format with cookie]
```

**Response (Success):**
```http
[Captured success response]
```

**Response (Unauthorized):**
```http
HTTP/1.1 401 Unauthorized
```

## Example curl Commands

```bash
# Submit code
curl -v [captured command]

# Submit SSH key
curl -v [captured command with cookie]
```
```

**Verification:** Run curl commands to confirm API documentation is accurate.

**Commit:**
```bash
git add docs/move-auth-api.md
git commit -m "docs: document Move authentication API"
```

---

## Phase 2: Tauri Project Setup

### Task 2.1: Initialize Tauri Project

**Files:**
- Create: `installer/` (new directory in repo root)
- Create: `installer/.gitignore`
- Create: `installer/package.json`
- Create: `installer/tauri.conf.json`

**Step 1: Create project directory**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything
mkdir installer
cd installer
```

**Step 2: Initialize npm project**

```bash
npm init -y
```

**Step 3: Install Tauri CLI**

```bash
npm install --save-dev @tauri-apps/cli
```

**Step 4: Initialize Tauri**

```bash
npx tauri init
```

Configuration prompts:
- App name: `Move Everything Installer`
- Window title: `Move Everything Installer`
- Web assets: `./ui`
- Dev server URL: `http://localhost:3000`
- Frontend dev command: (leave empty for now)
- Frontend build command: (leave empty for now)

**Step 5: Verify structure**

Expected directory structure:
```
installer/
  src-tauri/
    src/
      main.rs
    Cargo.toml
    tauri.conf.json
  ui/
    index.html
  package.json
```

**Step 6: Update .gitignore**

Create: `installer/.gitignore`
```
node_modules/
dist/
target/
.DS_Store
```

**Step 7: Test build**

```bash
npm run tauri build
```

Expected: Build succeeds, creates app bundle

**Step 8: Commit**

```bash
git add installer/
git commit -m "feat: initialize Tauri project for installer"
```

### Task 2.2: Configure Tauri Dependencies

**Files:**
- Modify: `installer/src-tauri/Cargo.toml`

**Step 1: Add Rust dependencies**

Edit `installer/src-tauri/Cargo.toml`:

```toml
[dependencies]
tauri = { version = "1.5", features = ["shell-open"] }
serde = { version = "1.0", features = ["derive"] }
serde_json = "1.0"
reqwest = { version = "0.11", features = ["cookies", "json"] }
tokio = { version = "1", features = ["full"] }
mdns-sd = "0.7"
keyring = "2.0"
dirs = "5.0"

[target.'cfg(unix)'.dependencies]
nix = { version = "0.27", features = ["fs"] }
```

**Step 2: Test build**

```bash
cd installer
cargo build
```

Expected: All dependencies compile successfully

**Step 3: Commit**

```bash
git add src-tauri/Cargo.toml
git commit -m "feat: add Rust dependencies for installer"
```

### Task 2.3: Bundle OpenSSH for Windows

**Files:**
- Create: `installer/src-tauri/resources/bin/` (Windows OpenSSH binaries)
- Modify: `installer/src-tauri/tauri.conf.json`

**Step 1: Download OpenSSH for Windows**

Download portable OpenSSH from: https://github.com/PowerShell/Win32-OpenSSH/releases

Extract: `ssh.exe`, `ssh-keygen.exe`, `scp.exe`, and required DLLs

**Step 2: Create resources directory**

```bash
mkdir -p src-tauri/resources/bin
```

**Step 3: Copy OpenSSH binaries**

Place in `src-tauri/resources/bin/`:
- `ssh.exe`
- `ssh-keygen.exe`
- `scp.exe`
- Required DLLs (typically in same archive)

**Step 4: Configure Tauri to bundle resources**

Edit `installer/src-tauri/tauri.conf.json`:

```json
{
  "tauri": {
    "bundle": {
      "resources": {
        "bin/*": "resources/bin/*"
      }
    }
  }
}
```

**Step 5: Add platform-specific build notes**

Create: `installer/README.md`

```markdown
# Move Everything Installer

## Building

### macOS
```bash
npm run tauri build
```

### Windows
Ensure OpenSSH binaries are in `src-tauri/resources/bin/` before building:
- ssh.exe
- ssh-keygen.exe
- scp.exe
- Required DLLs

```bash
npm run tauri build
```

## Development

```bash
npm run tauri dev
```
```

**Step 6: Commit**

```bash
git add src-tauri/resources/ src-tauri/tauri.conf.json installer/README.md
git commit -m "feat: bundle OpenSSH binaries for Windows"
```

---

## Phase 3: Backend Implementation

### Task 3.1: Device Discovery Module

**Files:**
- Create: `installer/src-tauri/src/device.rs`
- Modify: `installer/src-tauri/src/main.rs`

**Step 1: Write device discovery module**

Create: `installer/src-tauri/src/device.rs`

```rust
use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;

pub struct MoveDevice {
    pub hostname: String,
    pub ip: String,
}

/// Discover Move device on local network via mDNS
pub async fn discover_move() -> Result<MoveDevice, String> {
    let mdns = ServiceDaemon::new().map_err(|e| format!("Failed to start mDNS: {}", e))?;

    // Try to resolve move.local
    let receiver = mdns
        .browse("_http._tcp.local.")
        .map_err(|e| format!("Failed to browse: {}", e))?;

    // Wait up to 5 seconds for discovery
    let timeout = Duration::from_secs(5);
    let start = std::time::Instant::now();

    while start.elapsed() < timeout {
        if let Ok(event) = receiver.recv_timeout(Duration::from_millis(100)) {
            match event {
                ServiceEvent::ServiceResolved(info) => {
                    if info.get_hostname().contains("move") {
                        let ip = info.get_addresses().iter().next()
                            .ok_or("No IP address found")?
                            .to_string();
                        return Ok(MoveDevice {
                            hostname: "move.local".to_string(),
                            ip,
                        });
                    }
                }
                _ => {}
            }
        }
    }

    Err("Move device not found on network".to_string())
}

/// Validate device by checking HTTP endpoint
pub async fn validate_device(base_url: &str) -> Result<bool, String> {
    let client = reqwest::Client::new();
    match client.get(base_url).send().await {
        Ok(_) => Ok(true),
        Err(e) => Err(format!("Cannot reach device: {}", e)),
    }
}
```

**Step 2: Add IPC command for device discovery**

Modify: `installer/src-tauri/src/main.rs`

```rust
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

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![find_device, validate_device_at])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

**Step 3: Test compilation**

```bash
cargo build
```

Expected: Compiles without errors

**Step 4: Commit**

```bash
git add src-tauri/src/device.rs src-tauri/src/main.rs
git commit -m "feat: add device discovery module with mDNS"
```

### Task 3.2: HTTP Client with Cookie Management

**Files:**
- Create: `installer/src-tauri/src/auth.rs`
- Modify: `installer/src-tauri/src/main.rs`

**Step 1: Write authentication module**

Create: `installer/src-tauri/src/auth.rs`

```rust
use reqwest::{Client, cookie::Jar};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

const COOKIE_NAME: &str = "Ableton-Challenge-Response-Token";

#[derive(Debug, Serialize, Deserialize)]
pub struct AuthCookie {
    pub value: String,
    pub expires: String,
}

pub struct AuthClient {
    client: Client,
    base_url: String,
}

impl AuthClient {
    pub fn new(base_url: String) -> Self {
        let jar = Arc::new(Jar::default());
        let client = Client::builder()
            .cookie_provider(jar)
            .build()
            .unwrap();

        Self { client, base_url }
    }

    /// Submit 6-digit code and obtain auth cookie
    /// TODO: Update endpoint and request format based on API discovery
    pub async fn submit_code(&self, code: &str) -> Result<AuthCookie, String> {
        // Placeholder - update with actual endpoint from API discovery
        let endpoint = format!("{}/auth", self.base_url);

        let response = self.client
            .post(&endpoint)
            .form(&[("code", code)])  // Update format based on discovery
            .send()
            .await
            .map_err(|e| format!("Failed to submit code: {}", e))?;

        if !response.status().is_success() {
            return Err(format!("Invalid code: HTTP {}", response.status()));
        }

        // Extract cookie from response
        let cookies = response.cookies();
        for cookie in cookies {
            if cookie.name() == COOKIE_NAME {
                return Ok(AuthCookie {
                    value: cookie.value().to_string(),
                    expires: cookie.expires()
                        .map(|e| format!("{:?}", e))
                        .unwrap_or_default(),
                });
            }
        }

        Err("No auth cookie in response".to_string())
    }

    /// Submit SSH public key with authentication
    pub async fn submit_ssh_key(&self, pubkey: &str) -> Result<(), String> {
        let endpoint = format!("{}/development/ssh", self.base_url);

        let response = self.client
            .post(&endpoint)
            .form(&[("key", pubkey)])  // Update format based on discovery
            .send()
            .await
            .map_err(|e| format!("Failed to submit key: {}", e))?;

        match response.status().as_u16() {
            200 | 204 => Ok(()),
            401 | 403 => Err("Unauthorized - cookie expired or invalid".to_string()),
            400 => Err("Invalid key format".to_string()),
            code => Err(format!("Unexpected response: HTTP {}", code)),
        }
    }
}
```

**Step 2: Add cookie storage module**

Create: `installer/src-tauri/src/cookie_storage.rs`

```rust
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
```

**Step 3: Add IPC commands**

Modify: `installer/src-tauri/src/main.rs`

```rust
mod auth;
mod cookie_storage;

use auth::AuthClient;

#[tauri::command]
async fn submit_auth_code(base_url: String, code: String) -> Result<String, String> {
    let client = AuthClient::new(base_url);
    let cookie = client.submit_code(&code).await?;

    // Save cookie to keychain
    cookie_storage::save_cookie(&cookie.value)?;

    Ok(cookie.value)
}

#[tauri::command]
async fn submit_ssh_key_with_auth(base_url: String, pubkey: String) -> Result<(), String> {
    let client = AuthClient::new(base_url);

    // Try saved cookie first
    if let Some(saved_cookie) = cookie_storage::load_cookie()? {
        // TODO: Set cookie in client before request
        // This requires refactoring AuthClient to accept pre-set cookies
    }

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

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            find_device,
            validate_device_at,
            submit_auth_code,
            submit_ssh_key_with_auth,
            get_saved_cookie,
            clear_saved_cookie,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

**Step 4: Test compilation**

```bash
cargo build
```

Expected: Compiles without errors

**Step 5: Commit**

```bash
git add src-tauri/src/auth.rs src-tauri/src/cookie_storage.rs src-tauri/src/main.rs
git commit -m "feat: add HTTP client with cookie management and keychain storage"
```

**Note:** After Phase 1 API discovery, update `auth.rs` with actual endpoint URLs and request formats.

### Task 3.3: SSH Key Management Module

**Files:**
- Create: `installer/src-tauri/src/ssh.rs`
- Modify: `installer/src-tauri/src/main.rs`

**Step 1: Write SSH utilities module**

Create: `installer/src-tauri/src/ssh.rs`

```rust
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
```

**Step 2: Add hostname dependency**

Edit `installer/src-tauri/Cargo.toml`:

```toml
hostname = "0.3"
```

**Step 3: Add IPC commands**

Modify: `installer/src-tauri/src/main.rs`

```rust
mod ssh;

#[tauri::command]
fn find_existing_ssh_key() -> Option<String> {
    ssh::find_ssh_key()
        .and_then(|path| path.to_str().map(|s| s.to_string()))
}

#[tauri::command]
fn generate_new_ssh_key() -> Result<String, String> {
    let pubkey_path = ssh::generate_ssh_key()?;
    Ok(pubkey_path.to_str().unwrap().to_string())
}

#[tauri::command]
fn read_public_key(path: String) -> Result<String, String> {
    ssh::read_pubkey(Path::new(&path))
}

#[tauri::command]
fn test_ssh(hostname: String) -> Result<bool, String> {
    ssh::test_ssh_connection(&hostname)
}

#[tauri::command]
fn setup_ssh_config() -> Result<(), String> {
    ssh::write_ssh_config()
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            find_device,
            validate_device_at,
            submit_auth_code,
            submit_ssh_key_with_auth,
            get_saved_cookie,
            clear_saved_cookie,
            find_existing_ssh_key,
            generate_new_ssh_key,
            read_public_key,
            test_ssh,
            setup_ssh_config,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

**Step 4: Test compilation**

```bash
cargo build
```

Expected: Compiles without errors

**Step 5: Commit**

```bash
git add src-tauri/src/ssh.rs src-tauri/src/main.rs src-tauri/Cargo.toml
git commit -m "feat: add SSH key management module"
```

### Task 3.4: Installation Module

**Files:**
- Create: `installer/src-tauri/src/install.rs`
- Modify: `installer/src-tauri/src/main.rs`

**Step 1: Write installation module**

Create: `installer/src-tauri/src/install.rs`

```rust
use reqwest::Client;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::process::Command;
use std::fs;

#[derive(Debug, Serialize, Deserialize)]
pub struct Release {
    pub tag_name: String,
    pub name: String,
    pub download_url: String,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Module {
    pub id: String,
    pub name: String,
    pub description: String,
    pub component_type: String,
    pub github_repo: String,
    pub asset_name: String,
}

const GITHUB_API: &str = "https://api.github.com";
const MAIN_REPO: &str = "charlesvestal/move-anything";
const CATALOG_URL: &str = "https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json";

/// Fetch latest Move Everything release
pub async fn fetch_latest_release() -> Result<Release, String> {
    let client = Client::new();
    let url = format!("{}/repos/{}/releases/latest", GITHUB_API, MAIN_REPO);

    #[derive(Deserialize)]
    struct GithubRelease {
        tag_name: String,
        name: String,
        assets: Vec<GithubAsset>,
    }

    #[derive(Deserialize)]
    struct GithubAsset {
        name: String,
        browser_download_url: String,
    }

    let response = client
        .get(&url)
        .header("User-Agent", "move-installer")
        .send()
        .await
        .map_err(|e| format!("Failed to fetch release: {}", e))?;

    if !response.status().is_success() {
        return Err(format!("GitHub API error: HTTP {}", response.status()));
    }

    let release: GithubRelease = response.json().await
        .map_err(|e| format!("Failed to parse release: {}", e))?;

    let asset = release.assets.iter()
        .find(|a| a.name == "move-anything.tar.gz")
        .ok_or("Release missing move-anything.tar.gz asset")?;

    Ok(Release {
        tag_name: release.tag_name,
        name: release.name,
        download_url: asset.browser_download_url.clone(),
    })
}

/// Fetch module catalog
pub async fn fetch_module_catalog() -> Result<Vec<Module>, String> {
    let client = Client::new();

    let response = client
        .get(CATALOG_URL)
        .send()
        .await
        .map_err(|e| format!("Failed to fetch catalog: {}", e))?;

    #[derive(Deserialize)]
    struct Catalog {
        modules: Vec<Module>,
    }

    let catalog: Catalog = response.json().await
        .map_err(|e| format!("Failed to parse catalog: {}", e))?;

    Ok(catalog.modules)
}

/// Download file from URL
pub async fn download_file(url: &str, dest: &PathBuf) -> Result<(), String> {
    let client = Client::new();

    let response = client
        .get(url)
        .send()
        .await
        .map_err(|e| format!("Download failed: {}", e))?;

    if !response.status().is_success() {
        return Err(format!("Download failed: HTTP {}", response.status()));
    }

    let bytes = response.bytes().await
        .map_err(|e| format!("Failed to read response: {}", e))?;

    fs::write(dest, bytes)
        .map_err(|e| format!("Failed to write file: {}", e))?;

    Ok(())
}

/// Upload file to Move via SCP
pub fn scp_upload(local_path: &PathBuf, hostname: &str, remote_path: &str) -> Result<(), String> {
    let paths = crate::ssh::get_ssh_paths();

    let output = Command::new(&paths.scp)
        .args(&[
            "-o", "ConnectTimeout=5",
            "-o", "StrictHostKeyChecking=accept-new",
            local_path.to_str().unwrap(),
            &format!("ableton@{}:{}", hostname, remote_path),
        ])
        .output()
        .map_err(|e| format!("Failed to run scp: {}", e))?;

    if !output.status.success() {
        return Err(format!("SCP failed: {}", String::from_utf8_lossy(&output.stderr)));
    }

    Ok(())
}

/// Run SSH command on Move
pub fn ssh_exec(hostname: &str, command: &str) -> Result<String, String> {
    let paths = crate::ssh::get_ssh_paths();

    let output = Command::new(&paths.ssh)
        .args(&[
            "-o", "ConnectTimeout=5",
            "-o", "StrictHostKeyChecking=accept-new",
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

/// Validate tarball structure on device
pub fn validate_tarball(hostname: &str, tarball: &str, expected_file: &str) -> Result<(), String> {
    let command = format!("tar -tzf {} | grep -qx '{}'", tarball, expected_file);
    ssh_exec(hostname, &command)
        .map(|_| ())
}

/// Install Move Everything to device
pub async fn install_main_package(hostname: &str, local_tarball: &PathBuf) -> Result<(), String> {
    // Upload tarball
    scp_upload(local_tarball, hostname, "./move-anything.tar.gz")?;

    // Validate tarball structure
    validate_tarball(hostname, "move-anything.tar.gz", "move-anything/move-anything-shim.so")?;

    // Extract
    ssh_exec(hostname, "tar -xzf ./move-anything.tar.gz")?;

    // Verify payload
    ssh_exec(hostname, "test -f /data/UserData/move-anything/move-anything-shim.so")?;
    ssh_exec(hostname, "test -f /data/UserData/move-anything/shim-entrypoint.sh")?;

    // Run installation steps (simplified - full logic from install.sh)
    // TODO: Port remaining install.sh logic

    Ok(())
}

/// Install module to device
pub async fn install_module(hostname: &str, module: &Module, local_tarball: &PathBuf) -> Result<(), String> {
    // Upload module tarball
    let remote_path = format!("./{}", module.asset_name);
    scp_upload(local_tarball, hostname, &remote_path)?;

    // Validate module structure
    let expected_file = format!("{}/module.json", module.id);
    validate_tarball(hostname, &module.asset_name, &expected_file)?;

    // Determine destination based on component_type
    let dest_dir = match module.component_type.as_str() {
        "sound_generator" => "modules/sound_generators",
        "audio_fx" => "modules/audio_fx",
        "midi_fx" => "modules/midi_fx",
        "utility" => "modules/utilities",
        "overtake" => "modules/overtake",
        _ => "modules/other",
    };

    // Extract to appropriate directory
    let extract_cmd = format!(
        "cd move-anything && mkdir -p {} && tar -xzf ../{} -C {}/",
        dest_dir, module.asset_name, dest_dir
    );
    ssh_exec(hostname, &extract_cmd)?;

    // Clean up tarball
    ssh_exec(hostname, &format!("rm {}", remote_path))?;

    Ok(())
}
```

**Step 2: Add IPC commands**

Modify: `installer/src-tauri/src/main.rs`

```rust
mod install;

use install::{Release, Module};

#[tauri::command]
async fn get_latest_release() -> Result<Release, String> {
    install::fetch_latest_release().await
}

#[tauri::command]
async fn get_module_catalog() -> Result<Vec<Module>, String> {
    install::fetch_module_catalog().await
}

#[tauri::command]
async fn download_release(url: String, dest_path: String) -> Result<(), String> {
    install::download_file(&url, &PathBuf::from(dest_path)).await
}

#[tauri::command]
async fn install_main(hostname: String, tarball_path: String) -> Result<(), String> {
    install::install_main_package(&hostname, &PathBuf::from(tarball_path)).await
}

#[tauri::command]
async fn install_module_package(hostname: String, module: Module, tarball_path: String) -> Result<(), String> {
    install::install_module(&hostname, &module, &PathBuf::from(tarball_path)).await
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            find_device,
            validate_device_at,
            submit_auth_code,
            submit_ssh_key_with_auth,
            get_saved_cookie,
            clear_saved_cookie,
            find_existing_ssh_key,
            generate_new_ssh_key,
            read_public_key,
            test_ssh,
            setup_ssh_config,
            get_latest_release,
            get_module_catalog,
            download_release,
            install_main,
            install_module_package,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

**Step 3: Test compilation**

```bash
cargo build
```

Expected: Compiles without errors

**Step 4: Commit**

```bash
git add src-tauri/src/install.rs src-tauri/src/main.rs
git commit -m "feat: add installation module for Move Everything and modules"
```

---

## Phase 4: Frontend Implementation

### Task 4.1: Basic UI Structure

**Files:**
- Modify: `installer/ui/index.html`
- Create: `installer/ui/style.css`
- Create: `installer/ui/app.js`

**Step 1: Create HTML structure**

Edit: `installer/ui/index.html`

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Move Everything Installer</title>
    <link rel="stylesheet" href="style.css">
</head>
<body>
    <div id="app">
        <!-- Screen: Device Discovery -->
        <div id="screen-discovery" class="screen">
            <h1>Finding your Move...</h1>
            <div class="spinner"></div>
            <p id="discovery-status">Searching on local network...</p>
            <div id="manual-ip-section" style="display: none;">
                <p>Can't find it?</p>
                <input type="text" id="manual-ip" placeholder="Enter IP address (e.g., 192.168.1.100)">
                <button id="btn-manual-connect">Connect</button>
            </div>
        </div>

        <!-- Screen: Code Entry -->
        <div id="screen-code" class="screen hidden">
            <h1>Enter code from Move screen</h1>
            <input type="text" id="code-input" maxlength="6" placeholder="_ _ _ _ _ _">
            <button id="btn-submit-code">Submit</button>
            <p id="code-error" class="error"></p>
        </div>

        <!-- Screen: Confirm on Device -->
        <div id="screen-confirm" class="screen hidden">
            <h1>Confirm SSH key on Move</h1>
            <div class="spinner"></div>
            <p>Waiting for confirmation...</p>
            <button id="btn-retry-confirm">Retry</button>
        </div>

        <!-- Screen: Module Selection -->
        <div id="screen-modules" class="screen hidden">
            <h1>Choose modules to install</h1>
            <div class="radio-group">
                <label><input type="radio" name="module-mode" value="all"> Install All</label>
                <label><input type="radio" name="module-mode" value="none"> Skip All</label>
                <label><input type="radio" name="module-mode" value="choose" checked> Choose Modules</label>
            </div>
            <div id="module-list"></div>
            <button id="btn-install">Install Selected</button>
        </div>

        <!-- Screen: Installing -->
        <div id="screen-installing" class="screen hidden">
            <h1>Installing Move Everything...</h1>
            <div id="install-progress"></div>
            <progress id="progress-bar" value="0" max="100"></progress>
        </div>

        <!-- Screen: Success -->
        <div id="screen-success" class="screen hidden">
            <h1>✓ Installation Complete!</h1>
            <div class="info-box">
                <h3>Connect via SSH:</h3>
                <code id="ssh-command">ssh ableton@move.local</code>
                <button id="btn-copy-ssh">Copy SSH Command</button>
            </div>
            <div class="info-box">
                <h3>SFTP access:</h3>
                <code id="sftp-command">sftp ableton@move.local</code>
                <button id="btn-copy-sftp">Copy SFTP Command</button>
            </div>
            <button id="btn-clear-credentials">Clear Saved Credentials</button>
            <button id="btn-close">Close</button>
        </div>

        <!-- Screen: Error -->
        <div id="screen-error" class="screen hidden">
            <h1>Error</h1>
            <p id="error-message"></p>
            <button id="btn-retry">Retry</button>
            <button id="btn-diagnostics">Copy Diagnostics</button>
        </div>
    </div>

    <script src="app.js"></script>
</body>
</html>
```

**Step 2: Create CSS**

Create: `installer/ui/style.css`

```css
* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: #1a1a1a;
    color: #ffffff;
    display: flex;
    justify-content: center;
    align-items: center;
    min-height: 100vh;
}

#app {
    width: 500px;
    padding: 40px;
}

.screen {
    text-align: center;
}

.screen.hidden {
    display: none;
}

h1 {
    font-size: 24px;
    margin-bottom: 30px;
}

.spinner {
    width: 50px;
    height: 50px;
    border: 4px solid #333;
    border-top-color: #fff;
    border-radius: 50%;
    animation: spin 1s linear infinite;
    margin: 20px auto;
}

@keyframes spin {
    to { transform: rotate(360deg); }
}

input[type="text"] {
    width: 100%;
    padding: 12px;
    font-size: 18px;
    background: #2a2a2a;
    border: 1px solid #444;
    color: #fff;
    border-radius: 4px;
    margin: 20px 0;
    text-align: center;
}

button {
    background: #007aff;
    color: white;
    border: none;
    padding: 12px 24px;
    font-size: 16px;
    border-radius: 4px;
    cursor: pointer;
    margin: 10px 5px;
}

button:hover {
    background: #0051d5;
}

.error {
    color: #ff3b30;
    margin: 10px 0;
}

.info-box {
    background: #2a2a2a;
    padding: 20px;
    border-radius: 8px;
    margin: 20px 0;
}

code {
    background: #1a1a1a;
    padding: 10px;
    border-radius: 4px;
    display: block;
    margin: 10px 0;
    font-family: "Monaco", "Courier New", monospace;
}

progress {
    width: 100%;
    height: 20px;
    margin: 20px 0;
}

.radio-group {
    margin: 20px 0;
}

.radio-group label {
    display: block;
    padding: 10px;
    margin: 5px 0;
}

#module-list {
    max-height: 300px;
    overflow-y: auto;
    text-align: left;
    padding: 20px;
    background: #2a2a2a;
    border-radius: 8px;
    margin: 20px 0;
}

.module-category {
    margin: 20px 0;
}

.module-category h3 {
    font-size: 16px;
    color: #999;
    margin-bottom: 10px;
}

.module-item {
    padding: 10px;
    margin: 5px 0;
}

.module-item label {
    cursor: pointer;
}
```

**Step 3: Create JavaScript application logic**

Create: `installer/ui/app.js`

```javascript
const { invoke } = window.__TAURI__.tauri;

// Application state
const state = {
    device: null,
    sshKeyPath: null,
    selectedModules: [],
};

// Screen management
function showScreen(screenId) {
    document.querySelectorAll('.screen').forEach(s => s.classList.add('hidden'));
    document.getElementById(screenId).classList.remove('hidden');
}

function showError(message) {
    document.getElementById('error-message').textContent = message;
    showScreen('screen-error');
}

// Device discovery
async function discoverDevice() {
    showScreen('screen-discovery');

    try {
        state.device = await invoke('find_device');
        console.log('Found device:', state.device);
        await checkSSH();
    } catch (error) {
        console.error('Discovery failed:', error);
        document.getElementById('manual-ip-section').style.display = 'block';
    }
}

// Manual IP connection
document.getElementById('btn-manual-connect')?.addEventListener('click', async () => {
    const ip = document.getElementById('manual-ip').value.trim();
    if (!ip) return;

    try {
        const baseUrl = `http://${ip}`;
        const valid = await invoke('validate_device_at', { baseUrl });
        if (valid) {
            state.device = { hostname: 'move.local', ip };
            await checkSSH();
        } else {
            showError('Not a valid Move device at this IP');
        }
    } catch (error) {
        showError(`Cannot reach device: ${error}`);
    }
});

// Check SSH connection
async function checkSSH() {
    try {
        const works = await invoke('test_ssh', { hostname: state.device.hostname });
        if (works) {
            // SSH already works - skip to installation
            await startInstallation();
            return;
        }
    } catch (error) {
        console.log('SSH not working, need to set up:', error);
    }

    // Need to set up SSH
    await setupSSH();
}

// SSH setup flow
async function setupSSH() {
    // Find or generate key
    let keyPath = await invoke('find_existing_ssh_key');
    if (!keyPath) {
        keyPath = await invoke('generate_new_ssh_key');
    }
    state.sshKeyPath = keyPath;

    // Try saved cookie first
    try {
        const savedCookie = await invoke('get_saved_cookie');
        if (savedCookie) {
            const pubkey = await invoke('read_public_key', { path: keyPath });
            await invoke('submit_ssh_key_with_auth', {
                baseUrl: `http://${state.device.ip}`,
                pubkey,
            });
            await waitForSSHConfirmation();
            return;
        }
    } catch (error) {
        console.log('Saved cookie not valid, need code:', error);
    }

    // Need code entry
    showCodeEntry();
}

// Code entry
function showCodeEntry() {
    showScreen('screen-code');
    document.getElementById('code-input').focus();
}

document.getElementById('btn-submit-code')?.addEventListener('click', async () => {
    const code = document.getElementById('code-input').value.trim();
    if (code.length !== 6) {
        document.getElementById('code-error').textContent = 'Enter 6-digit code';
        return;
    }

    try {
        const baseUrl = `http://${state.device.ip}`;
        await invoke('submit_auth_code', { baseUrl, code });

        // Submit SSH key
        const pubkey = await invoke('read_public_key', { path: state.sshKeyPath });
        await invoke('submit_ssh_key_with_auth', { baseUrl, pubkey });

        await waitForSSHConfirmation();
    } catch (error) {
        document.getElementById('code-error').textContent = `Error: ${error}`;
    }
});

// Wait for on-device confirmation
async function waitForSSHConfirmation() {
    showScreen('screen-confirm');

    // Poll SSH connection
    let attempts = 0;
    const maxAttempts = 60; // 60 seconds

    const poll = setInterval(async () => {
        try {
            const works = await invoke('test_ssh', { hostname: state.device.hostname });
            if (works) {
                clearInterval(poll);
                await invoke('setup_ssh_config');
                await startInstallation();
            }
        } catch (error) {
            console.log('SSH not ready yet:', error);
        }

        attempts++;
        if (attempts >= maxAttempts) {
            clearInterval(poll);
            showError('Timeout waiting for confirmation. Please check your Move.');
        }
    }, 1000);
}

document.getElementById('btn-retry-confirm')?.addEventListener('click', waitForSSHConfirmation);

// Installation
async function startInstallation() {
    // Fetch module catalog
    try {
        const modules = await invoke('get_module_catalog');
        showModuleSelection(modules);
    } catch (error) {
        showError(`Failed to fetch module catalog: ${error}`);
    }
}

function showModuleSelection(modules) {
    showScreen('screen-modules');

    // Group by category
    const byCategory = {};
    modules.forEach(mod => {
        const cat = mod.component_type || 'other';
        if (!byCategory[cat]) byCategory[cat] = [];
        byCategory[cat].push(mod);
    });

    const categoryNames = {
        sound_generator: 'Sound Generators',
        audio_fx: 'Audio Effects',
        midi_fx: 'MIDI Effects',
        utility: 'Utilities',
        overtake: 'Overtake Modules',
    };

    const list = document.getElementById('module-list');
    list.innerHTML = '';

    Object.entries(byCategory).forEach(([cat, mods]) => {
        const catDiv = document.createElement('div');
        catDiv.className = 'module-category';
        catDiv.innerHTML = `<h3>${categoryNames[cat] || cat}</h3>`;

        mods.forEach(mod => {
            const item = document.createElement('div');
            item.className = 'module-item';
            item.innerHTML = `
                <label>
                    <input type="checkbox" value="${mod.id}" checked>
                    ${mod.name} - ${mod.description}
                </label>
            `;
            catDiv.appendChild(item);
        });

        list.appendChild(catDiv);
    });

    state.allModules = modules;
}

document.getElementById('btn-install')?.addEventListener('click', async () => {
    const mode = document.querySelector('input[name="module-mode"]:checked').value;

    let selectedModules = [];
    if (mode === 'all') {
        selectedModules = state.allModules;
    } else if (mode === 'choose') {
        const checked = document.querySelectorAll('#module-list input:checked');
        const selectedIds = Array.from(checked).map(cb => cb.value);
        selectedModules = state.allModules.filter(m => selectedIds.includes(m.id));
    }

    await performInstallation(selectedModules);
});

async function performInstallation(modules) {
    showScreen('screen-installing');
    const progress = document.getElementById('install-progress');
    const bar = document.getElementById('progress-bar');

    try {
        // Download main package
        progress.textContent = 'Downloading Move Everything...';
        const release = await invoke('get_latest_release');
        const mainTarball = `/tmp/move-anything.tar.gz`; // TODO: Use proper temp directory
        await invoke('download_release', { url: release.download_url, destPath: mainTarball });

        // Install main package
        progress.textContent = 'Installing Move Everything...';
        bar.value = 10;
        await invoke('install_main', { hostname: state.device.hostname, tarballPath: mainTarball });
        bar.value = 30;

        // Install modules
        const totalModules = modules.length;
        for (let i = 0; i < totalModules; i++) {
            const mod = modules[i];
            progress.textContent = `Installing ${mod.name}... (${i + 1}/${totalModules})`;

            // Download module
            const moduleUrl = `https://github.com/${mod.github_repo}/releases/latest/download/${mod.asset_name}`;
            const moduleTarball = `/tmp/${mod.asset_name}`;
            await invoke('download_release', { url: moduleUrl, destPath: moduleTarball });

            // Install module
            await invoke('install_module_package', {
                hostname: state.device.hostname,
                module: mod,
                tarballPath: moduleTarball,
            });

            bar.value = 30 + ((i + 1) / totalModules) * 60;
        }

        progress.textContent = 'Restarting Move...';
        // TODO: Restart Move via SSH
        bar.value = 100;

        showSuccess();
    } catch (error) {
        showError(`Installation failed: ${error}`);
    }
}

function showSuccess() {
    showScreen('screen-success');
    document.getElementById('ssh-command').textContent = `ssh ableton@${state.device.hostname}`;
    document.getElementById('sftp-command').textContent = `sftp ableton@${state.device.hostname}`;
}

document.getElementById('btn-copy-ssh')?.addEventListener('click', () => {
    navigator.clipboard.writeText(document.getElementById('ssh-command').textContent);
});

document.getElementById('btn-copy-sftp')?.addEventListener('click', () => {
    navigator.clipboard.writeText(document.getElementById('sftp-command').textContent);
});

document.getElementById('btn-clear-credentials')?.addEventListener('click', async () => {
    try {
        await invoke('clear_saved_cookie');
        alert('Credentials cleared');
    } catch (error) {
        alert(`Failed to clear credentials: ${error}`);
    }
});

document.getElementById('btn-close')?.addEventListener('click', () => {
    window.close();
});

// Start app
window.addEventListener('DOMContentLoaded', () => {
    discoverDevice();
});
```

**Step 4: Test in development**

```bash
cd installer
npm run tauri dev
```

Expected: App launches, shows device discovery screen

**Step 5: Commit**

```bash
git add ui/
git commit -m "feat: add frontend UI with multi-screen flow"
```

---

## Phase 5: Integration & Polish

### Task 5.1: Update API Module with Discovered Endpoints

**After completing Phase 1 API discovery, update the auth module.**

**Files:**
- Modify: `installer/src-tauri/src/auth.rs`

**Step 1: Update endpoint URLs and request formats**

Based on `docs/move-auth-api.md`, update `submit_code()` and `submit_ssh_key()` methods with actual:
- Endpoint URLs
- Request body formats (JSON vs form data)
- Header requirements

**Step 2: Test against real device**

Run installer and verify auth flow works end-to-end.

**Step 3: Commit**

```bash
git add src-tauri/src/auth.rs
git commit -m "fix: update auth module with discovered API endpoints"
```

### Task 5.2: Error Handling & Diagnostics

**Files:**
- Create: `installer/src-tauri/src/diagnostics.rs`
- Modify: `installer/ui/app.js`

**Step 1: Add diagnostics module**

Create: `installer/src-tauri/src/diagnostics.rs`

```rust
use serde::Serialize;
use std::time::SystemTime;

#[derive(Serialize)]
pub struct DiagnosticReport {
    pub timestamp: String,
    pub app_version: String,
    pub device_ip: Option<String>,
    pub errors: Vec<String>,
}

pub fn generate_report(device_ip: Option<String>, errors: Vec<String>) -> String {
    let report = DiagnosticReport {
        timestamp: format!("{:?}", SystemTime::now()),
        app_version: env!("CARGO_PKG_VERSION").to_string(),
        device_ip,
        errors,
    };

    serde_json::to_string_pretty(&report).unwrap_or_default()
}
```

**Step 2: Add IPC command**

Modify: `installer/src-tauri/src/main.rs`

```rust
mod diagnostics;

#[tauri::command]
fn get_diagnostics(device_ip: Option<String>, errors: Vec<String>) -> String {
    diagnostics::generate_report(device_ip, errors)
}
```

**Step 3: Wire up diagnostics in UI**

Modify: `installer/ui/app.js`

```javascript
document.getElementById('btn-diagnostics')?.addEventListener('click', async () => {
    const report = await invoke('get_diagnostics', {
        deviceIp: state.device?.ip,
        errors: state.errors || [],
    });

    navigator.clipboard.writeText(report);
    alert('Diagnostics copied to clipboard');
});
```

**Step 4: Commit**

```bash
git add src-tauri/src/diagnostics.rs src-tauri/src/main.rs ui/app.js
git commit -m "feat: add diagnostics export for troubleshooting"
```

### Task 5.3: Platform Testing

**Manual Testing Checklist:**

**macOS:**
- [ ] Device discovery via mDNS works
- [ ] Code entry and cookie storage works
- [ ] SSH key generation sets correct permissions (0600)
- [ ] SSH config is written correctly
- [ ] Main package installs successfully
- [ ] Module installation works
- [ ] All screens display correctly
- [ ] Error handling shows appropriate messages

**Windows:**
- [ ] Bundled OpenSSH binaries work
- [ ] Device discovery via mDNS works (may be flaky)
- [ ] Manual IP entry works
- [ ] Cookie stored in Credential Manager
- [ ] SSH key generation works
- [ ] File paths handle Windows drives correctly
- [ ] SCP uploads work
- [ ] All screens display correctly

**Test edge cases:**
- [ ] Move not on network → shows manual IP option
- [ ] Invalid code entry → shows error, allows retry
- [ ] Timeout on device confirmation → shows timeout error
- [ ] Host key changed → shows security warning
- [ ] Cookie expired → re-prompts for code
- [ ] Download failure → shows retry option
- [ ] Tarball validation fails → shows error, retries

**Step 1: Document test results**

Create: `installer/TESTING.md`

```markdown
# Testing Checklist

## macOS Testing
- [x] Device discovery
- [x] Auth flow
- [ ] Installation
- [ ] Module selection

## Windows Testing
- [ ] Bundled SSH works
- [ ] Auth flow
- [ ] Installation

## Edge Cases
- [ ] Network timeout
- [ ] Invalid code
- [ ] Host key change
```

**Step 2: Fix any issues found during testing**

**Step 3: Commit**

```bash
git add installer/TESTING.md
git commit -m "docs: add testing checklist"
```

### Task 5.4: Documentation

**Files:**
- Update: `installer/README.md`

**Step 1: Add comprehensive README**

Edit: `installer/README.md`

```markdown
# Move Everything Installer

Cross-platform desktop installer for Move Everything.

## For Users

Download the latest installer for your platform:
- macOS: `MoveEverythingInstaller.dmg`
- Windows: `MoveEverythingInstaller.exe`

Run the installer and follow the on-screen instructions.

## For Developers

### Prerequisites

- Node.js 18+
- Rust 1.70+
- Platform-specific tools:
  - macOS: Xcode Command Line Tools
  - Windows: Visual Studio Build Tools

### Setup

```bash
cd installer
npm install
```

### Development

```bash
npm run tauri dev
```

### Build

```bash
npm run tauri build
```

Outputs:
- macOS: `src-tauri/target/release/bundle/dmg/`
- Windows: `src-tauri/target/release/bundle/msi/`

### Windows Build Notes

OpenSSH binaries must be in `src-tauri/resources/bin/` before building:
- Download from: https://github.com/PowerShell/Win32-OpenSSH/releases
- Extract: `ssh.exe`, `ssh-keygen.exe`, `scp.exe`, required DLLs

## Architecture

See [Design Doc](../docs/plans/2026-02-10-desktop-installer-design.md) for detailed architecture.

**Backend (Rust):**
- Device discovery (mDNS)
- HTTP client with cookie management
- SSH operations
- Installation pipeline

**Frontend (HTML/CSS/JS):**
- Multi-screen state machine
- User input handling
- Progress display

## Troubleshooting

### macOS: "App cannot be opened"

```bash
xattr -d com.apple.quarantine /Applications/MoveEverythingInstaller.app
```

### Windows: OpenSSH not found

Ensure `resources/bin/` contains SSH binaries before building.

### Device not found

- Verify Move is on same WiFi network
- Try manual IP entry
- Check firewall settings
```

**Step 2: Commit**

```bash
git add installer/README.md
git commit -m "docs: add comprehensive installer README"
```

---

## Final Steps

### Task 6.1: Build Release Packages

**Step 1: Build for macOS**

```bash
cd installer
npm run tauri build
```

Output: `src-tauri/target/release/bundle/dmg/MoveEverythingInstaller_*.dmg`

**Step 2: Build for Windows**

On Windows machine:

```bash
cd installer
npm run tauri build
```

Output: `src-tauri/target/release/bundle/msi/MoveEverythingInstaller_*.msi`

**Step 3: Test installers**

- Install from DMG/MSI on fresh machines
- Verify full flow works end-to-end
- Check that bundled resources are included

### Task 6.2: Create GitHub Release

**Step 1: Tag release**

```bash
git tag -a installer-v0.1.0 -m "Initial desktop installer release"
git push --tags
```

**Step 2: Create GitHub release**

```bash
gh release create installer-v0.1.0 \
  --title "Desktop Installer v0.1.0" \
  --notes "Cross-platform installer for Move Everything (macOS + Windows)" \
  installer/src-tauri/target/release/bundle/dmg/*.dmg \
  installer/src-tauri/target/release/bundle/msi/*.msi
```

**Step 3: Update main README**

Add installer download links to main README.md.

**Step 4: Commit**

```bash
git add README.md
git commit -m "docs: add desktop installer download links"
git push
```

---

## Summary

This plan covers:

✅ **Phase 1:** API Discovery (manual reverse-engineering)
✅ **Phase 2:** Tauri project setup with bundled OpenSSH
✅ **Phase 3:** Backend implementation (device discovery, auth, SSH, installation)
✅ **Phase 4:** Frontend implementation (multi-screen UI)
✅ **Phase 5:** Integration, testing, documentation

**Estimated time:** 2-3 days for experienced developer

**Key deliverables:**
- Cross-platform desktop installer (macOS/Windows)
- Automated SSH setup with code-based auth
- Module cherry-picking UI
- Comprehensive error handling
- Platform-specific installers (DMG/MSI)
