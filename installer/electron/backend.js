const axios = require('axios');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Client } = require('ssh2');
const { spawn } = require('child_process');
const { promisify } = require('util');
const mkdir = promisify(fs.mkdir);
const writeFile = promisify(fs.writeFile);
const readFile = promisify(fs.readFile);
const access = promisify(fs.access);

// State management
let savedCookie = null;
const cookieStore = path.join(os.homedir(), '.move-everything-installer-cookie');

// HTTP client with cookie support
const httpClient = axios.create({
    timeout: 10000,
    validateStatus: () => true // Don't throw on non-2xx status
});

// Load saved cookie on startup
(async () => {
    try {
        if (fs.existsSync(cookieStore)) {
            savedCookie = await readFile(cookieStore, 'utf-8');
        }
    } catch (err) {
        console.error('Failed to load saved cookie:', err);
    }
})();

async function validateDevice(baseUrl) {
    try {
        const response = await httpClient.get(`${baseUrl}/`);
        return response.status === 200;
    } catch (err) {
        console.error('Device validation error:', err.message);
        return false;
    }
}

function getSavedCookie() {
    return savedCookie;
}

async function requestChallenge(baseUrl) {
    try {
        const response = await httpClient.post(`${baseUrl}/api/v1/challenge`, {});

        if (response.status !== 200) {
            throw new Error(`Challenge request failed: ${response.status}`);
        }

        return true;
    } catch (err) {
        throw new Error(`Failed to request challenge: ${err.message}`);
    }
}

async function submitAuthCode(baseUrl, code) {
    try {
        console.log('[DEBUG] Submitting auth code:', code);
        console.log('[DEBUG] Request URL:', `${baseUrl}/api/v1/challenge-response`);

        const response = await httpClient.post(`${baseUrl}/api/v1/challenge-response`, {
            secret: code
        });

        console.log('[DEBUG] Response status:', response.status);
        console.log('[DEBUG] Response data:', response.data);

        if (response.status !== 200) {
            throw new Error(`Auth failed: ${response.status} - ${JSON.stringify(response.data)}`);
        }

        // Extract Set-Cookie header
        const setCookie = response.headers['set-cookie'];
        if (setCookie && setCookie.length > 0) {
            savedCookie = setCookie[0].split(';')[0];
            await writeFile(cookieStore, savedCookie);
            return savedCookie;
        }

        throw new Error('No cookie returned from auth');
    } catch (err) {
        throw new Error(`Failed to submit auth code: ${err.message}`);
    }
}

function findExistingSshKey() {
    const sshDir = path.join(os.homedir(), '.ssh');

    // Prefer move_key.pub (ED25519) over id_rsa.pub
    const moveKeyPath = path.join(sshDir, 'move_key.pub');
    if (fs.existsSync(moveKeyPath)) {
        console.log('[DEBUG] Found move_key.pub');
        return moveKeyPath;
    }

    const rsaKeyPath = path.join(sshDir, 'id_rsa.pub');
    if (fs.existsSync(rsaKeyPath)) {
        console.log('[DEBUG] Found id_rsa.pub');
        return rsaKeyPath;
    }

    return null;
}

async function generateNewSshKey() {
    const sshDir = path.join(os.homedir(), '.ssh');
    const keyPath = path.join(sshDir, 'move_key');

    // Ensure .ssh directory exists
    await mkdir(sshDir, { recursive: true });

    return new Promise((resolve, reject) => {
        const keygen = spawn('ssh-keygen', [
            '-t', 'ed25519',
            '-f', keyPath,
            '-N', '', // No passphrase
            '-C', 'move-everything-installer'
        ]);

        keygen.on('close', (code) => {
            if (code === 0) {
                resolve(`${keyPath}.pub`);
            } else {
                reject(new Error(`ssh-keygen failed with code ${code}`));
            }
        });

        keygen.on('error', (err) => {
            reject(new Error(`Failed to run ssh-keygen: ${err.message}`));
        });
    });
}

async function readPublicKey(keyPath) {
    try {
        return await readFile(keyPath, 'utf-8');
    } catch (err) {
        throw new Error(`Failed to read public key: ${err.message}`);
    }
}

