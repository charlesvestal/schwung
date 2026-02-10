// Application State
const state = {
    currentScreen: 'discovery',
    deviceIp: null,
    authCode: null,
    selectedModules: [],
    sshPassword: null,
    errors: []
};

// Screen Management
function showScreen(screenName) {
    document.querySelectorAll('.screen').forEach(screen => {
        screen.classList.remove('active');
    });
    document.getElementById(`screen-${screenName}`).classList.add('active');
    state.currentScreen = screenName;
}

// Device Discovery
async function startDeviceDiscovery() {
    try {
        const devices = await window.__TAURI__.invoke('discover_devices');
        displayDevices(devices);
    } catch (error) {
        console.error('Discovery failed:', error);
        showError('Failed to discover devices: ' + error);
    }
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

async function selectDevice(ip) {
    state.deviceIp = ip;

    try {
        // Initiate SSH setup
        const result = await window.__TAURI__.invoke('setup_ssh', { deviceIp: ip });

        if (result.success) {
            showScreen('code-entry');
            setupCodeEntry();
        } else {
            showError('Failed to initiate SSH setup: ' + result.error);
        }
    } catch (error) {
        showError('Connection failed: ' + error);
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
        const result = await window.__TAURI__.invoke('submit_auth_code', {
            deviceIp: state.deviceIp,
            code: code
        });

        if (result.success) {
            showScreen('confirm');
            startConfirmationPolling();
        } else {
            showError('Failed to submit code: ' + result.error);
        }
    } catch (error) {
        showError('Failed to submit code: ' + error);
    }
}

// SSH Confirmation Polling
let confirmationPollInterval;

async function startConfirmationPolling() {
    confirmationPollInterval = setInterval(async () => {
        try {
            const result = await window.__TAURI__.invoke('check_ssh_confirmed', {
                deviceIp: state.deviceIp
            });

            if (result.confirmed) {
                clearInterval(confirmationPollInterval);
                state.sshPassword = result.password;
                await loadModuleList();
                showScreen('modules');
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
async function loadModuleList() {
    try {
        const modules = await window.__TAURI__.invoke('get_module_catalog');
        displayModules(modules);
    } catch (error) {
        console.error('Failed to load modules:', error);
        showError('Failed to load module list: ' + error);
    }
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

    const installButton = document.getElementById('btn-install');
    installButton.disabled = state.selectedModules.length === 0;
}

// Installation
async function startInstallation() {
    showScreen('installing');

    try {
        // Listen for progress updates
        await window.__TAURI__.event.listen('install_progress', (event) => {
            updateInstallProgress(event.payload);
        });

        const result = await window.__TAURI__.invoke('install_move_everything', {
            deviceIp: state.deviceIp,
            password: state.sshPassword,
            modules: state.selectedModules
        });

        if (result.success) {
            showInstallSuccess();
        } else {
            showError('Installation failed: ' + result.error);
        }
    } catch (error) {
        showError('Installation failed: ' + error);
    }
}

function updateInstallProgress(progress) {
    const progressFill = document.querySelector('.progress-fill');
    const statusText = document.getElementById('install-status');
    const logDiv = document.getElementById('install-log');

    // Update progress bar
    if (progress.percent !== undefined) {
        progressFill.style.width = `${progress.percent}%`;
    }

    // Update status text
    if (progress.status) {
        statusText.textContent = progress.status;
    }

    // Add log entry
    if (progress.message) {
        const logEntry = document.createElement('div');
        logEntry.className = 'log-entry';
        if (progress.error) {
            logEntry.classList.add('error');
        }
        logEntry.textContent = progress.message;
        logDiv.appendChild(logEntry);
        logDiv.scrollTop = logDiv.scrollHeight;
    }
}

function showInstallSuccess() {
    showScreen('success');
    document.getElementById('device-ip').textContent = state.deviceIp;
    document.getElementById('ssh-password').textContent = state.sshPassword;
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
async function copyCredentialsToClipboard() {
    const text = `SSH Access Details\nIP: ${state.deviceIp}\nUsername: root\nPassword: ${state.sshPassword}`;

    try {
        await navigator.clipboard.writeText(text);
        const button = document.getElementById('btn-copy-credentials');
        const originalText = button.textContent;
        button.textContent = 'Copied!';
        setTimeout(() => {
            button.textContent = originalText;
        }, 2000);
    } catch (error) {
        console.error('Failed to copy:', error);
    }
}

function closeApplication() {
    window.__TAURI__.process.exit(0);
}

// Event Listeners
document.addEventListener('DOMContentLoaded', () => {
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

    // Confirm screen
    document.getElementById('btn-cancel-confirm').onclick = cancelConfirmation;

    // Module selection screen
    document.getElementById('btn-install').onclick = startInstallation;
    document.getElementById('btn-back-code').onclick = () => {
        cancelConfirmation();
        showScreen('code-entry');
    };

    // Success screen
    document.getElementById('btn-copy-credentials').onclick = copyCredentialsToClipboard;
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

    // Start discovery on load
    startDeviceDiscovery();
});
