# Desktop Installer vs. install.sh Comparison

## Critical Gaps

### 1. Screen Reader Accessibility ⚠️ BLOCKING
**install.sh has:** Complete screen reader support with quiet mode, TTS deployment, toggle shortcuts
**Desktop installer has:** None - **Must implement before launch**

**Required changes:**
- ARIA labels on all UI elements
- Semantic HTML (proper headings, buttons, landmarks)
- Keyboard navigation (tab order, focus management)
- Screen reader announcements for state changes
- Alt text for all visual indicators (spinners, progress bars)
- Skip links for navigation

### 2. SSH Known Hosts Cleanup
**install.sh has:** Automatic cleanup of stale known_hosts entries (lines 202-212)
**Desktop installer has:** None - will fail after firmware updates

**Required changes:**
- Detect "host key verification failed" error
- Offer to remove old fingerprint and retry
- Use `ssh-keygen -R move.local`

### 3. Tarball Validation
**install.sh has:** Validates tar structure before extraction (line 489)
**Desktop installer has:** Validation after extraction only

**Required changes:**
- Validate tarball structure via SSH before extraction
- Check for required files: `move-anything-shim.so`, `shim-entrypoint.sh`
- Detect corrupted downloads early

### 4. Root SSH Fallback
**install.sh has:** Falls back to root@ to fix permissions (lines 304-320)
**Desktop installer has:** None - will fail if permissions are wrong

**Required changes:**
- Detect permission denied errors
- Attempt root@ connection
- Fix authorized_keys permissions: `chmod 644 /data/authorized_keys`

### 5. Unsupported/Liability Warning
**install.sh has:** Interactive disclaimer (lines 393-413)
**Desktop installer has:** None

**Required changes:**
- Show warning screen before device discovery
- Require explicit "I understand" confirmation
- Can't be dismissed with ESC or clicking outside

## Important Gaps

### 6. Feature Configuration
**install.sh has:** Creates features.json with shadow UI, standalone, screen reader toggles
**Desktop installer has:** None

**Required changes:**
- Add feature selection screen
- Allow enabling/disabling: Shadow UI, Standalone mode, Screen Reader
- Write features.json to `/data/UserData/move-anything/config/`

### 7. Move Restart
**install.sh has:** Restarts Move cleanly via init.d, verifies shim loaded (lines 969-997)
**Desktop installer has:** None - leaves Move in inconsistent state

**Required changes:**
- Stop Move: `ssh root@move.local '/etc/init.d/move stop'`
- Kill stale processes
- Clean up shared memory: `rm -f /dev/shm/move-shadow-*`
- Start Move: `ssh root@move.local '/etc/init.d/move start'`
- Verify shim loaded in MoveOriginal process

### 8. Disk Space Checks
**install.sh has:** Checks root partition, fails if <5MB free (lines 621-632)
**Desktop installer has:** None - could fill device and brick it

**Required changes:**
- Check free space: `df / | tail -1 | awk '{print $4}'`
- Warn if <10MB free
- Fail if <5MB free

### 9. Asset Copy Functionality
**install.sh has:** Interactive copy of ROMs, SoundFonts, DX7 patches (lines 809-967)
**Desktop installer has:** None

**Optional enhancement:**
- Add "Copy Assets" screen after module installation
- Support drag-and-drop folders
- Copy to correct module subdirectories

### 10. MD5 Checksums
**install.sh has:** Shows build MD5 for verification (lines 435-440)
**Desktop installer has:** None

**Optional enhancement:**
- Compute MD5 of downloaded tarball
- Display in diagnostics
- Allow manual verification

## Minor Gaps

### 11. Clipboard Support
**install.sh has:** Copies SSH key to clipboard (lines 177-200)
**Desktop installer has:** Clipboard for diagnostics only

**Enhancement:**
- Copy SSH connection commands to clipboard on success screen

### 12. Interactive Troubleshooting
**install.sh has:** Helpful prompts with retry options (lines 274-344)
**Desktop installer has:** Generic error messages

**Enhancement:**
- Add specific troubleshooting hints based on error type
- Offer "retry" button with different strategies

### 13. Network Retry Logic
**install.sh has:** Retry wrappers for all SSH/SCP commands (lines 56-116)
**Desktop installer has:** Some retries in install.rs but inconsistent

**Enhancement:**
- Standardize retry logic across all SSH operations
- Add exponential backoff

## Features Desktop Installer Has That install.sh Doesn't

1. **Cookie-based authentication** - Desktop uses 6-digit code + cookie, install.sh uses web form
2. **mDNS discovery** - Automatic device finding
3. **Platform keychain** - Secure cookie storage
4. **Cross-platform binary handling** - Bundled SSH for Windows
5. **Module categorization** - Organized by type (sound_generators, audio_fx, etc.)
6. **Progress tracking** - Real-time download/install progress
7. **Modern UI** - Multi-screen state machine

## Security Gaps (see separate security review)

Covered in security review document.

## Implementation Priority

**BLOCKING (must fix before release):**
1. Screen reader accessibility
2. SSH known hosts cleanup
3. Unsupported/liability warning

**HIGH (should fix before release):**
4. Tarball validation before extraction
5. Feature configuration (screen reader, shadow UI, standalone)
6. Move restart with shim verification
7. Disk space checks
8. Root SSH fallback

**MEDIUM (nice to have):**
9. Asset copy functionality
10. MD5 checksums
11. Clipboard support for SSH commands
12. Enhanced troubleshooting

**LOW (future enhancements):**
13. Network retry improvements
