const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const backend = require('./backend');

let mainWindow;

function createWindow() {
    mainWindow = new BrowserWindow({
        width: 800,
        height: 600,
        webPreferences: {
            preload: path.join(__dirname, 'preload.js'),
            nodeIntegration: false,
            contextIsolation: true
        }
    });

    mainWindow.loadFile(path.join(__dirname, '../ui/index.html'));

    // Set main window in backend for logging
    backend.setMainWindow(mainWindow);

    // Always open DevTools for debugging
    mainWindow.webContents.openDevTools();
}

app.whenReady().then(() => {
    createWindow();

    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) {
            createWindow();
        }
    });
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') {
        app.quit();
    }
});

// IPC Handlers
ipcMain.handle('validate_device_at', async (event, { baseUrl }) => {
    return await backend.validateDevice(baseUrl);
});

ipcMain.handle('get_saved_cookie', async () => {
    return backend.getSavedCookie();
});

ipcMain.handle('request_challenge', async (event, { baseUrl }) => {
    return await backend.requestChallenge(baseUrl);
});

ipcMain.handle('submit_auth_code', async (event, { baseUrl, code }) => {
    return await backend.submitAuthCode(baseUrl, code);
});

ipcMain.handle('find_existing_ssh_key', async () => {
    return backend.findExistingSshKey();
});

ipcMain.handle('generate_new_ssh_key', async () => {
    return await backend.generateNewSshKey();
});

ipcMain.handle('read_public_key', async (event, { path }) => {
    return backend.readPublicKey(path);
});

ipcMain.handle('submit_ssh_key_with_auth', async (event, { baseUrl, pubkey }) => {
    return await backend.submitSshKeyWithAuth(baseUrl, pubkey);
});

ipcMain.handle('test_ssh', async (event, { hostname }) => {
    return await backend.testSsh(hostname);
});

ipcMain.handle('setup_ssh_config', async () => {
    return backend.setupSshConfig();
});

ipcMain.handle('check_git_bash_available', async () => {
    return await backend.checkGitBashAvailable();
});

ipcMain.handle('get_module_catalog', async (event, { installedModuleIds = [] } = {}) => {
    return await backend.getModuleCatalog(installedModuleIds);
});

ipcMain.handle('get_latest_release', async () => {
    return await backend.getLatestRelease();
});

ipcMain.handle('download_release', async (event, { url, destPath }) => {
    return await backend.downloadRelease(url, destPath);
});

ipcMain.handle('install_main', async (event, { tarballPath, hostname, flags }) => {
    return await backend.installMain(tarballPath, hostname, flags);
});

ipcMain.handle('install_module_package', async (event, { moduleId, tarballPath, componentType, hostname }) => {
    return await backend.installModulePackage(moduleId, tarballPath, componentType, hostname);
});

ipcMain.handle('check_core_installation', async (event, { hostname }) => {
    return await backend.checkCoreInstallation(hostname);
});

ipcMain.handle('check_installed_versions', async (event, { hostname }) => {
    // Create progress callback that sends events to frontend
    const progressCallback = (message) => {
        event.sender.send('version-check-progress', message);
    };
    return await backend.checkInstalledVersions(hostname, progressCallback);
});

ipcMain.handle('compare_versions', async (event, { installed, latestRelease, moduleCatalog }) => {
    return backend.compareVersions(installed, latestRelease, moduleCatalog);
});

ipcMain.handle('get_diagnostics', async (event, { deviceIp, errors }) => {
    return backend.getDiagnostics(deviceIp, errors);
});

ipcMain.handle('get_screen_reader_status', async (event, { hostname }) => {
    return await backend.getScreenReaderStatus(hostname);
});

ipcMain.handle('set_screen_reader_state', async (event, { hostname, enabled }) => {
    return await backend.setScreenReaderState(hostname, enabled);
});

ipcMain.handle('uninstall_move_everything', async (event, { hostname }) => {
    return await backend.uninstallMoveEverything(hostname);
});

ipcMain.handle('test_ssh_formats', async (event, { cookie }) => {
    return await backend.testSshFormats(cookie);
});
