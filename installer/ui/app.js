// Test: Change text to confirm app.js is loading
document.getElementById('status-text').textContent = 'app.js is loading...';

// Log buffer for export
const logBuffer = [];
function addLog(source, message) {
    const timestamp = new Date().toISOString();
    logBuffer.push(`[${timestamp}] [${source}] ${message}`);
}

async function exportLogs() {
    const header = `Move Everything Installer Logs\nExported: ${new Date().toISOString()}\nPlatform: ${navigator.platform}\nUser Agent: ${navigator.userAgent}\n${'='.repeat(60)}\n\n`;
    const logs = header + logBuffer.join('\n');
    try {
        const saved = await window.installer.invoke('export_logs', { logs });
        if (saved) {
            alert('Logs saved successfully.');
        }
    } catch (err) {
        // Fallback: copy to clipboard
        try {
            await navigator.clipboard.writeText(logs);
            alert('Logs copied to clipboard.');
        } catch (e) {
            console.error('Failed to export logs:', e);
        }
    }
}

// Application State
const state = {
    currentScreen: 'discovery',
    hostname: 'move.local',
    deviceIp: null,
    authCode: null,
    baseUrl: null,
    installType: 'complete',
    selectedModules: [],
    enableScreenReader: false,
    sshPassword: null,
    errors: [],
    versionInfo: null,
    installedModules: []
};

// Screen Management
function showScreen(screenName) {
    document.querySelectorAll('.screen').forEach(screen => {
        screen.classList.remove('active');
    });
    document.getElementById(`screen-${screenName}`).classList.add('active');
    state.currentScreen = screenName;
}

// Device Discovery - Try configured hostname first, fall back to manual entry
async function startDeviceDiscovery() {
    // Read hostname from input field
    const hostnameInput = document.getElementById('device-hostname');
    if (hostnameInput && hostnameInput.value.trim()) {
        state.hostname = hostnameInput.value.trim();
    }

    console.log('[DEBUG] Trying', state.hostname, 'first...');

    const statusDiv = document.getElementById('discovery-status');
    statusDiv.innerHTML = `<div class="spinner"></div><p>Connecting to ${state.hostname}...</p>`;

    // Try configured hostname directly
    try {
        const baseUrl = `http://${state.hostname}`;
        const isValid = await window.installer.invoke('validate_device_at', { baseUrl });

        if (isValid) {
            console.log('[DEBUG]', state.hostname, 'validated successfully');
            statusDiv.innerHTML = `<p style="color: green;">✓ Connected to ${state.hostname}</p>`;

            // Automatically proceed to next step
            setTimeout(() => {
                selectDevice(state.hostname);
            }, 500);
            return;
        }
    } catch (error) {
        console.error('[DEBUG]', state.hostname, 'validation failed:', error);
    }

    // If hostname fails, show manual entry
    statusDiv.innerHTML = `<p style="color: orange;">Could not connect to ${state.hostname}</p><p>Please enter your Move\'s IP address below:</p>`;
    document.querySelector('.manual-entry').style.display = 'block';
}

function displayDevices(devices) {
    const deviceList = document.getElementById('device-list');
    const discoveryStatus = document.getElementById('discovery-status');

    if (devices.length === 0) {
        discoveryStatus.innerHTML = '<p>No devices found. Try entering IP manually below.</p>';
        return;
    }

    discoveryStatus.style.display = 'none';
    deviceList.innerHTML = '';

    devices.forEach(device => {
        const item = document.createElement('div');
        item.className = 'device-item';
        item.innerHTML = `
            <h3>${device.name || 'Ableton Move'}</h3>
            <p>${device.ip}</p>
        `;
        item.onclick = () => selectDevice(device.ip);
        deviceList.appendChild(item);
    });
}

async function selectDevice(hostname) {
    state.deviceIp = hostname;

    const statusDiv = document.getElementById('discovery-status');
    statusDiv.innerHTML = '<div class="spinner"></div><p>Validating device...</p>';

    try {
        // Validate device is reachable
        const baseUrl = `http://${hostname}`;
        const isValid = await window.installer.invoke('validate_device_at', { baseUrl });

        if (isValid) {
            console.log('[DEBUG] Device validated, checking for saved cookie...');
            // Check if we have a saved cookie
            const savedCookie = await window.installer.invoke('get_saved_cookie');

            // First, test if SSH already works
            console.log('[DEBUG] Testing SSH connection...');
            const sshWorks = await window.installer.invoke('test_ssh', { hostname: hostname });

            if (sshWorks) {
                console.log('[DEBUG] SSH already works, proceeding to version check');
                await checkVersions();
                return;
            }

            console.log('[DEBUG] SSH not available yet, need to set up key');

            if (savedCookie) {
                console.log('[DEBUG] Found saved cookie, proceeding to SSH setup');
                // Try to use saved cookie, skip to SSH setup
                proceedToSshSetup(baseUrl);
            } else {
                console.log('[DEBUG] No saved cookie, requesting challenge code');
                // Request challenge code from Move
                await window.installer.invoke('request_challenge', { baseUrl });
                // Show code entry screen
                showScreen('code-entry');
                setupCodeEntry();
            }
        } else {
            statusDiv.innerHTML = '<p style="color: red;">Not a valid Move device</p>';
            document.querySelector('.manual-entry').style.display = 'block';
        }
    } catch (error) {
        console.error('[DEBUG] selectDevice error:', error);
        statusDiv.innerHTML = '<p style="color: red;">Error: ' + error + '</p>';
        document.querySelector('.manual-entry').style.display = 'block';
    }
}

// Code Entry
function setupCodeEntry() {
    const codeDigits = document.querySelectorAll('.code-digit');
    const submitButton = document.getElementById('btn-submit-code');

    // Auto-focus first digit
    codeDigits[0].focus();

    // Handle digit input
    codeDigits.forEach((digit, index) => {
        digit.value = '';

        digit.addEventListener('input', (e) => {
            const value = e.target.value;

            if (value.length === 1 && /^\d$/.test(value)) {
                if (index < codeDigits.length - 1) {
                    codeDigits[index + 1].focus();
                }
            }

            // Check if all digits are filled
            const allFilled = Array.from(codeDigits).every(d => d.value.length === 1);
            submitButton.disabled = !allFilled;
        });

        digit.addEventListener('keydown', (e) => {
            if (e.key === 'Backspace' && !e.target.value && index > 0) {
                codeDigits[index - 1].focus();
            }
        });

        // Allow paste
        digit.addEventListener('paste', (e) => {
            e.preventDefault();
            const pastedData = e.clipboardData.getData('text');
            const digits = pastedData.replace(/\D/g, '').slice(0, 6);

            digits.split('').forEach((char, i) => {
                if (codeDigits[i]) {
                    codeDigits[i].value = char;
                }
            });

            if (codeDigits[digits.length - 1]) {
                codeDigits[digits.length - 1].focus();
            }

            const allFilled = Array.from(codeDigits).every(d => d.value.length === 1);
            submitButton.disabled = !allFilled;
        });
    });
}