async function submitSshKeyWithAuth(baseUrl, pubkey) {
    try {
        if (!savedCookie) {
            throw new Error('No auth cookie available');
        }

        console.log('[DEBUG] Submitting SSH key');
        console.log('[DEBUG] Cookie:', savedCookie);

        // Remove comment from SSH key (everything after the last space)
        const keyParts = pubkey.trim().split(' ');
        const keyWithoutComment = keyParts.slice(0, 2).join(' '); // Keep only "ssh-rsa AAAA..."

        console.log('[DEBUG] Pubkey length:', keyWithoutComment.length);
        console.log('[DEBUG] Pubkey content:', keyWithoutComment);

        // Send SSH key as raw POST body (not form field)
        const response = await httpClient.post(`${baseUrl}/api/v1/ssh`, keyWithoutComment, {
            headers: {
                'Cookie': savedCookie,
                'Content-Type': 'application/x-www-form-urlencoded'
            }
        });

        console.log('[DEBUG] SSH Response status:', response.status);
        console.log('[DEBUG] SSH Response data:', response.data);

        if (response.status !== 200) {
            throw new Error(`SSH key submission failed: ${response.status} - ${JSON.stringify(response.data)}`);
        }

        return true;
    } catch (err) {
        throw new Error(`Failed to submit SSH key: ${err.message}`);
    }
}

async function testSsh(hostname) {
    // Use move_key if it exists, otherwise id_rsa
    const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
    const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

    if (!fs.existsSync(keyPath)) {
        console.log('[DEBUG] No SSH key found for testing');
        return false;
    }

    const privateKey = fs.readFileSync(keyPath);

    // Try ableton@move.local first, then root@move.local
    const users = ['ableton', 'root'];

    for (const username of users) {
        console.log(`[DEBUG] Testing SSH as ${username}@${hostname}...`);

        const connected = await new Promise((resolve) => {
            const conn = new Client();

            const timeout = setTimeout(() => {
                conn.end();
                resolve(false);
            }, 5000);

            conn.on('ready', () => {
                clearTimeout(timeout);

                // If connected as ableton, fix authorized_keys permissions
                if (username === 'ableton') {
                    console.log('[DEBUG] Connected as ableton, fixing authorized_keys permissions');
                    conn.exec('chmod 600 ~/.ssh/authorized_keys && chmod 700 ~/.ssh', (err) => {
                        conn.end();
                        resolve(true);
                    });
                } else {
                    conn.end();
                    resolve(true);
                }
            });

            conn.on('error', (err) => {
                clearTimeout(timeout);
                console.log(`[DEBUG] SSH error for ${username}:`, err.message);
                resolve(false);
            });

            try {
                conn.connect({
                    host: hostname,
                    port: 22,
                    username: username,
                    privateKey: privateKey,
                    readyTimeout: 5000
                });
            } catch (err) {
                clearTimeout(timeout);
                resolve(false);
            }
        });

        if (connected) {
            console.log(`[DEBUG] SSH works as ${username}@${hostname}`);
            return true;
        }
    }

    console.log('[DEBUG] SSH failed for all users');
    return false;
}

async function setupSshConfig() {
    const sshDir = path.join(os.homedir(), '.ssh');
    const configPath = path.join(sshDir, 'config');

    const configEntry = `
Host move.local
    HostName move.local
    User root
    IdentityFile ~/.ssh/id_rsa
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
`;

    try {
        let existingConfig = '';
        if (fs.existsSync(configPath)) {
            existingConfig = await readFile(configPath, 'utf-8');
        }

        if (!existingConfig.includes('Host move.local')) {
            await writeFile(configPath, existingConfig + configEntry);
        }
    } catch (err) {
        throw new Error(`Failed to setup SSH config: ${err.message}`);
    }
}

async function getModuleCatalog() {
    try {
        const response = await httpClient.get(
            'https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json'
        );

        if (response.status !== 200) {
            throw new Error(`Failed to fetch catalog: ${response.status}`);
        }

        console.log('[DEBUG] Catalog response type:', typeof response.data);
        console.log('[DEBUG] Catalog response:', response.data);

        let catalog = response.data;

        // If it's a string, parse it
        if (typeof catalog === 'string') {
            catalog = JSON.parse(catalog);
        }

        console.log('[DEBUG] Parsed catalog is array:', Array.isArray(catalog));

        // Handle v2 catalog format
        const moduleList = catalog.modules || catalog;

        // For each module, get latest release info
        const modules = await Promise.all(moduleList.map(async (module) => {
            try {
                const releaseUrl = `https://api.github.com/repos/${module.github_repo}/releases/latest`;
                const releaseResponse = await httpClient.get(releaseUrl);

                if (releaseResponse.status === 200) {
                    const release = releaseResponse.data;
                    const asset = release.assets.find(a => a.name === module.asset_name);

                    if (asset) {
                        return {
                            ...module,
                            version: release.tag_name,
                            download_url: asset.browser_download_url
                        };
                    }
                }
            } catch (err) {
                console.error(`Failed to get release for ${module.id}:`, err.message);
            }

            return module;
        }));

        return modules;
    } catch (err) {
        throw new Error(`Failed to get module catalog: ${err.message}`);
    }
}

