# Code Signing Strategy for Desktop Installer

## Overview

Code signing is **critical** for distributing the desktop installer. Without it:
- **macOS:** Gatekeeper blocks unsigned apps by default
- **Windows:** SmartScreen warns users about unrecognized publishers
- **Users:** Must disable security features to install, creating bad UX and security risk

This document outlines the strategy for automatic code signing as part of the build process.

---

## macOS Code Signing

### Requirements

1. **Apple Developer Account** ($99/year)
   - Enroll at: https://developer.apple.com/programs/enroll/
   - Required for signing and notarization

2. **Developer ID Application Certificate**
   - Type: "Developer ID Application" (for distribution outside Mac App Store)
   - Create via: Xcode → Preferences → Accounts → Manage Certificates
   - Or via: https://developer.apple.com/account/resources/certificates

3. **Notarization**
   - Required for macOS 10.15+ (Catalina and later)
   - Apple scans app for malware before allowing it to run
   - Automated via `xcrun notarytool`

### Implementation

#### Option 1: Manual Signing (for initial testing)

```bash
# 1. Build the app
cd installer
npm run tauri:build

# 2. Sign the .app bundle
codesign --deep --force --verify --verbose \
  --sign "Developer ID Application: Your Name (TEAM_ID)" \
  --options runtime \
  "src-tauri/target/release/bundle/macos/Move Everything Installer.app"

# 3. Create DMG (Tauri does this automatically, but you can customize)
hdiutil create -volname "Move Everything Installer" \
  -srcfolder "src-tauri/target/release/bundle/macos/Move Everything Installer.app" \
  -ov -format UDZO "Move Everything Installer.dmg"

# 4. Sign the DMG
codesign --sign "Developer ID Application: Your Name (TEAM_ID)" \
  "Move Everything Installer.dmg"

# 5. Notarize (submit to Apple)
xcrun notarytool submit "Move Everything Installer.dmg" \
  --apple-id "your@email.com" \
  --team-id "TEAM_ID" \
  --password "app-specific-password" \
  --wait

# 6. Staple notarization ticket (attaches approval to DMG)
xcrun stapler staple "Move Everything Installer.dmg"

# 7. Verify
spctl -a -vvv -t install "Move Everything Installer.dmg"
# Should show: "accepted"
```

#### Option 2: Automatic Signing in Tauri

**tauri.conf.json:**
```json
{
  "bundle": {
    "macOS": {
      "signingIdentity": "Developer ID Application: Your Name (TEAM_ID)",
      "entitlements": "src-tauri/entitlements.plist",
      "hardenedRuntime": true
    }
  }
}
```

**src-tauri/entitlements.plist:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <!-- Hardened runtime entitlements -->
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <false/>
    <key>com.apple.security.cs.allow-dyld-environment-variables</key>
    <false/>
    <!-- Network access -->
    <key>com.apple.security.network.client</key>
    <true/>
    <!-- Keychain access -->
    <key>keychain-access-groups</key>
    <array>
        <string>$(AppIdentifierPrefix)com.move-everything.installer</string>
    </array>
</dict>
</plist>
```

#### Option 3: GitHub Actions (Recommended for CI/CD)

**Problem:** GitHub Actions runners don't have access to your signing certificate by default.

**Solution:** Store certificate in GitHub Secrets as base64-encoded .p12 file.

**.github/workflows/release-macos.yml:**
```yaml
name: Release macOS

on:
  push:
    tags: ['v*']

jobs:
  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install Node.js
        uses: actions/setup-node@v4
        with:
          node-version: 18

      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable

      - name: Install dependencies
        run: cd installer && npm install

      - name: Import signing certificate
        env:
          MACOS_CERTIFICATE: ${{ secrets.MACOS_CERTIFICATE }}
          MACOS_CERTIFICATE_PWD: ${{ secrets.MACOS_CERTIFICATE_PWD }}
        run: |
          # Decode certificate from base64
          echo $MACOS_CERTIFICATE | base64 --decode > certificate.p12

          # Create temporary keychain
          security create-keychain -p actions temp.keychain
          security default-keychain -s temp.keychain
          security unlock-keychain -p actions temp.keychain

          # Import certificate
          security import certificate.p12 -k temp.keychain \
            -P $MACOS_CERTIFICATE_PWD -T /usr/bin/codesign

          # Allow codesign to use the key
          security set-key-partition-list -S apple-tool:,apple: \
            -s -k actions temp.keychain

      - name: Build app
        run: cd installer && npm run tauri:build
        env:
          APPLE_SIGNING_IDENTITY: "Developer ID Application: Your Name (TEAM_ID)"

      - name: Notarize app
        env:
          APPLE_ID: ${{ secrets.APPLE_ID }}
          APPLE_ID_PASSWORD: ${{ secrets.APPLE_ID_PASSWORD }}
          APPLE_TEAM_ID: ${{ secrets.APPLE_TEAM_ID }}
        run: |
          # Submit for notarization
          xcrun notarytool submit \
            "installer/src-tauri/target/release/bundle/dmg/Move Everything Installer.dmg" \
            --apple-id "$APPLE_ID" \
            --team-id "$APPLE_TEAM_ID" \
            --password "$APPLE_ID_PASSWORD" \
            --wait

          # Staple ticket
          xcrun stapler staple \
            "installer/src-tauri/target/release/bundle/dmg/Move Everything Installer.dmg"

      - name: Upload release asset
        uses: softprops/action-gh-release@v1
        with:
          files: installer/src-tauri/target/release/bundle/dmg/Move Everything Installer.dmg