async function submitAuthCode() {
    const codeDigits = document.querySelectorAll('.code-digit');
    const code = Array.from(codeDigits).map(d => d.value).join('');
    state.authCode = code;

    try {
        const baseUrl = `http://${state.deviceIp}`;
        const cookieValue = await window.installer.invoke('submit_auth_code', {
            baseUrl: baseUrl,
            code: code
        });

        console.log('Auth successful, cookie saved');

        // On Windows, check for Git Bash before proceeding
        const gitBashCheck = await window.installer.invoke('check_git_bash_available');
        if (!gitBashCheck.available) {
            showError(
                'Git Bash is required for installation on Windows.\n\n' +
                'Please install Git for Windows from:\n' +
                'https://git-scm.com/download/win\n\n' +
                'Then restart the installer.'
            );
            return;
        }

        // Check for SSH key and show confirmation screen
        showSshKeyScreen(baseUrl);
    } catch (error) {
        showError('Failed to submit code: ' + error);
    }
}

async function showSshKeyScreen(baseUrl) {
    try {
        // Find or check if SSH key exists
        console.log('[DEBUG] Looking for existing SSH key...');
        let pubkeyPath = await window.installer.invoke('find_existing_ssh_key');

        const messageEl = document.getElementById('ssh-key-message');
        const explanationEl = document.getElementById('ssh-key-explanation');

        if (pubkeyPath) {
            console.log('[DEBUG] Found SSH key:', pubkeyPath);
            messageEl.textContent = 'Secure connection key found. Ready to add it to your Move device.';
            explanationEl.style.display = 'none';
        } else {
            console.log('[DEBUG] No SSH key found');
            messageEl.textContent = 'No secure connection key found. A new key will be generated and added to your Move device.';
            explanationEl.style.display = 'block';
        }

        // Store baseUrl for later
        state.baseUrl = baseUrl;

        showScreen('ssh-key');
    } catch (error) {
        showError('Connection setup failed: ' + error);
    }
}

async function proceedToSshSetup(baseUrl) {
    try {
        // Show confirmation screen FIRST (before submitting key)
        showScreen('confirm');

        // Find or generate SSH key
        console.log('[DEBUG] Looking for existing SSH key...');
        let pubkeyPath = await window.installer.invoke('find_existing_ssh_key');
        console.log('[DEBUG] Found SSH key:', pubkeyPath);

        if (!pubkeyPath) {
            console.log('[DEBUG] No SSH key found, generating new one');
            pubkeyPath = await window.installer.invoke('generate_new_ssh_key');
            console.log('[DEBUG] Generated SSH key:', pubkeyPath);
        }

        // Read public key content
        console.log('[DEBUG] Reading public key...');
        const pubkey = await window.installer.invoke('read_public_key', { path: pubkeyPath });
        console.log('[DEBUG] Public key length:', pubkey.length);
        console.log('[DEBUG] Public key preview:', pubkey.substring(0, 50) + '...');

        // Submit SSH key with auth cookie (this triggers prompt on Move)
        console.log('[DEBUG] Submitting SSH key to', baseUrl);
        await window.installer.invoke('submit_ssh_key_with_auth', {
            baseUrl: baseUrl,
            pubkey: pubkey
        });
        console.log('[DEBUG] SSH key submitted successfully');

        // Start polling for connection access
        startConfirmationPolling();
    } catch (error) {
        showError('Connection setup failed: ' + error);
    }
}

// SSH Confirmation Polling
let confirmationPollInterval;

async function startConfirmationPolling() {
    console.log('[DEBUG] Starting confirmation polling...');
    showScreen('confirm');

    confirmationPollInterval = setInterval(async () => {
        try {
            console.log('[DEBUG] Polling SSH connection...');
            const connected = await window.installer.invoke('test_ssh', {
                hostname: state.deviceIp
            });

            console.log('[DEBUG] SSH connected:', connected);
            if (connected) {
                clearInterval(confirmationPollInterval);
                console.log('[DEBUG] SSH confirmed, checking versions...');
                await checkVersions();
            }
        } catch (error) {
            console.error('Polling error:', error);
        }
    }, 2000); // Poll every 2 seconds
}

function cancelConfirmation() {
    if (confirmationPollInterval) {
        clearInterval(confirmationPollInterval);
    }
    showScreen('code-entry');
}

function cancelDiscovery() {
    // Reset state
    state.deviceIp = null;
    state.baseUrl = null;
    // Go back to warning screen
    showScreen('warning');
}

// Module Selection
function updateVersionCheckStatus(message) {
    const statusEl = document.querySelector('#version-check-status .instruction');
    if (statusEl) {
        statusEl.textContent = message;
    }
}

async function checkVersions() {
    try {
        console.log('[DEBUG] Checking if Move Everything is installed...');

        const hostname = state.deviceIp;

        // Quick lightweight check: is Move Everything installed? (doesn't scan modules)
        const coreCheck = await window.installer.invoke('check_core_installation', { hostname });

        if (!coreCheck.installed) {
            // Not installed - go directly to fresh install flow
            console.log('[DEBUG] Move Everything not installed, showing installation options');
            state.managementMode = false;
            state.installedModules = [];
            await loadModuleList();
            document.querySelector('#screen-modules h1').textContent = 'Installation Options';
            document.getElementById('btn-install').textContent = 'Install';
            document.querySelector('.install-options').style.display = 'block';
            document.getElementById('module-categories').style.display = 'none';
            document.getElementById('management-top-actions').style.display = 'none';
            document.getElementById('core-upgrade-row').style.display = 'none';
            document.getElementById('installed-modules').style.display = 'none';
            document.getElementById('available-modules').style.display = 'none';
            document.getElementById('secondary-actions').style.display = 'none';
            document.querySelector('#screen-modules > .action-buttons').style.display = 'flex';
            showScreen('modules');
            return;
        }

        // Already installed - go directly to combined upgrade & manage screen
        console.log('[DEBUG] Move Everything installed, loading upgrade & manage screen...');
        state.managementMode = true;

        // Show loading state
        showScreen('version-check');
        updateVersionCheckStatus('Checking installed modules...');

        // Listen for progress updates
        window.installer.on('version-check-progress', (message) => {
            updateVersionCheckStatus(message);
        });

        // Fetch installed modules, latest release, and module catalog in parallel
        const [installed, latestRelease] = await Promise.all([
            window.installer.invoke('check_installed_versions', { hostname }),
            window.installer.invoke('get_latest_release')
        ]);

        state.installedModules = installed.modules || [];
        const installedModuleIds = state.installedModules.map(m => m.id);

        const moduleCatalog = await window.installer.invoke('get_module_catalog', { installedModuleIds });

        // Clean up progress listener
        window.installer.removeAllListeners('version-check-progress');

        // Compare versions
        const versionInfo = await window.installer.invoke('compare_versions', {
            installed: { installed: true, core: null, modules: state.installedModules },
            latestRelease,
            moduleCatalog
        });

        // Add core upgrade info
        const hasUpgrade = coreCheck.core && latestRelease.version && coreCheck.core !== latestRelease.version;
        versionInfo.coreUpgrade = hasUpgrade ? {
            current: coreCheck.core,
            available: latestRelease.version
        } : null;

        state.versionInfo = versionInfo;
        state.allModules = moduleCatalog;

        // Merge assets info from installed modules into versionInfo entries
        const installedAssetsMap = new Map(
            state.installedModules.filter(m => m.assets).map(m => [m.id, m.assets])
        );
        for (const m of [...versionInfo.upgradableModules, ...versionInfo.upToDateModules]) {
            if (installedAssetsMap.has(m.id)) {
                m.assets = installedAssetsMap.get(m.id);
            }
        }

        // Configure modules screen for management mode
        document.querySelector('#screen-modules h1').textContent = 'Upgrade & Manage';
        document.querySelector('.install-options').style.display = 'none';
        document.getElementById('module-categories').style.display = 'none';
        document.getElementById('secondary-actions').style.display = 'flex';

        // Hide fresh-install action buttons, management mode has its own buttons
        document.querySelector('#screen-modules > .action-buttons').style.display = 'none';

        // Display management layout
        displayManagementModules();

        showScreen('modules');

    } catch (error) {
        console.error('Version check failed:', error);
        // If version check fails, assume fresh install
        state.managementMode = false;
        state.installedModules = [];
        await loadModuleList();
        document.querySelector('#screen-modules h1').textContent = 'Installation Options';
        document.getElementById('btn-install').textContent = 'Install';
        document.querySelector('.install-options').style.display = 'block';
        document.getElementById('management-top-actions').style.display = 'none';
        document.getElementById('core-upgrade-row').style.display = 'none';
        document.getElementById('installed-modules').style.display = 'none';
        document.getElementById('available-modules').style.display = 'none';
        document.getElementById('secondary-actions').style.display = 'none';
        document.querySelector('#screen-modules > .action-buttons').style.display = 'flex';
        showScreen('modules');
    }
}