async function getLatestRelease() {
    try {
        const response = await httpClient.get(
            'https://api.github.com/repos/charlesvestal/move-anything/releases/latest'
        );

        if (response.status !== 200) {
            throw new Error(`Failed to fetch release: ${response.status}`);
        }

        const release = response.data;
        const asset = release.assets.find(a => a.name.includes('move-anything') && a.name.endsWith('.tar.gz'));

        if (!asset) {
            throw new Error('No suitable asset found in release');
        }

        return {
            version: release.tag_name,
            asset_name: asset.name,
            download_url: asset.browser_download_url
        };
    } catch (err) {
        throw new Error(`Failed to get latest release: ${err.message}`);
    }
}

async function downloadRelease(url, destPath) {
    try {
        const response = await httpClient.get(url, {
            responseType: 'stream'
        });

        const writer = fs.createWriteStream(destPath);
        response.data.pipe(writer);

        return new Promise((resolve, reject) => {
            writer.on('finish', () => resolve(true));
            writer.on('error', reject);
        });
    } catch (err) {
        throw new Error(`Failed to download release: ${err.message}`);
    }
}

async function sshExec(hostname, command) {
    return new Promise((resolve, reject) => {
        const conn = new Client();

        conn.on('ready', () => {
            conn.exec(command, (err, stream) => {
                if (err) {
                    conn.end();
                    return reject(err);
                }

                let stdout = '';
                let stderr = '';

                stream.on('data', (data) => {
                    stdout += data.toString();
                });

                stream.stderr.on('data', (data) => {
                    stderr += data.toString();
                });

                stream.on('close', (code) => {
                    conn.end();
                    if (code === 0) {
                        resolve(stdout);
                    } else {
                        reject(new Error(`Command failed with code ${code}: ${stderr}`));
                    }
                });
            });
        });

        conn.on('error', (err) => {
            reject(err);
        });

        // Use move_key if it exists, otherwise id_rsa
        const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
        const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

        conn.connect({
            host: hostname,
            port: 22,
            username: 'root',
            privateKey: fs.readFileSync(keyPath)
        });
    });
}

async function installMain(tarballPath, hostname) {
    try {
        const filename = path.basename(tarballPath);

        // Use move_key if exists, otherwise id_rsa
        const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
        const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

        // Upload to home directory (ableton@move.local = /data/UserData/)
        await new Promise((resolve, reject) => {
            const scp = spawn('scp', [
                '-i', keyPath,
                '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null',
                tarballPath,
                `ableton@${hostname}:${filename}`
            ]);

            scp.on('close', (code) => {
                if (code === 0) resolve();
                else reject(new Error(`scp failed with code ${code}`));
            });
        });

        // Extract tarball (creates move-anything/ directory in home)
        await sshExec(hostname, `tar -xzf ${filename}`);

        // Run install script
        await sshExec(hostname, `cd move-anything && ./scripts/install.sh local --skip-modules`);

        return true;
    } catch (err) {
        throw new Error(`Installation failed: ${err.message}`);
    }
}

async function installModulePackage(moduleId, tarballPath, componentType, hostname) {
    try {
        const filename = path.basename(tarballPath);

        // Use move_key if exists, otherwise id_rsa
        const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
        const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

        // Upload to home directory
        await new Promise((resolve, reject) => {
            const scp = spawn('scp', [
                '-i', keyPath,
                '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null',
                tarballPath,
                `ableton@${hostname}:move-anything/${filename}`
            ]);

            scp.on('close', (code) => {
                if (code === 0) resolve();
                else reject(new Error(`scp failed with code ${code}`));
            });
        });

        // Extract and install module (similar to install.sh module installation)
        const categoryPath = componentType ? `${componentType}s` : 'utility';
        await sshExec(hostname, `cd move-anything && mkdir -p ${categoryPath} && tar -xzf ${filename} -C ${categoryPath}/ && rm ${filename}`);

        return true;
    } catch (err) {
        throw new Error(`Module installation failed: ${err.message}`);
    }
}

function getDiagnostics(deviceIp, errors) {
    const diagnostics = {
        timestamp: new Date().toISOString(),
        platform: os.platform(),
        arch: os.arch(),
        deviceIp,
        errors,
        sshKeyExists: fs.existsSync(path.join(os.homedir(), '.ssh', 'id_rsa')),
        hasCookie: !!savedCookie
    };

    return JSON.stringify(diagnostics, null, 2);
}

module.exports = {
    validateDevice,
    getSavedCookie,
    requestChallenge,
    submitAuthCode,
    findExistingSshKey,
    generateNewSshKey,
    readPublicKey,
    submitSshKeyWithAuth,
    testSsh,
    setupSshConfig,
    getModuleCatalog,
    getLatestRelease,
    downloadRelease,
    installMain,
    installModulePackage,
    getDiagnostics
};
