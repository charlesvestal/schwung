# Security Review - Desktop Installer

## Executive Summary

The desktop installer handles sensitive credentials (auth cookies, SSH private keys) and performs privileged operations (SSH key submission, device configuration). Overall security posture is **good** with proper use of OS-level credential storage, but there are **4 critical gaps** that must be addressed before release.

**Risk Level: MEDIUM** (acceptable for internal use, needs fixes for public release)

---

## 1. Credential Management

### ✅ Strengths

**Cookie Storage (auth.rs, cookie_storage.rs)**
- Uses platform keychain (macOS Keychain, Windows Credential Manager)
- Credentials encrypted at rest by OS
- No cookies stored in plaintext files
- Cookies scoped to service name "move-installer"

**Code Entry Flow**
- 6-digit code never logged or persisted
- Only transmitted once to `/api/v1/challenge-response`
- Code immediately discarded after submission

### 🔴 Critical Issues

**Issue 1: Windows SSH Private Key Permissions**
- **Location:** `ssh.rs:114-119`
- **Risk:** HIGH - Private keys may be readable by other users on shared Windows systems
- **Current code:**
  ```rust
  #[cfg(target_os = "windows")]
  {
      // TODO: Set ACLs on Windows
  }
  ```
- **Attack scenario:** On multi-user Windows machines, any user could read `~/.ssh/ableton_move` and gain SSH access to the Move device
- **Fix required:** Use Windows Security Descriptor API to restrict file to current user only

**Issue 2: No Input Validation on Manual IP Entry**
- **Location:** `ui/app.js` (device discovery screen)
- **Risk:** MEDIUM - User could enter malicious input
- **Attack scenario:** Attacker social-engineers user to enter attacker's IP, installer connects to malicious server instead of Move
- **Fix required:** Validate IP format, consider showing mDNS hostname for confirmation

### ⚠️ Important Issues

**Issue 3: Auto-Accept SSH Host Keys**
- **Location:** `ssh.rs:138, install.rs:136` - Uses `StrictHostKeyChecking=accept-new`
- **Risk:** MEDIUM - First connection auto-accepts any host key
- **Attack scenario:** MITM attack during initial setup (attacker spoofs move.local via DNS poisoning or ARP spoofing)
- **Current mitigation:** Requires local network access
- **Recommendation:** Show host key fingerprint on first connection, ask user to verify on Move's screen

**Issue 4: Cookie Replay**
- **Risk:** LOW - If attacker gains access to keychain, they can replay cookie
- **Current mitigation:** Cookie stored encrypted in OS keychain
- **Limitation:** No cookie expiry checking - old cookies may still work
- **Recommendation:** Add cookie age check, re-prompt if >30 days old

---

## 2. SSH Operations

### ✅ Strengths

**Key Generation**
- Uses ed25519 (modern, secure algorithm)
- Keys stored in dedicated location (`~/.ssh/ableton_move`)
- Unix permissions set correctly (0600 for private, 0644 for public)
- Dedicated known_hosts file to avoid conflicts

**Connection Security**
- All SSH operations use non-interactive mode (`-o BatchMode=yes`)
- Timeout configured (`-o ConnectTimeout=5`)
- Dedicated user account (`ableton@move.local`)

### 🔴 Critical Issues

**Issue 5: SSH Command Injection**
- **Location:** `install.rs:136-145` (ssh_exec function)
- **Risk:** HIGH - If hostname contains shell metacharacters, command injection is possible
- **Current code:**
  ```rust
  pub fn ssh_exec(hostname: &str, command: &str) -> Result<String, String> {
      let output = Command::new("ssh")
          .args(&[
              "-o", "StrictHostKeyChecking=accept-new",
              &format!("ableton@{}", hostname),
              command,
          ])
  ```
- **Attack scenario:** If user enters manual IP like `192.168.1.1; rm -rf /`, the hostname isn't validated
- **Fix required:** Validate hostname format before passing to SSH

### ⚠️ Important Issues