async function loadModuleList() {
    try {
        const modules = await window.installer.invoke('get_module_catalog');
        state.allModules = modules; // Store for later use
        displayModules(modules);
        setupInstallationOptions();
    } catch (error) {
        console.error('Failed to load modules:', error);
        showError('Failed to load module list: ' + error);
    }
}

function setupInstallationOptions() {
    const radioButtons = document.querySelectorAll('input[name="install-type"]');
    const screenReaderCheckbox = document.getElementById('enable-screenreader');
    const moduleCategories = document.getElementById('module-categories');

    // Handle installation type changes
    radioButtons.forEach(radio => {
        radio.addEventListener('change', (e) => {
            state.installType = e.target.value;

            // Show/hide module list based on selection
            if (e.target.value === 'custom') {
                moduleCategories.style.display = 'block';
            } else {
                moduleCategories.style.display = 'none';
            }

            // Auto-check screen reader for screen reader only mode
            if (e.target.value === 'screenreader') {
                screenReaderCheckbox.checked = true;
                screenReaderCheckbox.disabled = true;
                state.enableScreenReader = true;
            } else {
                screenReaderCheckbox.disabled = false;
            }

            updateInstallButtonState();
        });
    });

    // Handle screen reader checkbox
    screenReaderCheckbox.addEventListener('change', (e) => {
        state.enableScreenReader = e.target.checked;
    });

    // Initialize
    updateInstallButtonState();
}

function displayModules(modules) {
    const categoriesDiv = document.getElementById('module-categories');
    categoriesDiv.innerHTML = '';

    // Group modules by category
    const categories = {
        'sound_generator': { title: 'Sound Generators', modules: [] },
        'audio_fx': { title: 'Audio Effects', modules: [] },
        'midi_fx': { title: 'MIDI Effects', modules: [] },
        'utility': { title: 'Utilities', modules: [] },
        'overtake': { title: 'Overtake Modules', modules: [] }
    };

    modules.forEach(module => {
        const category = module.component_type || 'utility';
        if (categories[category]) {
            categories[category].modules.push(module);
        }
    });

    // Display categories with checkboxes (fresh install mode only)
    Object.entries(categories).forEach(([key, category]) => {
        if (category.modules.length === 0) return;

        const categoryDiv = document.createElement('div');
        categoryDiv.className = 'module-category';

        const title = document.createElement('h3');
        title.textContent = category.title;
        categoryDiv.appendChild(title);

        category.modules.forEach(module => {
            const moduleItem = document.createElement('div');
            moduleItem.className = 'module-item';

            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.id = `module-${module.id}`;
            checkbox.checked = true;
            checkbox.setAttribute('data-module-id', module.id);
            checkbox.onchange = () => updateSelectedModules();

            const moduleInfo = document.createElement('div');
            moduleInfo.className = 'module-info';

            const moduleName = document.createElement('h4');
            const nameLink = document.createElement('a');
            nameLink.href = `https://github.com/${module.github_repo}`;
            nameLink.target = '_blank';
            nameLink.textContent = module.name;
            nameLink.onclick = (e) => e.stopPropagation();
            moduleName.appendChild(nameLink);

            const moduleDesc = document.createElement('p');
            moduleDesc.textContent = module.description || 'No description available';

            moduleInfo.appendChild(moduleName);
            moduleInfo.appendChild(moduleDesc);

            moduleItem.appendChild(checkbox);
            moduleItem.appendChild(moduleInfo);
            moduleItem.onclick = (e) => {
                if (e.target !== checkbox) {
                    checkbox.checked = !checkbox.checked;
                    updateSelectedModules();
                }
            };

            categoryDiv.appendChild(moduleItem);
        });

        categoriesDiv.appendChild(categoryDiv);
    });

    updateSelectedModules();
}

function updateSelectedModules() {
    const checkboxes = document.querySelectorAll('#module-categories input[type="checkbox"]');
    state.selectedModules = Array.from(checkboxes)
        .filter(cb => cb.checked)
        .map(cb => cb.id.replace('module-', ''));

    updateInstallButtonState();
}

// --- Module operation queue ---
// Ensures Install/Upgrade/Remove operations don't collide on the SSH connection
const moduleOpQueue = [];
let moduleOpRunning = false;

async function enqueueModuleOp(op) {
    return new Promise((resolve, reject) => {
        moduleOpQueue.push({ op, resolve, reject });
        processModuleOpQueue();
    });
}

async function processModuleOpQueue() {
    if (moduleOpRunning || moduleOpQueue.length === 0) return;
    moduleOpRunning = true;
    const { op, resolve, reject } = moduleOpQueue.shift();
    try {
        const result = await op();
        resolve(result);
    } catch (err) {
        reject(err);
    } finally {
        moduleOpRunning = false;
        processModuleOpQueue();
    }
}

