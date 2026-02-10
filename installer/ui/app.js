// Test: Change text to confirm app.js is loading
document.getElementById('status-text').textContent = 'app.js is loading...';

// Application State
const state = {
    currentScreen: 'discovery',
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

// Device Discovery - Try move.local first, fall back to manual entry
async function startDeviceDiscovery() {
    console.log('[DEBUG] Trying move.local first...');

    const statusDiv = document.getElementById('discovery-status');
    statusDiv.innerHTML = '<div class="spinner"></div><p>Connecting to move.local...</p>';

    // Try move.local directly
    try {
        const baseUrl = 'http://move.local';
        const isValid = await window.__TAURI__.invoke('validate_device_at', { baseUrl });

        if (isValid) {
            console.log('[DEBUG] move.local validated successfully');
            statusDiv.innerHTML = '<p style="color: green;">✓ Connected to move.local</p>';

            // Automatically proceed to next step
            setTimeout(() => {
                selectDevice('move.local');
            }, 500);
            return;
        }
    } catch (error) {
        console.error('[DEBUG] move.local validation failed:', error);
    }

    // If move.local fails, show manual entry
    statusDiv.innerHTML = '<p style="color: orange;">Could not connect to move.local</p><p>Please enter your Move\'s IP address below:</p>';
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
        const isValid = await window.__TAURI__.invoke('validate_device_at', { baseUrl });

        if (isValid) {
            console.log('[DEBUG] Device validated, checking for saved cookie...');
            // Check if we have a saved cookie
            const savedCookie = await window.__TAURI__.invoke('get_saved_cookie');

            // First, test if SSH already works
            console.log('[DEBUG] Testing SSH connection...');
            const sshWorks = await window.__TAURI__.invoke('test_ssh', { hostname: hostname });

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
                await window.__TAURI__.invoke('request_challenge', { baseUrl });
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
        const cookieValue = await window.__TAURI__.invoke('submit_auth_code', {
            baseUrl: baseUrl,
            code: code
        });

        console.log('Auth successful, cookie saved');

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
        let pubkeyPath = await window.__TAURI__.invoke('find_existing_ssh_key');

        const messageEl = document.getElementById('ssh-key-message');
        if (pubkeyPath) {
            console.log('[DEBUG] Found SSH key:', pubkeyPath);
            messageEl.textContent = 'SSH key found. Ready to add it to your Move device for secure access.';
        } else {
            console.log('[DEBUG] No SSH key found');
            messageEl.textContent = 'No SSH key found. A new key will be generated and added to your Move device for secure access.';
        }

        // Store baseUrl for later
        state.baseUrl = baseUrl;

        showScreen('ssh-key');
    } catch (error) {
        showError('SSH key check failed: ' + error);
    }
}

async function proceedToSshSetup(baseUrl) {
    try {
        // Show confirmation screen FIRST (before submitting key)
        showScreen('confirm');

        // Find or generate SSH key
        console.log('[DEBUG] Looking for existing SSH key...');
        let pubkeyPath = await window.__TAURI__.invoke('find_existing_ssh_key');
        console.log('[DEBUG] Found SSH key:', pubkeyPath);

        if (!pubkeyPath) {
            console.log('[DEBUG] No SSH key found, generating new one');
            pubkeyPath = await window.__TAURI__.invoke('generate_new_ssh_key');
            console.log('[DEBUG] Generated SSH key:', pubkeyPath);
        }

        // Read public key content
        console.log('[DEBUG] Reading public key...');
        const pubkey = await window.__TAURI__.invoke('read_public_key', { path: pubkeyPath });
        console.log('[DEBUG] Public key length:', pubkey.length);
        console.log('[DEBUG] Public key preview:', pubkey.substring(0, 50) + '...');

        // Submit SSH key with auth cookie (this triggers prompt on Move)
        console.log('[DEBUG] Submitting SSH key to', baseUrl);
        await window.__TAURI__.invoke('submit_ssh_key_with_auth', {
            baseUrl: baseUrl,
            pubkey: pubkey
        });
        console.log('[DEBUG] SSH key submitted successfully');

        // Start polling for SSH access
        startConfirmationPolling();
    } catch (error) {
        showError('SSH setup failed: ' + error);
    }
}

// SSH Confirmation Polling
let confirmationPollInterval;

async function startConfirmationPolling() {
    confirmationPollInterval = setInterval(async () => {
        try {
            const hostname = 'move.local'; // Use mDNS hostname
            const connected = await window.__TAURI__.invoke('test_ssh', {
                hostname: hostname
            });

            if (connected) {
                clearInterval(confirmationPollInterval);
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

// Module Selection
async function checkVersions() {
    try {
        console.log('[DEBUG] Checking installed versions...');
        showScreen('version-check');

        // Check what's installed on device
        const hostname = state.deviceIp || 'move.local';
        const installed = await window.__TAURI__.invoke('check_installed_versions', { hostname });

        // Get latest release and module catalog
        const [latestRelease, moduleCatalog] = await Promise.all([
            window.__TAURI__.invoke('get_latest_release'),
            window.__TAURI__.invoke('get_module_catalog')
        ]);

        // Compare versions
        const versionInfo = await window.__TAURI__.invoke('compare_versions', {
            installed,
            latestRelease,
            moduleCatalog
        });

        state.versionInfo = versionInfo;
        state.installedModules = installed.modules || [];
        state.allModules = moduleCatalog;

        console.log('[DEBUG] Version info:', versionInfo);

        // Display version information
        displayVersionInfo(installed, versionInfo);

        // Hide spinner, show results
        document.getElementById('version-check-status').style.display = 'none';
        document.getElementById('version-info').style.display = 'block';

    } catch (error) {
        console.error('Version check failed:', error);
        // If version check fails, just proceed to module selection
        await loadModuleList();
        showScreen('modules');
    }
}

function preselectUpgradableModules() {
    if (!state.versionInfo) return;

    // Pre-select all upgradable modules
    const upgradableIds = state.versionInfo.upgradableModules.map(m => m.id);
    state.selectedModules = upgradableIds;

    // Update checkboxes
    upgradableIds.forEach(moduleId => {
        const checkbox = document.querySelector(`input[data-module-id="${moduleId}"]`);
        if (checkbox) {
            checkbox.checked = true;
        }
    });

    console.log('[DEBUG] Pre-selected upgradable modules:', upgradableIds);
}

function displayVersionInfo(installed, versionInfo) {
    const coreDiv = document.getElementById('core-version-info');
    const moduleDiv = document.getElementById('module-version-info');

    // Core version section
    let coreHTML = '<div class="version-section"><h3>Move Everything Core</h3>';

    if (!installed.installed) {
        coreHTML += '<p class="instruction">Move Everything is not currently installed on your device.</p>';
    } else if (versionInfo.coreUpgrade) {
        coreHTML += `
            <div class="version-item">
                <div class="version-item-name">
                    <strong>Core Framework</strong>
                    <span>${versionInfo.coreUpgrade.current} → ${versionInfo.coreUpgrade.available}</span>
                </div>
                <span class="version-badge upgrade">UPGRADE AVAILABLE</span>
            </div>
        `;
    } else if (installed.core) {
        coreHTML += `
            <div class="version-item">
                <div class="version-item-name">
                    <strong>Core Framework</strong>
                    <span>Version ${installed.core}</span>
                </div>
                <span class="version-badge current">UP TO DATE</span>
            </div>
        `;
    }

    coreHTML += '</div>';
    coreDiv.innerHTML = coreHTML;

    // Modules section
    let moduleHTML = '<div class="version-section"><h3>Modules</h3>';

    if (versionInfo.upgradableModules.length > 0) {
        moduleHTML += '<h4 style="color: #ff9900; margin: 1rem 0 0.5rem 0;">Upgrades Available</h4>';
        versionInfo.upgradableModules.forEach(module => {
            moduleHTML += `
                <div class="version-item">
                    <div class="version-item-name">
                        <strong>${module.name}</strong>
                        <span>${module.currentVersion} → ${module.version}</span>
                    </div>
                    <span class="version-badge upgrade">UPGRADE</span>
                </div>
            `;
        });
    }

    if (versionInfo.upToDateModules.length > 0) {
        moduleHTML += '<h4 style="color: #a0a0a0; margin: 1rem 0 0.5rem 0;">Up to Date</h4>';
        versionInfo.upToDateModules.forEach(module => {
            moduleHTML += `
                <div class="version-item">
                    <div class="version-item-name">
                        <strong>${module.name}</strong>
                        <span>Version ${module.currentVersion}</span>
                    </div>
                    <span class="version-badge current">CURRENT</span>
                </div>
            `;
        });
    }

    if (versionInfo.newModules.length > 0) {
        moduleHTML += '<h4 style="color: #00cc00; margin: 1rem 0 0.5rem 0;">New Modules</h4>';
        versionInfo.newModules.forEach(module => {
            moduleHTML += `
                <div class="version-item">
                    <div class="version-item-name">
                        <strong>${module.name}</strong>
                        <span>${module.description || 'Available for installation'}</span>
                    </div>
                    <span class="version-badge new">NEW</span>
                </div>
            `;
        });
    }

    if (versionInfo.upgradableModules.length === 0 &&
        versionInfo.upToDateModules.length === 0 &&
        versionInfo.newModules.length === 0) {
        moduleHTML += '<p class="instruction">No modules installed yet.</p>';
    }

    moduleHTML += '</div>';
    moduleDiv.innerHTML = moduleHTML;
}

async function loadModuleList() {
    try {
        const modules = await window.__TAURI__.invoke('get_module_catalog');
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

    // Display categories with modules
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
            checkbox.checked = true; // Default to selected
            checkbox.onchange = () => updateSelectedModules();

            const moduleInfo = document.createElement('div');
            moduleInfo.className = 'module-info';

            const moduleName = document.createElement('h4');
            moduleName.textContent = module.name;

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

function updateInstallButtonState() {
    const installButton = document.getElementById('btn-install');
    if (!installButton) return;

    // Install button is always enabled except for custom mode with no modules selected
    if (state.installType === 'custom') {
        installButton.disabled = state.selectedModules.length === 0;
    } else {
        installButton.disabled = false;
    }
}

// Installation
async function startInstallation() {
    showScreen('installing');

    try {
        // Setup SSH config
        updateInstallProgress('Setting up SSH configuration...', 0);
        await window.__TAURI__.invoke('setup_ssh_config');

        // Get latest release info
        updateInstallProgress('Fetching latest release...', 5);
        const release = await window.__TAURI__.invoke('get_latest_release');

        // Download main package
        updateInstallProgress(`Downloading ${release.asset_name}...`, 10);
        const mainTarballPath = `/tmp/${release.asset_name}`;
        await window.__TAURI__.invoke('download_release', {
            url: release.download_url,
            destPath: mainTarballPath
        });

        // Determine installation flags based on mode
        const installFlags = [];
        if (state.enableScreenReader) {
            installFlags.push('--enable-screen-reader');
        }
        if (state.installType === 'screenreader') {
            installFlags.push('--disable-shadow-ui');
            installFlags.push('--disable-standalone');
        }

        // Install/Upgrade main package
        const coreAction = (state.versionInfo && state.versionInfo.coreUpgrade) ? 'Upgrading' : 'Installing';
        updateInstallProgress(`${coreAction} Move Everything core...`, 30);
        await window.__TAURI__.invoke('install_main', {
            tarballPath: mainTarballPath,
            hostname: 'move.local',
            flags: installFlags
        });

        // Determine which modules to install
        let modulesToInstall = [];
        if (state.installType === 'complete') {
            // Install all modules
            modulesToInstall = state.allModules.map(m => m.id);
        } else if (state.installType === 'custom') {
            // Install selected modules
            modulesToInstall = state.selectedModules;
        }
        // For 'core' and 'screenreader' modes, modulesToInstall stays empty

        if (modulesToInstall.length > 0) {
            updateInstallProgress('Fetching module catalog...', 50);
            const modules = state.allModules;

            const moduleCount = modulesToInstall.length;
            const progressPerModule = 50 / moduleCount; // 50% total for all modules

            // Install each module
            for (let i = 0; i < moduleCount; i++) {
                const moduleId = modulesToInstall[i];
                const module = modules.find(m => m.id === moduleId);

                if (module) {
                    const baseProgress = 50 + (i * progressPerModule);

                    // Determine if this is an upgrade or fresh install
                    const isUpgrade = state.installedModules.some(m => m.id === module.id);
                    const action = isUpgrade ? 'Upgrading' : 'Installing';

                    updateInstallProgress(`Downloading ${module.name} (${i + 1}/${moduleCount})...`, baseProgress);
                    const moduleTarballPath = `/tmp/${module.asset_name}`;
                    await window.__TAURI__.invoke('download_release', {
                        url: module.download_url,
                        destPath: moduleTarballPath
                    });

                    updateInstallProgress(`${action} ${module.name} (${i + 1}/${moduleCount})...`, baseProgress + progressPerModule * 0.5);
                    await window.__TAURI__.invoke('install_module_package', {
                        moduleId: module.id,
                        tarballPath: moduleTarballPath,
                        componentType: module.component_type,
                        hostname: 'move.local'
                    });
                }
            }
        }

        // Installation complete
        updateInstallProgress('Installation complete!', 100);
        setTimeout(() => showScreen('success'), 500);
    } catch (error) {
        state.errors.push({
            timestamp: new Date().toISOString(),
            message: error.toString()
        });
        showError('Installation failed: ' + error);
    }
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
function showError(message) {
    state.errors.push({
        timestamp: new Date().toISOString(),
        message: message
    });
    showScreen('error');
    document.getElementById('error-message').textContent = message;
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
    console.log('[DEBUG] DOM loaded, Tauri available:', !!window.__TAURI__);

    // Warning screen
    document.getElementById('btn-accept-warning').onclick = () => {
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

    // Code entry screen
    document.getElementById('btn-submit-code').onclick = submitAuthCode;
    document.getElementById('btn-back-discovery').onclick = () => showScreen('discovery');

    // SSH Key screen
    document.getElementById('btn-add-ssh-key').onclick = () => proceedToSshSetup(state.baseUrl);
    document.getElementById('btn-back-ssh-key').onclick = () => showScreen('code-entry');

    // Confirm screen
    document.getElementById('btn-cancel-confirm').onclick = cancelConfirmation;

    // Version check screen
    document.getElementById('btn-continue-to-options').onclick = async () => {
        await loadModuleList();
        showScreen('modules');
        // Pre-select upgradable modules
        preselectUpgradableModules();
    };

    // Module selection screen
    document.getElementById('btn-install').onclick = startInstallation;
    document.getElementById('btn-back-modules').onclick = () => {
        showScreen('confirm');
    };

    // Success screen
    document.getElementById('btn-done').onclick = closeApplication;

    // Error screen
    document.getElementById('btn-retry').onclick = retryInstallation;
    document.getElementById('btn-diagnostics').onclick = async () => {
        try {
            const errorMessages = state.errors.map(e => `[${e.timestamp}] ${e.message}`);
            const report = await window.__TAURI__.invoke('get_diagnostics', {
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

    // Start on warning screen - user must accept before proceeding
    console.log('[DEBUG] DOM loaded, showing warning');
});