**Issue 6: Root Access Not Restricted**
- **Location:** `install.sh` is called via `sudo` (install.rs:217)
- **Risk:** MEDIUM - Installation script runs as root on Move device
- **Current mitigation:** Script is part of trusted tarball from GitHub
- **Limitation:** If GitHub account compromised, malicious script could be distributed
- **Recommendation:** Add checksum verification of install.sh before execution

---

## 3. Network Security

### ✅ Strengths

**HTTPS for Downloads**
- All GitHub downloads use HTTPS
- Module catalog fetched via HTTPS

**Local Network Only**
- Installer only connects to local `move.local` hostname
- No external API calls (besides GitHub releases)

### ⚠️ Important Issues

**Issue 7: No Certificate Pinning for GitHub**
- **Risk:** LOW - If GitHub's certificate is compromised, attacker could serve malicious releases
- **Current mitigation:** Relies on OS certificate store
- **Recommendation:** Add checksum verification (see Issue 6)

**Issue 8: mDNS Spoofing**
- **Location:** `device.rs:13-32` (mDNS discovery)
- **Risk:** LOW - Attacker on local network could advertise fake "move.local" service
- **Current mitigation:** Requires local network access
- **Recommendation:** Verify device responds correctly to `/api/v1/challenge-response` before proceeding

---

## 4. Data Privacy

### ✅ Strengths

**No Telemetry**
- Installer doesn't send any analytics or crash reports
- No tracking of installations
- No PII collected

**Minimal Data Storage**
- Only stores: auth cookie, SSH keys, last-known IP
- No logs written to disk (except diagnostics on error)

### 💡 Suggestions

**Suggestion 1: Diagnostics Sanitization**
- **Location:** `diagnostics.rs`
- **Current:** Diagnostics include device IP and error messages
- **Recommendation:** Add explicit note in UI that diagnostics don't contain secrets

---

## 5. Binary Security

### ✅ Strengths

**Code Signing (macOS)**
- Plan includes "figure out automatic code signing"
- macOS Gatekeeper will block unsigned apps by default

### 🔴 Critical Issues

**Issue 9: No Code Signing Yet**
- **Risk:** HIGH - Users must disable Gatekeeper to run installer
- **Attack scenario:** Malicious actor distributes fake installer, users bypass security to install it
- **Fix required:** Implement automatic code signing for macOS (see separate section below)

**Issue 10: No Windows Code Signing**
- **Risk:** MEDIUM - Windows SmartScreen will warn users
- **Attack scenario:** Users may ignore SmartScreen warnings
- **Recommendation:** Obtain code signing certificate for Windows

---

## 6. Dependency Security

### ✅ Strengths

**Minimal Dependencies**
- Only 7 main Rust crates (tauri, reqwest, serde, tokio, mdns-sd, keyring, dirs)
- All from crates.io (audited ecosystem)
- No JavaScript dependencies (vanilla JS)

### ⚠️ Important Issues

**Issue 11: No Dependency Auditing**
- **Risk:** LOW - Supply chain attack via compromised crate
- **Current mitigation:** Cargo.lock pins exact versions
- **Recommendation:** Run `cargo audit` in CI, add to release checklist

---

## 7. Attack Surface Analysis

### Potential Attack Vectors

| Vector | Risk | Mitigation |
|--------|------|------------|
| Malicious Move device on network | MEDIUM | Verify device via challenge-response |
| MITM during SSH setup | MEDIUM | Show host key fingerprint |
| Compromised GitHub release | MEDIUM | Add checksum verification |
| Malicious install.sh | LOW | Script in signed tarball |
| Keychain extraction | LOW | OS-level encryption |
| Code injection via hostname | HIGH | **Must fix** - validate input |
| Private key theft (Windows) | HIGH | **Must fix** - set ACLs |

---

## 8. Threat Model

### Assumptions

**Trusted:**
- User's computer (not compromised)
- User's local network (home WiFi)
- GitHub's infrastructure
- OS keychain security