function displayManagementModules() {
    const versionInfo = state.versionInfo;
    if (!versionInfo) return;

    // --- Core upgrade row ---
    const coreRow = document.getElementById('core-upgrade-row');
    if (versionInfo.coreUpgrade) {
        coreRow.style.display = 'block';
        coreRow.innerHTML = `
            <div class="module-row">
                <div class="module-row-info">
                    <h4>Move Everything Core</h4>
                    <span class="version-status upgrade">${versionInfo.coreUpgrade.current} \u2192 ${versionInfo.coreUpgrade.available}</span>
                </div>
                <div class="module-actions">
                    <button class="btn-action btn-upgrade" onclick="handleUpgradeCore()">Upgrade</button>
                </div>
            </div>
        `;
    } else {
        coreRow.style.display = 'none';
    }

    // --- Installed modules by category ---
    const installedDiv = document.getElementById('installed-modules');
    installedDiv.style.display = 'block';
    installedDiv.innerHTML = '';

    const allInstalled = [...versionInfo.upgradableModules, ...versionInfo.upToDateModules];

    // Group by component_type
    const categories = {
        'sound_generator': { title: 'Sound Generators', modules: [] },
        'audio_fx': { title: 'Audio Effects', modules: [] },
        'midi_fx': { title: 'MIDI Effects', modules: [] },
        'utility': { title: 'Utilities', modules: [] },
        'overtake': { title: 'Overtake Modules', modules: [] }
    };

    const upgradableIds = new Set(versionInfo.upgradableModules.map(m => m.id));

    allInstalled.forEach(module => {
        const cat = module.component_type || 'utility';
        if (categories[cat]) {
            categories[cat].modules.push(module);
        }
    });

    Object.entries(categories).forEach(([key, category]) => {
        if (category.modules.length === 0) return;

        const categoryDiv = document.createElement('div');
        categoryDiv.className = 'module-category';

        const title = document.createElement('h3');
        title.textContent = category.title;
        categoryDiv.appendChild(title);

        category.modules.forEach(module => {
            const row = document.createElement('div');
            row.className = 'module-row';
            row.setAttribute('data-module-id', module.id);

            const info = document.createElement('div');
            info.className = 'module-row-info';

            const nameEl = document.createElement('h4');
            const nameLink = document.createElement('a');
            nameLink.href = `https://github.com/${module.github_repo}`;
            nameLink.target = '_blank';
            nameLink.textContent = module.name;
            nameEl.appendChild(nameLink);

            const versionEl = document.createElement('span');
            if (upgradableIds.has(module.id)) {
                versionEl.className = 'version-status upgrade';
                versionEl.textContent = `${module.currentVersion} \u2192 `;
                const newVerLink = document.createElement('a');
                newVerLink.href = `https://github.com/${module.github_repo}/releases/tag/v${module.version}`;
                newVerLink.target = '_blank';
                newVerLink.textContent = module.version;
                versionEl.appendChild(newVerLink);
            } else {
                versionEl.className = 'version-status current';
                const verLink = document.createElement('a');
                const displayVer = module.currentVersion || module.version;
                verLink.href = `https://github.com/${module.github_repo}/releases/tag/v${displayVer}`;
                verLink.target = '_blank';
                verLink.textContent = `${displayVer} (latest)`;
                versionEl.appendChild(verLink);
            }

            info.appendChild(nameEl);
            info.appendChild(versionEl);

            const actions = document.createElement('div');
            actions.className = 'module-actions';

            if (upgradableIds.has(module.id)) {
                const upgradeBtn = document.createElement('button');
                upgradeBtn.className = 'btn-action btn-upgrade';
                upgradeBtn.textContent = 'Upgrade';
                upgradeBtn.onclick = () => handleUpgradeModule(module.id);
                actions.appendChild(upgradeBtn);
            }

            const removeBtn = document.createElement('button');
            removeBtn.className = 'btn-action btn-remove';
            removeBtn.textContent = 'Remove';
            removeBtn.onclick = () => handleRemoveModule(module.id, module.component_type);
            actions.appendChild(removeBtn);

            if (module.assets) {
                const assetBtn = document.createElement('button');
                assetBtn.className = 'btn-action btn-add-assets';
                assetBtn.textContent = `Add ${module.assets.label}`;
                assetBtn.onclick = () => handleAddAssets(module.id, module.component_type, module.assets);
                actions.appendChild(assetBtn);
            }

            row.appendChild(info);
            row.appendChild(actions);
            categoryDiv.appendChild(row);
        });

        installedDiv.appendChild(categoryDiv);
    });

    // --- Available (not installed) modules with individual Install buttons ---
    const availableDiv = document.getElementById('available-modules');
    const availableList = document.getElementById('available-module-list');

    if (versionInfo.newModules.length > 0) {
        availableDiv.style.display = 'block';
        availableList.innerHTML = '';

        versionInfo.newModules.forEach(module => {
            const row = document.createElement('div');
            row.className = 'module-row';
            row.setAttribute('data-module-id', module.id);

            const info = document.createElement('div');
            info.className = 'module-row-info';

            const nameEl = document.createElement('h4');
            const nameLink = document.createElement('a');
            nameLink.href = `https://github.com/${module.github_repo}`;
            nameLink.target = '_blank';
            nameLink.textContent = module.name;
            nameEl.appendChild(nameLink);

            if (module.version) {
                const versionLink = document.createElement('a');
                versionLink.href = `https://github.com/${module.github_repo}/releases/tag/v${module.version}`;
                versionLink.target = '_blank';
                versionLink.className = 'version-status current';
                versionLink.textContent = `v${module.version}`;
                nameEl.appendChild(document.createTextNode(' '));
                nameEl.appendChild(versionLink);
            }

            const descEl = document.createElement('p');
            descEl.textContent = module.description || 'No description available';

            info.appendChild(nameEl);
            info.appendChild(descEl);

            const actions = document.createElement('div');
            actions.className = 'module-actions';

            const installBtn = document.createElement('button');
            installBtn.className = 'btn-action btn-install-module';
            installBtn.textContent = 'Install';
            installBtn.onclick = () => handleInstallModule(module.id);
            actions.appendChild(installBtn);

            row.appendChild(info);
            row.appendChild(actions);
            availableList.appendChild(row);
        });
    } else {
        availableDiv.style.display = 'none';
    }

    // --- Upgrade All button visibility ---
    const hasAnyUpgrade = versionInfo.coreUpgrade || versionInfo.upgradableModules.length > 0;
    document.getElementById('management-top-actions').style.display = hasAnyUpgrade ? 'block' : 'none';
}

async function handleUpgradeCore() {
    if (!confirm('Upgrade Move Everything Core?')) return;

    showScreen('installing');
    try {
        initializeChecklist([]);
        // Manually add core item
        const checklist = document.getElementById('install-checklist');
        checklist.innerHTML = `
            <div class="checklist-item" data-item-id="core">
                <div class="checklist-icon pending">\u25CB</div>
                <div class="checklist-item-text">Move Everything Core</div>
            </div>
        `;

        updateInstallProgress('Setting up SSH configuration...', 0);
        await window.installer.invoke('setup_ssh_config', { hostname: state.hostname });

        updateInstallProgress('Fetching latest release...', 5);
        const release = await window.installer.invoke('get_latest_release');

        updateChecklistItem('core', 'in-progress');
        updateInstallProgress(`Downloading ${release.asset_name}...`, 10);
        const tarballPath = await window.installer.invoke('download_release', {
            url: release.download_url,
            destPath: `/tmp/${release.asset_name}`
        });

        updateInstallProgress('Upgrading Move Everything core...', 30);
        await window.installer.invoke('install_main', {
            tarballPath,
            hostname: state.deviceIp,
            flags: []
        });
        updateChecklistItem('core', 'completed');

        updateInstallProgress('Upgrade complete!', 100);
        setTimeout(() => {
            populateSuccessScreen();
            showScreen('success');
        }, 500);
    } catch (error) {
        state.errors.push({ timestamp: new Date().toISOString(), message: error.toString() });
        showError('Upgrade failed: ' + error);
    }
}

