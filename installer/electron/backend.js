const axios = require('axios');
const fs = require('fs');
const os = require('os');
const path = require('path');
const dns = require('dns');
const { Client } = require('ssh2');
const { spawn } = require('child_process');
const { promisify } = require('util');
const mkdir = promisify(fs.mkdir);
const writeFile = promisify(fs.writeFile);
const readFile = promisify(fs.readFile);
const access = promisify(fs.access);
const dnsResolve4 = promisify(dns.resolve4);

// State management
let savedCookie = null;
const cookieStore = path.join(os.homedir(), '.move-everything-installer-cookie');

// HTTP client with cookie support
const httpClient = axios.create({
    timeout: 60000, // 60 seconds for user interactions
    validateStatus: () => true, // Don't throw on non-2xx status
    family: 4 // Force IPv4
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

// Cache device IP for current session only (not persisted between runs)
let cachedDeviceIp = null;

async function validateDevice(baseUrl) {
    try {
        // Extract hostname from baseUrl
        const url = new URL(baseUrl);
        const hostname = url.hostname;

        // For .local domains, use system resolver to get IPv4 (mDNS)
        if (!cachedDeviceIp) {
            try {
                // Windows: Rely on Bonjour services (from iTunes/iCloud) - just use hostname
                if (process.platform === 'win32') {
                    console.log(`[DEBUG] Windows: Using hostname directly (Bonjour will resolve)`);
                    cachedDeviceIp = hostname;
                } else {
                    // macOS/Linux: Use system resolver to get IPv4
                    const { stdout } = await new Promise((resolve, reject) => {
                        const cmd = process.platform === 'darwin'
                            ? `dscacheutil -q host -a name ${hostname} | grep ip_address | head -1 | awk '{print $2}'`
                            : `getent ahostsv4 ${hostname} | head -1 | awk '{print $1}'`;

                        require('child_process').exec(cmd, (err, stdout, stderr) => {
                            if (err) reject(err);
                            else resolve({ stdout });
                        });
                    });

                    const ip = stdout.trim();
                    if (ip && /^\d+\.\d+\.\d+\.\d+$/.test(ip)) {
                        cachedDeviceIp = ip;
                        console.log(`[DEBUG] Resolved ${hostname} to ${cachedDeviceIp} (IPv4)`);
                    } else {
                        throw new Error('No IPv4 address found');
                    }
                }
            } catch (err) {
                console.log(`[DEBUG] mDNS resolution failed: ${err.message}, using hostname`);
                cachedDeviceIp = hostname;
            }
        }

        // Use cached IP for validation
        const validateUrl = cachedDeviceIp ? `http://${cachedDeviceIp}/` : baseUrl;
        const response = await httpClient.get(validateUrl);

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

    // Use cached IP from HTTP connection first, then try DNS
    let hostIp = cachedDeviceIp || hostname;
    if (!cachedDeviceIp) {
        try {
            const addresses = await dnsResolve4(hostname);
            if (addresses && addresses.length > 0) {
                hostIp = addresses[0];
                console.log(`[DEBUG] Resolved ${hostname} to IPv4: ${hostIp}`);
            }
        } catch (err) {
            console.log(`[DEBUG] DNS resolution failed: ${err.message}`);
            // Fall back to hostname as-is
            hostIp = hostname;
        }
    } else {
        console.log(`[DEBUG] Using cached IP: ${hostIp}`);
    }

    // Try ableton@move.local first, then root@move.local
    const users = ['ableton', 'root'];

    for (const username of users) {
        console.log(`[DEBUG] Testing SSH as ${username}@${hostIp}...`);

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
                    host: hostIp,
                    port: 22,
                    username: username,
                    privateKey: privateKey,
                    readyTimeout: 5000,
                    family: 4  // Force IPv4
                });
            } catch (err) {
                clearTimeout(timeout);
                resolve(false);
            }
        });

        if (connected) {
            console.log(`[DEBUG] SSH works as ${username}@${hostIp}`);
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

        let catalog = response.data;

        // If it's a string, parse it
        if (typeof catalog === 'string') {
            catalog = JSON.parse(catalog);
        }

        // Handle v2 catalog format
        const moduleList = catalog.modules || catalog;

        // For each module, construct direct download URL (avoid GitHub API rate limits)
        const modules = moduleList.map((module) => {
            // Construct direct download URL like install.sh does
            const downloadUrl = `https://github.com/${module.github_repo}/releases/latest/download/${module.asset_name}`;

            return {
                ...module,
                version: 'latest',
                download_url: downloadUrl
            };
        });

        return modules;
    } catch (err) {
        throw new Error(`Failed to get module catalog: ${err.message}`);
    }
}