```

**GitHub Secrets to configure:**
1. `MACOS_CERTIFICATE` - Base64-encoded .p12 file:
   ```bash
   base64 -i DeveloperID.p12 | pbcopy
   # Paste into GitHub Secrets
   ```
2. `MACOS_CERTIFICATE_PWD` - Password for .p12 file
3. `APPLE_ID` - Your Apple ID email
4. `APPLE_ID_PASSWORD` - App-specific password (create at appleid.apple.com)
5. `APPLE_TEAM_ID` - Your 10-character team ID

---

## Windows Code Signing

### Requirements

1. **Code Signing Certificate**
   - Purchase from: DigiCert, Sectigo, GlobalSign, etc.
   - Cost: ~$100-400/year
   - Requires business verification (EV certificates require hardware token)

2. **signtool.exe**
   - Included with Windows SDK
   - Part of Visual Studio Build Tools

### Implementation

#### Option 1: Manual Signing

```powershell
# 1. Build the app
cd installer
npm run tauri:build

# 2. Sign the .msi installer
signtool sign /f "certificate.pfx" /p "password" /t http://timestamp.digicert.com `
  "src-tauri\target\release\bundle\msi\Move Everything Installer.msi"

# 3. Verify signature
signtool verify /pa "src-tauri\target\release\bundle\msi\Move Everything Installer.msi"
```

#### Option 2: Automatic Signing in Tauri

**tauri.conf.json:**
```json
{
  "bundle": {
    "windows": {
      "certificateThumbprint": "YOUR_CERT_THUMBPRINT",
      "timestampUrl": "http://timestamp.digicert.com",
      "wix": {
        "skipWebviewInstall": false
      }
    }
  }
}
```

**Notes:**
- `certificateThumbprint`: Find via `certmgr.msc` → Your cert → Details → Thumbprint
- Install certificate to Windows certificate store before building

#### Option 3: GitHub Actions

**Problem:** Windows code signing typically requires hardware token (EV certificate) which can't be used in CI.

**Solutions:**
1. **Standard certificate (OV)**: Can be stored in GitHub Secrets
2. **Cloud signing service**: Azure SignTool, DigiCert ONE
3. **Self-hosted runner**: With hardware token attached

**.github/workflows/release-windows.yml (using OV certificate):**
```yaml
name: Release Windows

on:
  push:
    tags: ['v*']

jobs:
  build-windows:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install Node.js
        uses: actions/setup-node@v4
        with:
          node-version: 18

      - name: Install Rust
        uses: dtolnay/rust-toolchain@stable

      - name: Install dependencies
        run: cd installer && npm install

      - name: Decode certificate
        env:
          WINDOWS_CERTIFICATE: ${{ secrets.WINDOWS_CERTIFICATE }}
        run: |
          # Decode from base64
          $bytes = [Convert]::FromBase64String($env:WINDOWS_CERTIFICATE)
          [IO.File]::WriteAllBytes("certificate.pfx", $bytes)

      - name: Build and sign
        env:
          WINDOWS_CERTIFICATE_PWD: ${{ secrets.WINDOWS_CERTIFICATE_PWD }}
        run: |
          cd installer
          npm run tauri:build

          # Sign the MSI
          & "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe" sign `
            /f ..\certificate.pfx `
            /p $env:WINDOWS_CERTIFICATE_PWD `
            /t http://timestamp.digicert.com `
            src-tauri\target\release\bundle\msi\Move Everything Installer.msi

      - name: Upload release asset
        uses: softprops/action-gh-release@v1
        with:
          files: installer/src-tauri/target/release/bundle/msi/Move Everything Installer.msi