async function handleUpgradeAll() {
    const versionInfo = state.versionInfo;
    const upgradableModules = versionInfo.upgradableModules || [];
    const hasCore = !!versionInfo.coreUpgrade;

    if (!hasCore && upgradableModules.length === 0) return;

    const items = [];
    if (hasCore) items.push('Core');
    items.push(...upgradableModules.map(m => m.name));

    if (!confirm(`Upgrade the following?\n\n${items.join('\n')}`)) return;

    showScreen('installing');
    try {
        const moduleObjects = upgradableModules;
        // Build checklist manually
        const checklist = document.getElementById('install-checklist');
        const allItems = [];
        if (hasCore) allItems.push({ id: 'core', name: 'Move Everything Core' });
        moduleObjects.forEach(m => allItems.push({ id: m.id, name: m.name }));

        checklist.innerHTML = allItems.map(item => `
            <div class="checklist-item" data-item-id="${item.id}">
                <div class="checklist-icon pending">\u25CB</div>
                <div class="checklist-item-text">${item.name}</div>
            </div>
        `).join('');

        updateInstallProgress('Setting up SSH configuration...', 0);
        await window.installer.invoke('setup_ssh_config', { hostname: state.hostname });

        let progress = 5;

        // Upgrade core if available
        if (hasCore) {
            updateInstallProgress('Fetching latest release...', progress);
            const release = await window.installer.invoke('get_latest_release');

            updateChecklistItem('core', 'in-progress');
            updateInstallProgress(`Downloading ${release.asset_name}...`, 10);
            const tarballPath = await window.installer.invoke('download_release', {
                url: release.download_url,
                destPath: `/tmp/${release.asset_name}`
            });

            updateInstallProgress('Upgrading Move Everything core...', 20);
            await window.installer.invoke('install_main', {
                tarballPath,
                hostname: state.deviceIp,
                flags: []
            });
            updateChecklistItem('core', 'completed');
            progress = 40;
        }

        // Upgrade modules
        if (moduleObjects.length > 0) {
            const remainingProgress = 100 - progress;
            const progressPerModule = remainingProgress / moduleObjects.length;

            for (let i = 0; i < moduleObjects.length; i++) {
                const module = moduleObjects[i];
                const baseProgress = progress + (i * progressPerModule);

                updateChecklistItem(module.id, 'in-progress');
                updateInstallProgress(`Downloading ${module.name} (${i + 1}/${moduleObjects.length})...`, baseProgress);
                const tarballPath = await window.installer.invoke('download_release', {
                    url: module.download_url,
                    destPath: `/tmp/${module.asset_name}`
                });

                updateInstallProgress(`Upgrading ${module.name} (${i + 1}/${moduleObjects.length})...`, baseProgress + progressPerModule * 0.5);
                await window.installer.invoke('install_module_package', {
                    moduleId: module.id,
                    tarballPath,
                    componentType: module.component_type,
                    hostname: state.deviceIp
                });
                updateChecklistItem(module.id, 'completed');
            }
        }

        updateInstallProgress('All upgrades complete!', 100);
        setTimeout(() => {
            populateSuccessScreen();
            showScreen('success');
        }, 500);
    } catch (error) {
        state.errors.push({ timestamp: new Date().toISOString(), message: error.toString() });
        showError('Upgrade failed: ' + error);
    }
}

async function handleUpgradeModule(moduleId) {
    const module = state.allModules.find(m => m.id === moduleId);
    if (!module) return;

    // Update button to show queued/in-progress state
    const row = document.querySelector(`.module-row[data-module-id="${moduleId}"]`);
    if (row) {
        const actions = row.querySelector('.module-actions');
        actions.innerHTML = '<span class="action-status installing">Queued...</span>';
    }

    try {
        await enqueueModuleOp(async () => {
            // Update to in-progress
            if (row) {
                const actions = row.querySelector('.module-actions');
                actions.innerHTML = '<span class="action-status installing">Upgrading...</span>';
            }

            await window.installer.invoke('setup_ssh_config', { hostname: state.hostname });

            const tarballPath = await window.installer.invoke('download_release', {
                url: module.download_url,
                destPath: `/tmp/${module.asset_name}`
            });

            await window.installer.invoke('install_module_package', {
                moduleId: module.id,
                tarballPath,
                componentType: module.component_type,
                hostname: state.deviceIp
            });
        });

        // Move from upgradable to up-to-date in state
        const vi = state.versionInfo;
        const upgraded = vi.upgradableModules.find(m => m.id === moduleId);
        if (upgraded) {
            vi.upgradableModules = vi.upgradableModules.filter(m => m.id !== moduleId);
            upgraded.currentVersion = upgraded.version;
            vi.upToDateModules.push(upgraded);
        }

        // Re-render to reflect new state
        displayManagementModules();
    } catch (error) {
        console.error('Upgrade failed:', error);
        if (row) {
            const actions = row.querySelector('.module-actions');
            actions.innerHTML = `<span class="action-status error">Failed</span>`;
        }
        // Re-render after a delay so user sees the error
        setTimeout(() => displayManagementModules(), 2000);
    }
}

async function handleRemoveModule(moduleId, componentType) {
    const module = state.allModules.find(m => m.id === moduleId);
    const displayName = module ? module.name : moduleId;

    if (!confirm(`Remove ${displayName}? This will delete the module from your device.`)) return;

    const row = document.querySelector(`.module-row[data-module-id="${moduleId}"]`);
    if (row) {
        const actions = row.querySelector('.module-actions');
        actions.innerHTML = '<span class="action-status installing">Removing...</span>';
    }

    try {
        await enqueueModuleOp(async () => {
            await window.installer.invoke('remove_module', {
                moduleId,
                componentType,
                hostname: state.deviceIp
            });
        });

        // Remove from installed modules state
        state.installedModules = state.installedModules.filter(m => m.id !== moduleId);

        // Move module from installed to available in versionInfo
        const vi = state.versionInfo;
        const removedFromUpgradable = vi.upgradableModules.find(m => m.id === moduleId);
        const removedFromUpToDate = vi.upToDateModules.find(m => m.id === moduleId);
        const removedModule = removedFromUpgradable || removedFromUpToDate;

        vi.upgradableModules = vi.upgradableModules.filter(m => m.id !== moduleId);
        vi.upToDateModules = vi.upToDateModules.filter(m => m.id !== moduleId);

        if (removedModule) {
            vi.newModules.push(removedModule);
        }

        // Re-render management view
        displayManagementModules();
    } catch (error) {
        console.error('Remove failed:', error);
        if (row) {
            const actions = row.querySelector('.module-actions');
            actions.innerHTML = `<span class="action-status error">Failed</span>`;
        }
        setTimeout(() => displayManagementModules(), 2000);
    }
}