async function getLatestRelease() {
    try {
        // Use direct download URL like install.sh does (no API, no rate limits)
        const assetName = 'move-anything.tar.gz';
        const downloadUrl = `https://github.com/charlesvestal/move-anything/releases/latest/download/${assetName}`;

        return {
            version: 'latest',
            asset_name: assetName,
            download_url: downloadUrl
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
    // Use cached IP from session (already resolved in validateDevice)
    const hostIp = cachedDeviceIp || hostname;

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
            host: hostIp,
            port: 22,
            username: 'root',
            privateKey: fs.readFileSync(keyPath),
            family: 4  // Force IPv4
        });
    });
}

async function installMain(tarballPath, hostname) {
    try {
        const tmpDir = path.join(os.tmpdir(), 'move-installer-' + Date.now());
        const scriptsDir = path.join(tmpDir, 'scripts');
        await mkdir(scriptsDir, { recursive: true });

        // Copy tarball to tmpDir (install.sh expects it at $REPO_ROOT/move-anything.tar.gz)
        const tarballDest = path.join(tmpDir, 'move-anything.tar.gz');
        fs.copyFileSync(tarballPath, tarballDest);
        console.log('[DEBUG] Copied tarball to:', tarballDest);

        // Download install.sh from repo to scripts/ directory
        console.log('[DEBUG] Downloading install.sh from repo...');
        const installShUrl = 'https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh';
        const installScriptPath = path.join(scriptsDir, 'install.sh');

        const scriptResponse = await httpClient.get(installShUrl);
        if (scriptResponse.status !== 200) {
            throw new Error(`Failed to download install.sh: ${scriptResponse.status}`);
        }

        await writeFile(installScriptPath, scriptResponse.data);
        fs.chmodSync(installScriptPath, '755');

        // Run install.sh in local mode from tmpDir
        // Directory structure now matches local repo:
        //   tmpDir/
        //     scripts/install.sh
        //     move-anything.tar.gz
        // Use cached IP for faster connection (avoid mDNS lookup)
        const targetHost = cachedDeviceIp || hostname;
        console.log('[DEBUG] Running install.sh from:', tmpDir);
        console.log('[DEBUG] Target host:', targetHost, '(cached:', !!cachedDeviceIp + ')');
        await new Promise((resolve, reject) => {
            const installScript = spawn('bash', ['scripts/install.sh', 'local', '--skip-modules', '--skip-confirmation'], {
                cwd: tmpDir,
                env: {
                    ...process.env,
                    MOVE_HOSTNAME: targetHost
                }
            });

            installScript.stdout.on('data', (data) => {
                console.log('[install.sh]', data.toString().trim());
            });

            installScript.stderr.on('data', (data) => {
                console.error('[install.sh]', data.toString().trim());
            });

            installScript.on('close', (code) => {
                if (code === 0) resolve();
                else reject(new Error(`install.sh failed with code ${code}`));
            });

            installScript.on('error', reject);
        });

        // Cleanup
        await new Promise((resolve) => {
            spawn('rm', ['-rf', tmpDir]).on('close', resolve);
        });

        return true;
    } catch (err) {
        throw new Error(`Installation failed: ${err.message}`);
    }
}

async function installModulePackage(moduleId, tarballPath, componentType, hostname) {
    try {
        console.log(`[DEBUG] Installing module ${moduleId} (${componentType})`);
        const filename = path.basename(tarballPath);

        // Use cached IP instead of hostname for faster connection
        const targetHost = cachedDeviceIp || hostname;
        console.log(`[DEBUG] Using host: ${targetHost} (cached: ${!!cachedDeviceIp})`);

        // Use move_key if exists, otherwise id_rsa
        const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
        const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');
        console.log(`[DEBUG] Using SSH key: ${path.basename(keyPath)}`);

        // Upload to Move Everything directory
        console.log(`[DEBUG] Uploading ${filename} to device...`);
        await new Promise((resolve, reject) => {
            const scp = spawn('scp', [
                '-i', keyPath,
                '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null',
                tarballPath,
                `ableton@${targetHost}:/data/UserData/move-anything/${filename}`
            ]);

            let stderr = '';
            scp.stderr.on('data', (data) => {
                stderr += data.toString();
            });

            scp.on('close', (code) => {
                if (code === 0) {
                    console.log(`[DEBUG] Upload complete for ${moduleId}`);
                    resolve();
                } else {
                    console.error(`[DEBUG] scp failed: ${stderr}`);
                    reject(new Error(`scp failed with code ${code}: ${stderr}`));
                }
            });
        });

        // Extract and install module (similar to install.sh module installation)
        const categoryPath = componentType ? `${componentType}s` : 'utility';
        console.log(`[DEBUG] Extracting ${moduleId} to modules/${categoryPath}/`);
        await sshExec(targetHost, `cd /data/UserData/move-anything && mkdir -p modules/${categoryPath} && tar -xzf ${filename} -C modules/${categoryPath}/ && rm ${filename}`);
        console.log(`[DEBUG] Module ${moduleId} installed successfully`);

        return true;
    } catch (err) {
        console.error(`[DEBUG] Module installation error for ${moduleId}:`, err.message);
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