```

**GitHub Secrets:**
1. `WINDOWS_CERTIFICATE` - Base64-encoded .pfx file:
   ```powershell
   $bytes = [IO.File]::ReadAllBytes("certificate.pfx")
   [Convert]::ToBase64String($bytes) | Set-Clipboard
   ```
2. `WINDOWS_CERTIFICATE_PWD` - Certificate password

---

## Development vs. Production Signing

### During Development

**macOS:**
- Use "Developer ID Application" certificate (same as production)
- Skip notarization for local builds (speeds up iteration)
- Enable "Allow unsigned executable memory" for debugging

**Windows:**
- Use self-signed certificate for testing (free)
- Create via: `New-SelfSignedCertificate` PowerShell cmdlet
- Install to trusted root store locally

### For Production Releases

**macOS:**
- Always sign with "Developer ID Application"
- Always notarize (required for Catalina+)
- Test on clean Mac to verify Gatekeeper acceptance

**Windows:**
- Always sign with trusted certificate (OV or EV)
- Use timestamp server (ensures signature remains valid after cert expires)
- Test on clean Windows to verify SmartScreen acceptance

---

## Certificate Management Best Practices

### Storage

- **Never commit certificates to git** (add *.p12, *.pfx to .gitignore)
- Store in password manager (1Password, LastPass, etc.)
- Use GitHub Secrets for CI/CD
- Rotate passwords annually

### Access Control

- Limit certificate access to release managers only
- Use hardware tokens (YubiKey) for EV certificates
- Log all signing operations for audit trail

### Expiry Management

- Set calendar reminders 60 days before expiry
- Renew certificates early (don't wait until last minute)
- Update GitHub Secrets after renewal

---

## Verification Checklist

### macOS

```bash
# Check signature
codesign -vvv --deep --strict "Move Everything Installer.app"

# Check notarization
spctl -a -vvv -t install "Move Everything Installer.dmg"

# Should show:
# - "signed by Developer ID Application: Your Name"
# - "accepted"
# - "origin=Developer ID Application: Your Name"
```

### Windows

```powershell
# Check signature
signtool verify /pa "Move Everything Installer.msi"

# Should show:
# - "Successfully verified"
# - Your organization name
# - Timestamp server URL
```

---

## Cost Breakdown

| Item | Cost | Frequency |
|------|------|-----------|
| Apple Developer Account | $99 | Annual |
| macOS Certificate | Included | With account |
| Windows OV Certificate | $100-200 | Annual |
| Windows EV Certificate | $300-400 | Annual |
| **Total (OV)** | **~$200-300/year** | |
| **Total (EV)** | **~$400-500/year** | |

**Recommendation:** Start with OV certificate (cheaper, easier to automate). Upgrade to EV if you see SmartScreen warnings.

---

## Timeline for Implementation

### Phase 1: Manual Signing (Week 1)
- [ ] Obtain Apple Developer account
- [ ] Create Developer ID Application certificate
- [ ] Test manual signing and notarization on macOS
- [ ] Purchase Windows OV certificate
- [ ] Test manual signing on Windows

### Phase 2: Automated Signing (Week 2)
- [ ] Configure Tauri for automatic signing
- [ ] Test local builds with signing enabled
- [ ] Update build documentation

### Phase 3: CI/CD Integration (Week 3)
- [ ] Export certificates to .p12/.pfx
- [ ] Add to GitHub Secrets
- [ ] Create GitHub Actions workflows
- [ ] Test release workflow end-to-end

### Phase 4: Documentation (Week 4)
- [ ] Document certificate renewal process
- [ ] Create troubleshooting guide
- [ ] Add verification steps to release checklist

---

## Troubleshooting

### macOS: "App is damaged and can't be opened"
- **Cause:** App not properly signed or notarized
- **Fix:** Check codesign and spctl output, re-sign if needed

### macOS: Notarization fails
- **Cause:** Hardened runtime issues or invalid entitlements
- **Fix:** Check notarization log: `xcrun notarytool log SUBMISSION_ID`

### Windows: "Unknown publisher" warning
- **Cause:** Certificate not from trusted CA, or SmartScreen hasn't built reputation yet
- **Fix:** Use EV certificate (instant reputation) or wait for SmartScreen reputation to build

### CI: Certificate import fails
- **Cause:** Invalid base64 encoding or wrong password
- **Fix:** Re-encode certificate, verify password in GitHub Secrets

---

## Recommended Approach

**For initial release:**
1. Obtain Apple Developer account ($99)
2. Create Developer ID Application certificate (free with account)
3. Purchase Windows OV certificate (~$100-200)
4. Set up manual signing process first (test locally)
5. Implement GitHub Actions automation (once manual process works)

**Total initial cost:** ~$200-300
**Time to implement:** ~2-3 weeks (accounting for certificate verification delays)

This approach balances cost, security, and automation for a professional release.