async function handleInstallModule(moduleId) {
    const module = state.allModules.find(m => m.id === moduleId);
    if (!module) return;

    // Update button to show queued state
    const row = document.querySelector(`#available-module-list .module-row[data-module-id="${moduleId}"]`);
    if (row) {
        const actions = row.querySelector('.module-actions');
        actions.innerHTML = '<span class="action-status installing">Queued...</span>';
    }

    try {
        await enqueueModuleOp(async () => {
            // Update to in-progress
            if (row) {
                const actions = row.querySelector('.module-actions');
                actions.innerHTML = '<span class="action-status installing">Installing...</span>';
            }

            await window.installer.invoke('setup_ssh_config', { hostname: state.hostname });

            const tarballPath = await window.installer.invoke('download_release', {
                url: module.download_url,
                destPath: `/tmp/${module.asset_name}`
            });

            await window.installer.invoke('install_module_package', {
                moduleId: module.id,
                tarballPath,
                componentType: module.component_type,
                hostname: state.deviceIp
            });
        });

        // Move from new to up-to-date in state
        const vi = state.versionInfo;
        vi.newModules = vi.newModules.filter(m => m.id !== moduleId);
        const installedVersion = module.version || 'installed';
        module.currentVersion = installedVersion;
        vi.upToDateModules.push(module);

        // Add to installed modules
        state.installedModules.push({
            id: module.id,
            name: module.name,
            version: installedVersion,
            component_type: module.component_type
        });

        // Re-render to move module to installed section
        displayManagementModules();
    } catch (error) {
        console.error('Install failed:', error);
        if (row) {
            const actions = row.querySelector('.module-actions');
            actions.innerHTML = `<span class="action-status error">Failed</span>`;
        }
        setTimeout(() => displayManagementModules(), 2000);
    }
}

async function handleAddAssets(moduleId, componentType, assets) {
    const row = document.querySelector(`.module-row[data-module-id="${moduleId}"]`);
    const assetBtn = row ? row.querySelector('.btn-add-assets') : null;

    if (assetBtn) {
        assetBtn.textContent = 'Uploading...';
        assetBtn.disabled = true;
    }

    try {
        const categoryPath = componentType ? `${componentType}s` : 'utility';
        const remoteDir = `/data/UserData/move-anything/modules/${categoryPath}/${moduleId}/${assets.path}`;

        const result = await enqueueModuleOp(async () => {
            return await window.installer.invoke('pick_and_upload_assets', {
                remoteDir,
                hostname: state.deviceIp,
                extensions: assets.extensions,
                label: assets.label
            });
        });

        if (result.canceled) {
            // User cancelled file picker
            if (assetBtn) {
                assetBtn.textContent = `Add ${assets.label}`;
                assetBtn.disabled = false;
            }
            return;
        }

        const failed = result.results.filter(r => !r.success);
        if (failed.length > 0) {
            const failedNames = failed.map(f => f.file).join(', ');
            alert(`Some files failed to upload: ${failedNames}`);
        }

        const succeeded = result.results.filter(r => r.success).length;
        if (assetBtn) {
            assetBtn.textContent = `${succeeded} uploaded`;
            assetBtn.disabled = false;
            setTimeout(() => {
                assetBtn.textContent = `Add ${assets.label}`;
            }, 2000);
        }
    } catch (error) {
        console.error('Asset upload failed:', error);
        if (assetBtn) {
            assetBtn.textContent = 'Failed';
            assetBtn.disabled = false;
            setTimeout(() => {
                assetBtn.textContent = `Add ${assets.label}`;
            }, 2000);
        }
    }
}

function updateInstallButtonState() {
    const installButton = document.getElementById('btn-install');
    if (!installButton) return;

    if (state.installType === 'custom') {
        installButton.disabled = state.selectedModules.length === 0;
    } else {
        installButton.disabled = false;
    }
}

// Installation
function initializeChecklist(modules) {
    const checklist = document.getElementById('install-checklist');
    const items = [];

    // Always add core item for fresh install
    items.push({
        id: 'core',
        name: 'Move Everything Core',
        status: 'pending'
    });

    // Add module items
    modules.forEach(module => {
        items.push({
            id: module.id,
            name: module.name,
            status: 'pending'
        });
    });

    // Render checklist
    checklist.innerHTML = items.map(item => `
        <div class="checklist-item" data-item-id="${item.id}">
            <div class="checklist-icon pending">○</div>
            <div class="checklist-item-text">${item.name}</div>
        </div>
    `).join('');
}

function updateChecklistItem(itemId, status) {
    const item = document.querySelector(`.checklist-item[data-item-id="${itemId}"]`);
    if (!item) return;

    const icon = item.querySelector('.checklist-icon');

    // Remove old status classes
    item.classList.remove('pending', 'in-progress', 'completed');
    icon.classList.remove('pending', 'in-progress', 'completed');

    // Add new status
    item.classList.add(status);
    icon.classList.add(status);

    // Update icon
    if (status === 'pending') {
        icon.innerHTML = '○';
    } else if (status === 'in-progress') {
        icon.innerHTML = '<div class="spinner"></div>';
    } else if (status === 'completed') {
        icon.innerHTML = '✓';
    }
}

