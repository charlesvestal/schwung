const { contextBridge, ipcRenderer } = require('electron');

// Expose protected methods that allow the renderer process to use
// the ipcRenderer without exposing the entire object
contextBridge.exposeInMainWorld('__TAURI_INTERNALS__', {
    invoke: (cmd, args = {}) => {
        return ipcRenderer.invoke(cmd, args);
    }
});