**Untrusted:**
- Network traffic (could be MITM'd)
- User input (could be malicious)
- Move device (could be impersonated)

### Out of Scope

- Physical security of Move device
- Security of Move firmware itself
- Social engineering (user tricked into installing malware)

---

## 9. Compliance Considerations

### GDPR
- **N/A** - No personal data collected or transmitted to third parties

### Export Controls
- **N/A** - Uses standard cryptography (SSH, TLS), no custom crypto

### Open Source Licensing
- **OK** - All dependencies are permissively licensed (MIT, Apache 2.0)

---

## 10. Security Checklist for Release

### BLOCKING (Must Fix)

- [ ] **Issue 1:** Implement Windows ACL for private keys
- [ ] **Issue 5:** Validate hostname before SSH operations
- [ ] **Issue 9:** Implement macOS code signing

### HIGH PRIORITY (Should Fix)

- [ ] **Issue 2:** Validate IP address format in manual entry
- [ ] **Issue 3:** Show SSH host key fingerprint for user verification
- [ ] **Issue 10:** Obtain Windows code signing certificate

### MEDIUM PRIORITY (Nice to Have)

- [ ] **Issue 4:** Add cookie age checking (re-prompt if >30 days)
- [ ] **Issue 6:** Add checksum verification for install.sh
- [ ] **Issue 8:** Verify device identity before connecting
- [ ] **Issue 11:** Add `cargo audit` to CI pipeline

### Documentation

- [ ] Add security FAQ to README
- [ ] Document data storage locations
- [ ] Explain what diagnostics include/exclude
- [ ] Add "Report Security Issue" section

---

## 11. Recommended Security Hardening

### Short-Term (Before Release)

1. **Input Validation:**
   ```rust
   fn validate_hostname(hostname: &str) -> Result<(), String> {
       // Only allow: letters, numbers, dots, hyphens
       let re = Regex::new(r"^[a-zA-Z0-9\.\-]+$").unwrap();
       if !re.is_match(hostname) {
           return Err("Invalid hostname format".to_string());
       }
       Ok(())
   }
   ```

2. **Windows Key Permissions:**
   ```rust
   #[cfg(target_os = "windows")]
   fn set_windows_acl(path: &Path) -> Result<(), String> {
       use std::os::windows::fs::OpenOptionsExt;
       use winapi::um::winnt::FILE_ATTRIBUTE_NORMAL;
       // Restrict to current user only
       // Implementation details...
   }
   ```

3. **Host Key Verification:**
   ```rust
   fn show_host_key_fingerprint(hostname: &str) -> Result<String, String> {
       let output = Command::new("ssh-keyscan")
           .args(&["-t", "ed25519", hostname])
           .output()?;
       // Parse and show fingerprint to user
   }
   ```

### Long-Term (Future Releases)

4. Add checksum verification for all downloads
5. Implement automatic updates with signature verification
6. Add hardware security module (HSM) support for code signing
7. Consider sandboxing the installer (macOS App Sandbox)

---

## 12. Security Testing Recommendations

### Manual Testing

- [ ] Test on Windows with multiple user accounts (verify key permissions)
- [ ] Test manual IP entry with malicious input (`; rm -rf /`, `$(whoami)`, etc.)
- [ ] Test MITM scenarios (ARP spoofing, DNS poisoning)
- [ ] Test with expired/invalid cookies
- [ ] Test with changed SSH host keys

### Automated Testing

- [ ] Unit tests for input validation functions
- [ ] Integration tests for SSH operations
- [ ] Fuzzing for hostname/IP parsing

---

## Conclusion

The installer has a **solid security foundation** with proper credential management and minimal attack surface. The **critical gaps** (Windows key permissions, hostname validation, code signing) are well-understood and fixable.

**Security Grade: B** (Good, but needs critical fixes before public release)

**Recommendation:** Fix Issues 1, 5, and 9 before any public release. Other issues can be addressed in subsequent updates.