async function startInstallation() {
    showScreen('installing');

    try {
        // Determine which modules to install (fresh install mode only)
        let modulesToInstall = [];
        if (state.installType === 'complete') {
            modulesToInstall = state.allModules.map(m => m.id);
        } else if (state.installType === 'custom') {
            modulesToInstall = state.selectedModules;
        }

        // Get module objects for checklist
        const moduleObjects = state.allModules.filter(m => modulesToInstall.includes(m.id));

        // Initialize checklist
        initializeChecklist(moduleObjects);

        // Setup SSH config
        updateInstallProgress('Setting up SSH configuration...', 0);
        await window.installer.invoke('setup_ssh_config', { hostname: state.hostname });

        let startProgressForModules = 10;

        // Always install core in fresh install mode
        {
            // Get latest release info
            updateInstallProgress('Fetching latest release...', 5);
            const release = await window.installer.invoke('get_latest_release');

            // Download main package
            updateInstallProgress(`Downloading ${release.asset_name}...`, 10);
            const mainTarballPath = await window.installer.invoke('download_release', {
                url: release.download_url,
                destPath: `/tmp/${release.asset_name}`
            });

            // Determine installation flags based on mode
            const installFlags = [];
            if (state.installType === 'screenreader') {
                installFlags.push('--enable-screen-reader');
                installFlags.push('--disable-shadow-ui');
                installFlags.push('--disable-standalone');
            } else if (state.enableScreenReader) {
                installFlags.push('--enable-screen-reader');
            }

            // Install main package
            const coreAction = 'Installing';
            updateChecklistItem('core', 'in-progress');
            updateInstallProgress(`${coreAction} Move Everything core...`, 30);
            await window.installer.invoke('install_main', {
                tarballPath: mainTarballPath,
                hostname: state.deviceIp,
                flags: installFlags
            });
            updateChecklistItem('core', 'completed');
            startProgressForModules = 50;
        }

        // Install modules (if any)
        if (modulesToInstall.length > 0) {
            updateInstallProgress('Fetching module catalog...', startProgressForModules);
            const modules = state.allModules;

            const moduleCount = modulesToInstall.length;
            const remainingProgress = 100 - startProgressForModules;
            const progressPerModule = remainingProgress / moduleCount;

            // Install each module
            for (let i = 0; i < moduleCount; i++) {
                const moduleId = modulesToInstall[i];
                const module = modules.find(m => m.id === moduleId);

                if (module) {
                    const baseProgress = startProgressForModules + (i * progressPerModule);

                    // Determine if this is an upgrade or fresh install
                    const isUpgrade = state.installedModules.some(m => m.id === module.id);
                    const action = isUpgrade ? 'Upgrading' : 'Installing';

                    updateChecklistItem(module.id, 'in-progress');
                    updateInstallProgress(`Downloading ${module.name} (${i + 1}/${moduleCount})...`, baseProgress);
                    const moduleTarballPath = await window.installer.invoke('download_release', {
                        url: module.download_url,
                        destPath: `/tmp/${module.asset_name}`
                    });

                    updateInstallProgress(`${action} ${module.name} (${i + 1}/${moduleCount})...`, baseProgress + progressPerModule * 0.5);
                    await window.installer.invoke('install_module_package', {
                        moduleId: module.id,
                        tarballPath: moduleTarballPath,
                        componentType: module.component_type,
                        hostname: state.deviceIp
                    });
                    updateChecklistItem(module.id, 'completed');
                }
            }
        }

        // Installation complete
        updateInstallProgress('Installation complete!', 100);
        setTimeout(() => {
            populateSuccessScreen();
            showScreen('success');
        }, 500);
    } catch (error) {
        state.errors.push({
            timestamp: new Date().toISOString(),
            message: error.toString()
        });
        showError('Installation failed: ' + error);
    }
}

function populateSuccessScreen() {
    const container = document.getElementById('success-next-steps');
    const isScreenReaderOnly = state.installType === 'screenreader';
    const hasShadowUi = !isScreenReaderOnly;
    const hasStandalone = !isScreenReaderOnly;

    let html = '<p><strong>Getting Started:</strong></p>';
    html += '<ul style="margin: 0.5rem 0 0 1.5rem; color: #b8b8b8; list-style: none; padding: 0;">';

    if (hasShadowUi) {
        html += '<li style="margin: 0.5rem 0;"><strong style="color: #0066cc;">Shift + Vol + Track</strong> or <strong style="color: #0066cc;">Shift + Vol + Menu</strong> &mdash; Access track and master slots</li>';
        html += '<li style="margin: 0.5rem 0;"><strong style="color: #0066cc;">Shift + Vol + Jog Click</strong> &mdash; Access overtake modules</li>';
    }

    if (hasStandalone) {
        html += '<li style="margin: 0.5rem 0;"><strong style="color: #0066cc;">Shift + Vol + Knob 8</strong> &mdash; Enter standalone mode</li>';
    }

    if (isScreenReaderOnly) {
        html += '<li style="margin: 0.5rem 0;"><strong style="color: #0066cc;">Shift + Menu</strong> &mdash; Toggle screen reader on and off</li>';
    }

    html += '</ul>';
    container.innerHTML = html;
    container.style.display = '';

    // Reset title in case it was changed by uninstall
    document.querySelector('#screen-success h1').textContent = "You're All Set!";
}

function updateInstallProgress(message, percent) {
    const progressStatus = document.getElementById('install-status');
    if (progressStatus) {
        progressStatus.textContent = message;
    }

    if (percent !== undefined) {
        const progressFill = document.querySelector('.progress-fill');
        if (progressFill) {
            progressFill.style.width = `${percent}%`;
        }
    }

    console.log('Install progress:', message, percent !== undefined ? `${percent}%` : '');
}

// Error Handling
function parseError(error) {
    const errorStr = error.toString().toLowerCase();

    // Network/connectivity errors
    if (errorStr.includes('timeout') || errorStr.includes('econnrefused') || errorStr.includes('ehostunreach')) {
        return {
            title: 'Connection Failed',
            message: 'Could not connect to your Move device.',
            suggestions: [
                'Check that your Move is powered on',
                'Ensure your Move is connected to the same WiFi network',
                'Try restarting your Move',
                'Check your WiFi connection'
            ]
        };
    }

    if (errorStr.includes('dns') || errorStr.includes('getaddrinfo') || errorStr.includes('.local')) {
        return {
            title: 'Device Not Found',
            message: 'Could not find your Move on the network.',
            suggestions: [
                'Try entering your Move\'s IP address manually',
                'Check that your Move is connected to WiFi',
                'Make sure you\'re on the same WiFi network as your Move',
                'On Windows, install Bonjour service (comes with iTunes/iCloud)'
            ]
        };
    }

    // Download errors
    if (errorStr.includes('download') || errorStr.includes('404') || errorStr.includes('fetch failed')) {
        return {
            title: 'Download Failed',
            message: 'Could not download required files.',
            suggestions: [
                'Check your internet connection',
                'Try again in a few moments',
                'Verify GitHub is accessible from your network'
            ]
        };
    }

    // Authentication errors
    if (errorStr.includes('auth') || errorStr.includes('unauthorized') || errorStr.includes('challenge')) {
        return {
            title: 'Authentication Failed',
            message: 'Could not authenticate with your Move.',
            suggestions: [
                'The authorization code may have expired',
                'Try restarting the installer',
                'Make sure you entered the correct code from your Move display'
            ]
        };
    }

    // Connection setup errors
    if (errorStr.includes('key') || errorStr.includes('permission denied')) {
        return {
            title: 'Connection Setup Failed',
            message: 'Could not set up secure connection to your Move.',
            suggestions: [
                'Make sure you confirmed "Yes" on your Move device',
                'Try the setup process again',
                'Restart your Move and try again'
            ]
        };
    }

    // Disk space errors
    if (errorStr.includes('enospc') || errorStr.includes('no space')) {
        return {
            title: 'Installation Failed',
            message: 'Not enough space on your Move device.',
            suggestions: [
                'Free up space by deleting unused samples or sets',
                'Try installing fewer modules (use Custom mode)',
                'Consider installing Core Only and adding modules later'
            ]
        };
    }

    // Generic fallback
    return {
        title: 'Installation Error',
        message: error.toString(),
        suggestions: [
            'Try restarting the installer',
            'Check that your Move has the latest firmware',
            'Copy diagnostics and report the issue on GitHub'
        ]
    };
}

