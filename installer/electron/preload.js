const { contextBridge, ipcRenderer } = require('electron');

// Expose installer API
contextBridge.exposeInMainWorld('installer', {
    // IPC invoke wrapper
    invoke: (cmd, args = {}) => {
        return ipcRenderer.invoke(cmd, args);
    },

    // Event listeners
    on: (channel, callback) => {
        const validChannels = ['version-check-progress', 'backend-log'];
        if (validChannels.includes(channel)) {
            ipcRenderer.on(channel, (event, ...args) => callback(...args));
        }
    },

    removeAllListeners: (channel) => {
        ipcRenderer.removeAllListeners(channel);
    }
});