function showError(message) {
    const parsed = parseError(message);

    state.errors.push({
        timestamp: new Date().toISOString(),
        message: message
    });

    showScreen('error');

    const errorDiv = document.getElementById('error-message');
    errorDiv.innerHTML = `
        <h3 style="margin: 0 0 1rem 0; color: #ff6666;">${parsed.title}</h3>
        <p style="margin: 0 0 1rem 0;">${parsed.message}</p>
        <div style="margin-top: 1.5rem;">
            <strong style="color: #fff;">What to try:</strong>
            <ul style="margin: 0.5rem 0 0 1.5rem; color: #b8b8b8;">
                ${parsed.suggestions.map(s => `<li style="margin: 0.5rem 0;">${s}</li>`).join('')}
            </ul>
        </div>
    `;
}

function retryInstallation() {
    state.currentScreen = 'discovery';
    state.deviceIp = null;
    state.authCode = null;
    state.selectedModules = [];
    state.sshPassword = null;
    state.errors = [];

    showScreen('discovery');
    startDeviceDiscovery();
}

// Utility Functions
function closeApplication() {
    window.close();
}

// Event Listeners
document.addEventListener('DOMContentLoaded', () => {
    console.log('[DEBUG] DOM loaded, installer API available:', !!window.installer);

    // Listen for backend logs
    window.installer.on('backend-log', (message) => {
        console.log('[BACKEND]', message);
        addLog('BACKEND', message);
    });

    // Capture frontend console.log too
    const origConsoleLog = console.log;
    const origConsoleError = console.error;
    console.log = function(...args) {
        origConsoleLog.apply(console, args);
        addLog('UI', args.join(' '));
    };
    console.error = function(...args) {
        origConsoleError.apply(console, args);
        addLog('UI:ERROR', args.join(' '));
    };

    // Warning screen
    document.getElementById('btn-accept-warning').onclick = async (e) => {
        // Hidden test mode: Shift+Click to run SSH format tests
        if (e.shiftKey) {
            console.log('[DEBUG] Running SSH format tests...');
            alert('Running SSH format tests... Check console for results.');
            try {
                const cookie = await window.installer.invoke('get_saved_cookie');
                const results = await window.installer.invoke('test_ssh_formats', { cookie });
                console.log('[TEST RESULTS]', JSON.stringify(results, null, 2));
                alert(`Test complete! Results:\n\n${JSON.stringify(results, null, 2)}`);
            } catch (err) {
                console.error('[TEST ERROR]', err);
                alert(`Test failed: ${err.message}`);
            }
            return;
        }

        showScreen('discovery');
        startDeviceDiscovery();
    };

    document.getElementById('btn-cancel').onclick = () => {
        closeApplication();
    };

    // Discovery screen
    document.getElementById('btn-manual-connect').onclick = () => {
        const ip = document.getElementById('manual-ip').value.trim();
        if (ip) {
            selectDevice(ip);
        }
    };

    // Allow Enter key to connect from manual IP input
    document.getElementById('manual-ip').addEventListener('keypress', (e) => {
        if (e.key === 'Enter') {
            const ip = e.target.value.trim();
            if (ip) {
                selectDevice(ip);
            }
        }
    });

    // Discovery screen
    document.getElementById('btn-cancel-discovery').onclick = cancelDiscovery;

    // Code entry screen
    document.getElementById('btn-submit-code').onclick = submitAuthCode;
    document.getElementById('btn-back-discovery').onclick = () => showScreen('discovery');

    // SSH Key screen
    document.getElementById('btn-add-ssh-key').onclick = () => proceedToSshSetup(state.baseUrl);
    document.getElementById('btn-back-ssh-key').onclick = () => showScreen('code-entry');

    // Confirm screen
    document.getElementById('btn-cancel-confirm').onclick = cancelConfirmation;

    // Management mode buttons
    document.getElementById('btn-upgrade-all').onclick = handleUpgradeAll;

    // Secondary action links (Screen Reader / Uninstall)
    document.getElementById('link-screenreader').onclick = async (e) => {
        e.preventDefault();
        try {
            const hostname = state.deviceIp;

            // Check current status
            updateInstallProgress('Checking screen reader status...', 30);
            showScreen('installing');

            const currentStatus = await window.installer.invoke('get_screen_reader_status', { hostname });

            // Ask user what they want
            const action = currentStatus ? 'disable' : 'enable';
            const confirmMsg = currentStatus
                ? 'Screen reader is currently enabled. Disable it?'
                : 'Screen reader is currently disabled. Enable it?';

            if (!confirm(confirmMsg)) {
                showScreen('modules');
                return;
            }

            // Toggle the state
            updateInstallProgress(`${action === 'enable' ? 'Enabling' : 'Disabling'} screen reader...`, 60);
            const result = await window.installer.invoke('set_screen_reader_state', {
                hostname,
                enabled: !currentStatus
            });

            updateInstallProgress('Complete!', 100);
            alert(result.message);

            // Go back to modules screen
            showScreen('modules');
        } catch (error) {
            console.error('Failed to toggle screen reader:', error);
            showError('Failed to toggle screen reader: ' + error.message);
        }
    };

    document.getElementById('link-uninstall').onclick = async (e) => {
        e.preventDefault();
        if (confirm('Are you sure you want to uninstall Move Everything? This will restore your Move to stock firmware.')) {
            try {
                const hostname = state.deviceIp;
                updateInstallProgress('Uninstalling Move Everything...', 50);
                showScreen('installing');

                const result = await window.installer.invoke('uninstall_move_everything', { hostname });

                updateInstallProgress('Complete!', 100);

                // Show success message
                setTimeout(() => {
                    const successDiv = document.querySelector('#screen-success .instruction');
                    successDiv.textContent = result.message;
                    document.querySelector('#screen-success h1').textContent = 'Uninstall Complete';
                    document.querySelector('.success-icon').textContent = '✓';
                    document.querySelector('#screen-success .info-box').style.display = 'none';
                    showScreen('success');
                }, 500);
            } catch (error) {
                console.error('Uninstall failed:', error);
                showError('Uninstall failed: ' + error.message);
            }
        }
    };

    // Module selection screen
    document.getElementById('btn-install').onclick = startInstallation;
    document.getElementById('btn-back-modules').onclick = () => {
        showScreen('discovery');
        startDeviceDiscovery();
    };

    // Success screen
    document.getElementById('btn-done').onclick = closeApplication;

    // Error screen
    document.getElementById('btn-retry').onclick = retryInstallation;
    document.getElementById('btn-diagnostics').onclick = async () => {
        try {
            const errorMessages = state.errors.map(e => `[${e.timestamp}] ${e.message}`);
            const report = await window.installer.invoke('get_diagnostics', {
                deviceIp: state.deviceIp,
                errors: errorMessages
            });

            await navigator.clipboard.writeText(report);
            alert('Diagnostics copied to clipboard');
        } catch (error) {
            console.error('Failed to generate diagnostics:', error);
            alert('Failed to copy diagnostics: ' + error);
        }
    };

    // Export logs buttons
    document.getElementById('btn-export-logs').onclick = (e) => { e.preventDefault(); exportLogs(); };
    document.getElementById('btn-export-logs-error').onclick = exportLogs;

    // Start on warning screen - user must accept before proceeding
    console.log('[DEBUG] DOM loaded, showing warning');
});
