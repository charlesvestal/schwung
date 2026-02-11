const axios = require('axios');
const fs = require('fs');
const os = require('os');
const path = require('path');
const dns = require('dns');
const crypto = require('crypto');
const { Client } = require('ssh2');
const { promisify } = require('util');
const mkdir = promisify(fs.mkdir);
const writeFile = promisify(fs.writeFile);
const readFile = promisify(fs.readFile);
const copyFile = promisify(fs.copyFile);
const access = promisify(fs.access);
const rm = promisify(fs.rm);
const dnsResolve4 = promisify(dns.resolve4);

// State management
let savedCookie = null;
const cookieStore = path.join(os.homedir(), '.move-everything-installer-cookie');

// Store reference to main window for logging
let mainWindowForLogging = null;

function setMainWindow(win) {
    mainWindowForLogging = win;
}

// Override console.log to also send to renderer
const originalLog = console.log;
console.log = function(...args) {
    originalLog.apply(console, args);
    if (mainWindowForLogging && mainWindowForLogging.webContents) {
        mainWindowForLogging.webContents.send('backend-log', args.join(' '));
    }
};

const originalError = console.error;
console.error = function(...args) {
    originalError.apply(console, args);
    if (mainWindowForLogging && mainWindowForLogging.webContents) {
        mainWindowForLogging.webContents.send('backend-log', '[ERROR] ' + args.join(' '));
    }
};

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

        // Check if hostname is already an IP address
        const isIpAddress = /^\d+\.\d+\.\d+\.\d+$/.test(hostname);

        // If we already have a cached IP, use it (don't reset for same device)
        // Only reset if user explicitly enters a DIFFERENT IP address
        if (cachedDeviceIp) {
            if (isIpAddress && cachedDeviceIp !== hostname) {
                console.log(`[DEBUG] User entered different IP, resetting cache (was ${cachedDeviceIp}, now ${hostname})`);
                cachedDeviceIp = null;
            } else {
                console.log(`[DEBUG] Using cached IP: ${cachedDeviceIp}`);
                // Don't resolve again, use cached IP
            }
        }

        // For .local domains or non-IP hostnames, resolve to IP first
        if (!cachedDeviceIp) {
            if (isIpAddress) {
                // Already an IP, use it directly
                console.log(`[DEBUG] Using IP address directly: ${hostname}`);
                cachedDeviceIp = hostname;
            } else {
                // Try DNS resolution
                try {
                    console.log(`[DEBUG] Resolving ${hostname} to IP...`);

                    if (process.platform === 'win32') {
                        // Windows: Node.js can't resolve .local, use ping to get IP (IPv4 or IPv6)
                        console.log(`[DEBUG] Windows: Using ping to resolve .local domain...`);
                        const { exec } = require('child_process');
                        const { promisify } = require('util');
                        const execAsync = promisify(exec);

                        // Ping once to get the IP address
                        const { stdout } = await execAsync(`ping -n 1 ${hostname}`, { timeout: 5000 });

                        // Try to extract IPv4 first (192.168.x.x)
                        let ipMatch = stdout.match(/\[?(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})\]?/);
                        if (ipMatch) {
                            cachedDeviceIp = ipMatch[1];
                            console.log(`[DEBUG] Resolved ${hostname} to IPv4: ${cachedDeviceIp}`);
                        } else {
                            // Extract IPv6 (2003:ed:5f03:...)
                            ipMatch = stdout.match(/\[([0-9a-f:]+)\]/i);
                            if (ipMatch) {
                                cachedDeviceIp = `[${ipMatch[1]}]`;  // Keep brackets for URL formatting
                                console.log(`[DEBUG] Resolved ${hostname} to IPv6: ${cachedDeviceIp}`);
                            } else {
                                throw new Error('Could not parse IP from ping output');
                            }
                        }
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
                    // Don't cache hostname - leave cachedDeviceIp as null
                    console.log(`[DEBUG] DNS resolution failed: ${err.message}`);
                    console.log(`[DEBUG] Could not resolve ${hostname} to IPv4 address`);
                }
            }
        }

        // Simple HTTP validation only (SSH keys may not be set up yet)
        const validateUrl = cachedDeviceIp ? `http://${cachedDeviceIp}/` : baseUrl;
        console.log(`[DEBUG] Validating via HTTP: ${validateUrl}`);

        try {
            // If we don't have an IP yet, try to extract it from the HTTP connection
            if (!cachedDeviceIp && hostname.endsWith('.local')) {
                console.log(`[DEBUG] Attempting to get IP from HTTP connection...`);

                // Use Node's http module directly to access socket info
                const http = require('http');
                const connectedIp = await new Promise((resolve, reject) => {
                    const req = http.get(validateUrl, { timeout: 10000 }, (res) => {
                        const socketIp = res.socket.remoteAddress;
                        console.log(`[DEBUG] HTTP connected to IP: ${socketIp}`);
                        resolve(socketIp);
                        res.resume(); // Consume response
                    });
                    req.on('error', reject);
                    req.on('timeout', () => reject(new Error('HTTP connection timeout')));
                });

                if (connectedIp) {
                    // Clean up IPv6 formatting if needed (remove ::ffff: prefix)
                    let cleanIp = connectedIp.replace(/^::ffff:/, '');
                    cachedDeviceIp = cleanIp;
                    console.log(`[DEBUG] Cached IP from HTTP connection: ${cachedDeviceIp}`);
                }
            } else {
                // Just validate normally
                const response = await httpClient.get(validateUrl, { timeout: 10000 });
                console.log(`[DEBUG] HTTP validation successful`);
            }

            return true;
        } catch (err) {
            console.log(`[DEBUG] HTTP validation failed: ${err.message}`);
            return false;
        }
    } catch (err) {
        console.error('Device validation error:', err.message);
        return false;
    }
}

async function discoverIpViaSsh(hostname) {
    try {
        const { exec } = require('child_process');
        const { promisify } = require('util');
        const execAsync = promisify(exec);

        // Use SSH with verbose output to see the resolved IP
        // Try to connect and immediately exit, capture the verbose output
        const sshCmd = `ssh -v -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o BatchMode=yes ableton@${hostname} exit 2>&1`;
        const { stdout, stderr } = await execAsync(sshCmd, { timeout: 10000 });

        // SSH writes debug info to stderr, look for "Connecting to" or "Connected to"
        const output = stdout + stderr;

        // Parse IP from output like "Connecting to move.local [192.168.1.100]"
        const ipMatch = output.match(/Connecting to [^\[]+\[(\d+\.\d+\.\d+\.\d+)\]/);
        if (ipMatch && ipMatch[1]) {
            return ipMatch[1];
        }

        // Alternative: run a command on the device to get its IP
        const ipCmd = `ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no -o BatchMode=yes ableton@${hostname} "hostname -I | awk '{print \\$1}'"`;
        const { stdout: ipOutput } = await execAsync(ipCmd, { timeout: 10000 });
        const ip = ipOutput.trim();

        if (ip && /^\d+\.\d+\.\d+\.\d+$/.test(ip)) {
            return ip;
        }

        return null;
    } catch (err) {
        console.log('[DEBUG] SSH IP discovery error:', err.message);
        return null;
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
    try {
        const sshDir = path.join(os.homedir(), '.ssh');
        const keyPath = path.join(sshDir, 'move_key');

        // Ensure .ssh directory exists
        await mkdir(sshDir, { recursive: true });

        console.log('[DEBUG] Checking for ssh-keygen...');

        // Try to use native ssh-keygen (available on Windows 10+, macOS, Linux)
        const { exec } = require('child_process');
        const { promisify } = require('util');
        const execAsync = promisify(exec);

        try {
            // Test if ssh-keygen is available
            await execAsync('ssh-keygen -V', { timeout: 2000 }).catch(() => {
                // -V might not be supported, try version instead
                return execAsync('ssh-keygen', { timeout: 2000 });
            });

            console.log('[DEBUG] Using native ssh-keygen to generate key');

            // Generate Ed25519 key using ssh-keygen
            const keygenCmd = `ssh-keygen -t ed25519 -f "${keyPath}" -N "" -C "move-everything-installer"`;
            await execAsync(keygenCmd);

            console.log('[DEBUG] Key pair generated successfully using ssh-keygen');
            return `${keyPath}.pub`;
        } catch (sshKeygenError) {
            console.log('[DEBUG] ssh-keygen not available, falling back to sshpk library');

            // Use sshpk library to generate OpenSSH-format keys
            // This format works with both ssh2 library and native SSH
            const sshpk = require('sshpk');

            // Generate Ed25519 key using Node.js crypto
            const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');

            // Export as PEM for sshpk to parse
            const privateKeyPem = privateKey.export({
                type: 'pkcs8',
                format: 'pem'
            });

            const publicKeyPem = publicKey.export({
                type: 'spki',
                format: 'pem'
            });

            // Parse with sshpk and convert to OpenSSH format
            const sshpkPrivateKey = sshpk.parsePrivateKey(privateKeyPem, 'pem');
            const sshpkPublicKey = sshpk.parseKey(publicKeyPem, 'pem');

            // Export in OpenSSH format (which ssh2 can read)
            const privateKeyOpenSSH = sshpkPrivateKey.toString('openssh');
            const publicKeySSH = sshpkPublicKey.toString('ssh') + ' move-everything-installer\n';

            await writeFile(keyPath, privateKeyOpenSSH, { mode: 0o600 });
            await writeFile(`${keyPath}.pub`, publicKeySSH, { mode: 0o644 });

            console.log('[DEBUG] Key pair generated successfully using sshpk (OpenSSH format)');
            console.log('[DEBUG] Private key length:', privateKeyOpenSSH.length);
            return `${keyPath}.pub`;
        }
    } catch (err) {
        console.error('[DEBUG] Key generation error:', err);
        throw new Error(`Failed to generate SSH key: ${err.message}`);
    }
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

        // Use cached IP if available, otherwise fall back to baseUrl
        const targetUrl = cachedDeviceIp ? `http://${cachedDeviceIp}` : baseUrl;
        console.log('[DEBUG] Submitting SSH key to:', targetUrl);
        console.log('[DEBUG] Cookie:', savedCookie);

        // Remove comment from SSH key (everything after the last space)
        const keyParts = pubkey.trim().split(' ');
        const keyWithoutComment = keyParts.slice(0, 2).join(' '); // Keep only "ssh-rsa AAAA..."

        console.log('[DEBUG] Pubkey length:', keyWithoutComment.length);
        console.log('[DEBUG] Pubkey content:', keyWithoutComment);

        // Send SSH key as raw POST body (not form field)
        const response = await httpClient.post(`${targetUrl}/api/v1/ssh`, keyWithoutComment, {
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
    try {
        // Use move_key if it exists, otherwise id_rsa
        const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
        const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

        console.log('[DEBUG] testSsh: Looking for key at:', keyPath);
        console.log('[DEBUG] testSsh: Key exists:', fs.existsSync(keyPath));

        if (!fs.existsSync(keyPath)) {
            console.log('[DEBUG] No SSH key found for testing');
            return false;
        }

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

        // Try native SSH first (Windows 10+, macOS, Linux)
        const { exec } = require('child_process');
        const { promisify } = require('util');
        const execAsync = promisify(exec);

        try {
            console.log('[DEBUG] Trying native SSH...');

            // Try ableton@move.local first, then root@move.local
            const users = ['ableton', 'root'];

            for (const username of users) {
                try {
                    console.log(`[DEBUG] Testing SSH as ${username}@${hostIp} using native ssh`);

                    // Test connection with a simple command
                    const sshCmd = `ssh -i "${keyPath}" -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o BatchMode=yes ${username}@${hostIp} "echo test"`;
                    const { stdout } = await execAsync(sshCmd, { timeout: 8000 });

                    if (stdout.trim() === 'test') {
                        console.log(`[DEBUG] Native SSH works as ${username}@${hostIp}`);

                        // If connected as ableton, fix authorized_keys permissions
                        if (username === 'ableton') {
                            console.log('[DEBUG] Fixing authorized_keys permissions via native SSH');
                            const chmodCmd = `ssh -i "${keyPath}" -o StrictHostKeyChecking=no -o BatchMode=yes ${username}@${hostIp} "chmod 600 ~/.ssh/authorized_keys && chmod 700 ~/.ssh"`;
                            await execAsync(chmodCmd, { timeout: 5000 });
                        }

                        return true;
                    }
                } catch (userErr) {
                    console.log(`[DEBUG] Native SSH failed for ${username}:`, userErr.message);
                }
            }

            console.log('[DEBUG] Native SSH failed for all users, trying ssh2 library...');
        } catch (nativeSshErr) {
            console.log('[DEBUG] Native SSH not available, using ssh2 library');
        }

        // Fallback to ssh2 library
        console.log('[DEBUG] testSsh: Reading private key for ssh2...');
        const privateKey = fs.readFileSync(keyPath);
        console.log('[DEBUG] testSsh: Private key length:', privateKey.length);

        const users = ['ableton', 'root'];

        for (const username of users) {
            console.log(`[DEBUG] Testing SSH as ${username}@${hostIp} using ssh2...`);

            const connected = await new Promise((resolve) => {
                const conn = new Client();
                let resolved = false;

                const timeout = setTimeout(() => {
                    console.log(`[DEBUG] Manual timeout fired for ${username}`);
                    if (!resolved) {
                        resolved = true;
                        conn.end();
                        resolve(false);
                    }
                }, 8000);

                conn.on('ready', () => {
                    console.log(`[DEBUG] SSH 'ready' event for ${username}`);
                    clearTimeout(timeout);
                    if (resolved) return;
                    resolved = true;

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
                    console.log(`[DEBUG] SSH 'error' event for ${username}:`, err.message);
                    clearTimeout(timeout);
                    if (resolved) return;
                    resolved = true;
                    resolve(false);
                });

                conn.on('close', (hadError) => {
                    console.log(`[DEBUG] SSH 'close' event for ${username}, hadError:`, hadError);
                    clearTimeout(timeout);
                    if (resolved) return;
                    resolved = true;
                    resolve(false);
                });

                conn.on('timeout', () => {
                    console.log(`[DEBUG] SSH 'timeout' event for ${username}`);
                    clearTimeout(timeout);
                    if (resolved) return;
                    resolved = true;
                    conn.end();
                    resolve(false);
                });

                try {
                    console.log(`[DEBUG] Calling conn.connect() for ${username}`);
                    conn.connect({
                        host: hostIp,
                        port: 22,
                        username: username,
                        privateKey: privateKey,
                        readyTimeout: 8000,
                        family: 4  // Force IPv4
                    });
                    console.log(`[DEBUG] conn.connect() called successfully for ${username}`);
                } catch (err) {
                    console.log(`[DEBUG] Exception in conn.connect() for ${username}:`, err.message);
                    clearTimeout(timeout);
                    if (resolved) return;
                    resolved = true;
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
    } catch (err) {
        console.error('[DEBUG] testSsh error:', err.message);
        console.error('[DEBUG] testSsh stack:', err.stack);
        return false;
    }
}

async function setupSshConfig(hostname = 'move.local') {
    const sshDir = path.join(os.homedir(), '.ssh');
    const configPath = path.join(sshDir, 'config');

    // Strip brackets from IPv6 if present
    const deviceIp = cachedDeviceIp ? cachedDeviceIp.replace(/^\[|\]$/g, '') : null;

    // Escape hostname for use in regex
    const hostnameEscaped = hostname.replace(/\./g, '\\.');

    let configEntry = `
Host ${hostname}
    HostName ${hostname}
    User ableton
    IdentityFile ~/.ssh/move_key
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
`;

    // Add a simple alias "movedevice" that points to the actual IP
    // This avoids IPv6 bracket issues in install.sh
    if (deviceIp) {
        configEntry += `
Host movedevice
    HostName ${deviceIp}
    User ableton
    IdentityFile ~/.ssh/move_key
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
`;
    }

    try {
        let existingConfig = '';
        if (fs.existsSync(configPath)) {
            existingConfig = await readFile(configPath, 'utf-8');
        }

        // Remove old entries to avoid duplicates
        const hostnameRegex = new RegExp(`Host ${hostnameEscaped}\n(?:.*\n)*?(?=Host |$)`, 'm');
        existingConfig = existingConfig.replace(hostnameRegex, '');
        existingConfig = existingConfig.replace(/Host movedevice\n(?:.*\n)*?(?=Host |$)/m, '');

        await writeFile(configPath, existingConfig + configEntry);
        console.log(`[DEBUG] SSH config updated for ${hostname} and movedevice ->`, deviceIp);
    } catch (err) {
        throw new Error(`Failed to setup SSH config: ${err.message}`);
    }
}

async function getModuleCatalog(installedModuleIds = []) {
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

        // For each module, construct direct download URL and fetch version if installed
        const modules = await Promise.all(moduleList.map(async (module) => {
            // Construct direct download URL like install.sh does
            const downloadUrl = `https://github.com/${module.github_repo}/releases/latest/download/${module.asset_name}`;

            let version = null;

            // Only fetch version from GitHub API if module is installed (to avoid rate limits)
            if (installedModuleIds.includes(module.id)) {
                try {
                    console.log(`[DEBUG] Fetching version for installed module: ${module.id}`);
                    const releaseResponse = await httpClient.get(
                        `https://api.github.com/repos/${module.github_repo}/releases/latest`,
                        {
                            headers: { 'User-Agent': 'MoveEverything-Installer' }
                        }
                    );
                    if (releaseResponse.status === 200 && releaseResponse.data && releaseResponse.data.tag_name) {
                        const tagName = releaseResponse.data.tag_name;
                        version = tagName.startsWith('v') ? tagName.substring(1) : tagName;
                        console.log(`[DEBUG] Found version ${version} for ${module.id}`);
                    }
                } catch (err) {
                    console.log(`[DEBUG] Could not fetch version for ${module.id}:`, err.message);
                }
            }

            return {
                ...module,
                version: version,
                download_url: downloadUrl
            };
        }));

        return modules;
    } catch (err) {
        throw new Error(`Failed to get module catalog: ${err.message}`);
    }
}

async function getLatestRelease() {
    try {
        // Fetch latest release info from GitHub API to get actual version
        // GitHub API requires a User-Agent header
        const response = await httpClient.get('https://api.github.com/repos/charlesvestal/move-anything/releases/latest', {
            headers: {
                'User-Agent': 'MoveEverything-Installer'
            }
        });

        console.log('[DEBUG] GitHub API response status:', response.status);

        if (response.status === 200 && response.data && response.data.tag_name) {
            const tagName = response.data.tag_name; // e.g., "v0.3.53"
            const version = tagName.startsWith('v') ? tagName.substring(1) : tagName; // Remove 'v' prefix
            const assetName = 'move-anything.tar.gz';
            const downloadUrl = `https://github.com/charlesvestal/move-anything/releases/latest/download/${assetName}`;

            console.log('[DEBUG] Extracted version from GitHub:', version);

            return {
                version: version,
                asset_name: assetName,
                download_url: downloadUrl
            };
        }

        // Fallback if API fails
        console.log('[DEBUG] GitHub API response missing tag_name, falling back to "latest"');
        const assetName = 'move-anything.tar.gz';
        const downloadUrl = `https://github.com/charlesvestal/move-anything/releases/latest/download/${assetName}`;
        return {
            version: 'latest',
            asset_name: assetName,
            download_url: downloadUrl
        };
    } catch (err) {
        console.error('[DEBUG] Failed to get version from API:', err.message);
        // Fallback to latest URL without version
        const assetName = 'move-anything.tar.gz';
        const downloadUrl = `https://github.com/charlesvestal/move-anything/releases/latest/download/${assetName}`;
        return {
            version: 'latest',
            asset_name: assetName,
            download_url: downloadUrl
        };
    }
}

async function downloadRelease(url, destPath) {
    try {
        // If destPath is just a filename or starts with /tmp/, use system temp dir
        let actualDestPath = destPath;
        if (!path.isAbsolute(destPath) || destPath.startsWith('/tmp/')) {
            const filename = path.basename(destPath);
            actualDestPath = path.join(os.tmpdir(), filename);
            console.log(`[DEBUG] Using temp path: ${actualDestPath}`);
        }

        const response = await httpClient.get(url, {
            responseType: 'stream'
        });

        const writer = fs.createWriteStream(actualDestPath);
        response.data.pipe(writer);

        return new Promise((resolve, reject) => {
            writer.on('finish', () => resolve(actualDestPath));
            writer.on('error', reject);
        });
    } catch (err) {
        throw new Error(`Failed to download release: ${err.message}`);
    }
}

async function sshExec(hostname, command) {
    // Use cached IP from session (already resolved in validateDevice)
    // Prefer cached IP, but allow fallback to hostname for SSH (native ssh can resolve .local)
    const hostIp = cachedDeviceIp || hostname;

    // Use move_key if it exists, otherwise id_rsa
    const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
    const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

    // Try native SSH first
    try {
        const { exec } = require('child_process');
        const { promisify } = require('util');
        const execAsync = promisify(exec);

        const sshCmd = `ssh -i "${keyPath}" -o StrictHostKeyChecking=no -o BatchMode=yes root@${hostIp} "${command.replace(/"/g, '\\"')}"`;
        const { stdout } = await execAsync(sshCmd, { timeout: 30000 });
        return stdout;
    } catch (nativeErr) {
        // Fallback to ssh2 library
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

            conn.connect({
                host: hostIp,
                port: 22,
                username: 'root',
                privateKey: fs.readFileSync(keyPath),
                family: 4  // Force IPv4
            });
        });
    }
}

// Helper to upload file via SFTP
async function sftpUpload(hostname, localPath, remotePath) {
    const hostIp = cachedDeviceIp || hostname;
    const moveKeyPath = path.join(os.homedir(), '.ssh', 'move_key');
    const keyPath = fs.existsSync(moveKeyPath) ? moveKeyPath : path.join(os.homedir(), '.ssh', 'id_rsa');

    return new Promise((resolve, reject) => {
        const conn = new Client();

        conn.on('ready', () => {
            conn.sftp((err, sftp) => {
                if (err) {
                    conn.end();
                    return reject(err);
                }

                sftp.fastPut(localPath, remotePath, (err) => {
                    conn.end();
                    if (err) {
                        reject(err);
                    } else {
                        resolve();
                    }
                });
            });
        });

        conn.on('error', (err) => {
            reject(err);
        });

        conn.connect({
            host: hostIp,
            port: 22,
            username: 'root',
            privateKey: fs.readFileSync(keyPath),
            family: 4
        });
    });
}

async function findGitBash() {
    const bashPaths = [
        'bash',  // If in PATH
        'C:\\Program Files\\Git\\bin\\bash.exe',
        'C:\\Program Files (x86)\\Git\\bin\\bash.exe',
        path.join(process.env.PROGRAMFILES || 'C:\\Program Files', 'Git', 'bin', 'bash.exe'),
        path.join(process.env['PROGRAMFILES(X86)'] || 'C:\\Program Files (x86)', 'Git', 'bin', 'bash.exe')
    ];

    const { exec } = require('child_process');
    const { promisify } = require('util');
    const execAsync = promisify(exec);

    for (const bashPath of bashPaths) {
        try {
            await execAsync(`"${bashPath}" --version`, { timeout: 2000 });
            console.log('[DEBUG] Found Git Bash at:', bashPath);
            return bashPath;
        } catch (err) {
            // Try next path
        }
    }

    return null;
}

async function checkGitBashAvailable() {
    // Only required on Windows
    if (process.platform !== 'win32') {
        return { available: true };
    }

    const bashPath = await findGitBash();
    return {
        available: bashPath !== null,
        path: bashPath
    };
}

async function installMain(tarballPath, hostname, flags = []) {
    try {
        // Verify we have a valid IP address (IPv4 or IPv6)
        if (!cachedDeviceIp) {
            throw new Error(
                'Cannot install: Device IP address not available.\n' +
                'Please enter the device IP address manually.'
            );
        }

        console.log('[DEBUG] Installing using IP:', cachedDeviceIp);

        // On Windows, check for Git Bash
        if (process.platform === 'win32') {
            const bashPath = await findGitBash();
            if (!bashPath) {
                throw new Error(
                    'Git Bash is required for installation on Windows.\n\n' +
                    'Please install Git for Windows from:\n' +
                    'https://git-scm.com/download/win\n\n' +
                    'Then restart the installer.'
                );
            }
        }

        const hostIp = cachedDeviceIp;
        console.log('[DEBUG] Installing to:', hostIp);
        console.log('[DEBUG] Install flags:', flags);

        const { exec } = require('child_process');
        const { promisify } = require('util');
        const execAsync = promisify(exec);

        // Find Git Bash
        const bashPath = await findGitBash();

        // Create temp directory for install script
        const tempDir = path.join(os.tmpdir(), `move-installer-${Date.now()}`);
        await mkdir(tempDir, { recursive: true });
        await mkdir(path.join(tempDir, 'scripts'), { recursive: true });

        try {
            // Download install.sh from GitHub (same source as the tarball)
            console.log('[DEBUG] Downloading install.sh from GitHub...');
            const installScriptUrl = 'https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh';
            const response = await httpClient.get(installScriptUrl);
            let installScriptContent = response.data;

            // Replace move.local with "movedevice" SSH config alias
            // This works for both IPv4 and IPv6 without bracket issues
            installScriptContent = installScriptContent.replace(/move\.local/g, 'movedevice');
            console.log('[DEBUG] Replaced move.local with movedevice (SSH config -> ', hostIp, ')');

            const tempInstallScript = path.join(tempDir, 'scripts', 'install.sh');
            await writeFile(tempInstallScript, installScriptContent, { mode: 0o755 });

            // Copy tarball to temp directory
            const tempTarball = path.join(tempDir, 'move-anything.tar.gz');
            await copyFile(tarballPath, tempTarball);

            // Pre-install cleanup: remove stale temp files and root-owned tarball on device
            console.log('[DEBUG] Cleaning up stale files on device...');
            try {
                const cleanupCmds = [
                    'rm -f ~/move-anything.tar.gz',  // Remove old tarball (may be root-owned)
                    'rm -rf /var/volatile/tmp/move-install-* /var/volatile/tmp/move-uninstall-*'  // Stale temp dirs
                ];
                for (const cleanCmd of cleanupCmds) {
                    // Try as ableton first, then as root for permission issues
                    try {
                        await execAsync(`"${bashPath}" -c "ssh movedevice '${cleanCmd}'"`, { timeout: 10000 });
                    } catch (e) {
                        try {
                            await execAsync(`"${bashPath}" -c "ssh -i ~/.ssh/move_key -o StrictHostKeyChecking=no root@movedevice '${cleanCmd}'"`, { timeout: 10000 });
                        } catch (e2) {
                            console.log('[DEBUG] Cleanup command failed (non-fatal):', cleanCmd);
                        }
                    }
                }
                console.log('[DEBUG] Device cleanup complete');
            } catch (cleanupErr) {
                console.log('[DEBUG] Device cleanup failed (non-fatal):', cleanupErr.message);
            }

            // Build install.sh arguments
            const installArgs = ['local', '--skip-confirmation', '--skip-modules', ...flags];

            // Convert Windows path to Unix path for Git Bash
            const unixTempDir = tempDir.replace(/\\/g, '/').replace(/^([A-Z]):/, (match, drive) => {
                return `/${drive.toLowerCase()}`;
            });

            // Run install.sh via Git Bash
            console.log('[DEBUG] Running install.sh via Git Bash...');
            console.log('[DEBUG] Script:', tempInstallScript);
            console.log('[DEBUG] Args:', installArgs.join(' '));

            const cmd = `"${bashPath}" -c "cd '${unixTempDir}/scripts' && ./install.sh ${installArgs.join(' ')}"`;
            console.log('[DEBUG] Command:', cmd);

            let stdout, stderr;
            try {
                const result = await execAsync(cmd, {
                    timeout: 300000,  // 5 minutes
                    maxBuffer: 10 * 1024 * 1024  // 10MB buffer
                });
                stdout = result.stdout;
                stderr = result.stderr;
            } catch (execError) {
                // Capture output even on failure
                stdout = execError.stdout || '';
                stderr = execError.stderr || '';
                console.log('[DEBUG] Install script failed!');
                console.log('[DEBUG] Exit code:', execError.code);
                console.log('[DEBUG] stdout:', stdout);
                console.log('[DEBUG] stderr:', stderr);
                throw new Error(
                    `install.sh failed with exit code ${execError.code}\n\n` +
                    `Output:\n${stdout}\n\n` +
                    `Errors:\n${stderr}`
                );
            }

            console.log('[DEBUG] Install script output:', stdout);
            if (stderr) {
                console.log('[DEBUG] Install script stderr:', stderr);
            }

            console.log('[DEBUG] Installation complete!');
            return true;
        } finally {
            // Clean up temp directory
            try {
                await rm(tempDir, { recursive: true, force: true });
            } catch (err) {
                console.log('[DEBUG] Failed to clean up temp dir:', err.message);
            }
        }
    } catch (err) {
        console.error('[DEBUG] Installation error:', err.message);
        throw new Error(`Installation failed: ${err.message}`);
    }
}

async function installModulePackage(moduleId, tarballPath, componentType, hostname) {
    try {
        console.log(`[DEBUG] Installing module ${moduleId} (${componentType})`);
        const filename = path.basename(tarballPath);

        // Use cached IP instead of hostname for faster connection
        const hostIp = cachedDeviceIp || hostname;
        console.log(`[DEBUG] Using host: ${hostIp} (cached: ${!!cachedDeviceIp})`);

        // Upload to Move Everything directory using SFTP
        const remotePath = `/data/UserData/move-anything/${filename}`;
        console.log(`[DEBUG] Uploading ${filename} to device via SFTP...`);
        await sftpUpload(hostIp, tarballPath, remotePath);
        console.log(`[DEBUG] Upload complete for ${moduleId}`);

        // Extract and install module (similar to install.sh module installation)
        const categoryPath = componentType ? `${componentType}s` : 'utility';
        console.log(`[DEBUG] Extracting ${moduleId} to modules/${categoryPath}/`);
        await sshExec(hostIp, `cd /data/UserData/move-anything && mkdir -p modules/${categoryPath} && tar -xzf ${filename} -C modules/${categoryPath}/ && rm ${filename}`);
        console.log(`[DEBUG] Module ${moduleId} installed successfully`);

        return true;
    } catch (err) {
        console.error(`[DEBUG] Module installation error for ${moduleId}:`, err.message);
        throw new Error(`Module installation failed: ${err.message}`);
    }
}

async function checkCoreInstallation(hostname) {
    try {
        const hostIp = cachedDeviceIp || hostname;
        console.log('[DEBUG] Checking if Move Everything is installed...');

        // Quick check: is Move Everything installed?
        const installCheck = await sshExec(hostIp, 'test -d /data/UserData/move-anything && echo "installed" || echo "not_installed"');
        if (installCheck.trim() === 'not_installed') {
            console.log('[DEBUG] Move Everything not installed');
            return { installed: false, core: null };
        }

        // Get core version only
        let coreVersion = null;
        try {
            const versionOutput = await sshExec(hostIp, 'cat /data/UserData/move-anything/version.txt 2>/dev/null || echo ""');
            coreVersion = versionOutput.trim() || null;
            console.log('[DEBUG] Core version:', coreVersion);
        } catch (err) {
            console.log('[DEBUG] Could not read core version:', err.message);
        }

        return { installed: true, core: coreVersion };
    } catch (err) {
        console.error('[DEBUG] Error checking core installation:', err.message);
        throw new Error(`Failed to check installation: ${err.message}`);
    }
}

async function checkInstalledVersions(hostname, progressCallback = null) {
    try {
        const hostIp = cachedDeviceIp || hostname;
        console.log('[DEBUG] Checking installed versions on device...');

        // Check if Move Everything is installed
        const installCheck = await sshExec(hostIp, 'test -d /data/UserData/move-anything && echo "installed" || echo "not_installed"');
        if (installCheck.trim() === 'not_installed') {
            console.log('[DEBUG] Move Everything not installed on device');
            return {
                installed: false,
                core: null,
                modules: []
            };
        }

        // Get core version
        let coreVersion = null;
        try {
            if (progressCallback) progressCallback('Checking core version...');
            const versionOutput = await sshExec(hostIp, 'cat /data/UserData/move-anything/version.txt 2>/dev/null || echo ""');
            coreVersion = versionOutput.trim() || null;
            console.log('[DEBUG] Core version:', coreVersion);
        } catch (err) {
            console.log('[DEBUG] Could not read core version:', err.message);
        }

        // Find all installed modules
        const modules = [];
        try {
            if (progressCallback) progressCallback('Finding installed modules...');

            // Find all module.json files in modules subdirectories
            const findOutput = await sshExec(hostIp,
                'find /data/UserData/move-anything/modules -name module.json -type f 2>/dev/null || echo ""'
            );

            const moduleFiles = findOutput.trim().split('\n').filter(line => line);
            console.log(`[DEBUG] Found ${moduleFiles.length} module.json files`);

            // Read each module.json
            for (let i = 0; i < moduleFiles.length; i++) {
                const moduleFile = moduleFiles[i];
                try {
                    if (progressCallback) {
                        progressCallback(`Checking module ${i + 1} of ${moduleFiles.length}...`);
                    }

                    const jsonContent = await sshExec(hostIp, `cat "${moduleFile}"`);
                    const moduleInfo = JSON.parse(jsonContent);

                    if (moduleInfo.id && moduleInfo.version) {
                        modules.push({
                            id: moduleInfo.id,
                            name: moduleInfo.name || moduleInfo.id,
                            version: moduleInfo.version,
                            component_type: moduleInfo.component_type || 'utility'
                        });
                        console.log(`[DEBUG] Found module: ${moduleInfo.id} v${moduleInfo.version}`);
                    }
                } catch (err) {
                    console.log(`[DEBUG] Error reading ${moduleFile}:`, err.message);
                }
            }
        } catch (err) {
            console.log('[DEBUG] Error finding modules:', err.message);
        }

        return {
            installed: true,
            core: coreVersion,
            modules
        };
    } catch (err) {
        console.error('[DEBUG] Error checking installed versions:', err.message);
        throw new Error(`Failed to check installed versions: ${err.message}`);
    }
}

function compareVersions(installed, latestRelease, moduleCatalog) {
    const result = {
        coreUpgrade: null,
        upgradableModules: [],
        upToDateModules: [],
        newModules: []
    };

    // Compare core version
    if (installed.core && latestRelease.version && installed.core !== latestRelease.version) {
        result.coreUpgrade = {
            current: installed.core,
            available: latestRelease.version
        };
    }

    // Create map of installed modules by id
    const installedMap = new Map(installed.modules.map(m => [m.id, m]));

    // Check each module in catalog
    for (const catalogModule of moduleCatalog) {
        const installedModule = installedMap.get(catalogModule.id);

        if (installedModule) {
            // Module is installed - check if upgrade available
            if (catalogModule.version && catalogModule.version !== installedModule.version) {
                result.upgradableModules.push({
                    ...catalogModule,
                    currentVersion: installedModule.version
                });
            } else if (catalogModule.version) {
                // Version matches - up to date
                result.upToDateModules.push({
                    ...catalogModule,
                    currentVersion: installedModule.version
                });
            } else {
                // Could not fetch version - show as up to date (no upgrade info available)
                result.upToDateModules.push({
                    ...catalogModule,
                    currentVersion: installedModule.version
                });
            }
        } else {
            // Module not installed - it's new
            result.newModules.push(catalogModule);
        }
    }

    return result;
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

async function getScreenReaderStatus(hostname) {
    try {
        const hostIp = cachedDeviceIp || hostname;

        // Check for screen reader state file (used by tts_engine_flite.c)
        const checkCmd = 'cat /data/UserData/move-anything/config/screen_reader_state.txt 2>/dev/null || echo "0"';
        const status = (await sshExec(hostIp, checkCmd)).trim();

        return status === '1';
    } catch (err) {
        console.log('[DEBUG] Could not read screen reader status:', err.message);
        return false;
    }
}

async function setScreenReaderState(hostname, enabled) {
    try {
        const hostIp = cachedDeviceIp || hostname;
        console.log('[DEBUG] Setting screen reader to:', enabled);

        // Ensure config directory exists
        await sshExec(hostIp, 'mkdir -p /data/UserData/move-anything/config');

        // Write state file (1 = enabled, 0 = disabled)
        const value = enabled ? '1' : '0';
        await sshExec(hostIp, `echo "${value}" > /data/UserData/move-anything/config/screen_reader_state.txt`);

        // Restart move-anything process so it picks up the new state
        console.log('[DEBUG] Restarting move-anything...');
        await sshExec(hostIp, 'killall move-anything 2>/dev/null || true');

        return {
            enabled: enabled,
            message: `Screen reader ${enabled ? 'enabled' : 'disabled'}. Move Everything is restarting.`
        };
    } catch (err) {
        console.error('[DEBUG] Screen reader toggle error:', err.message);
        throw new Error(`Failed to set screen reader state: ${err.message}`);
    }
}

async function uninstallMoveEverything(hostname) {
    try {
        const hostIp = cachedDeviceIp || hostname;
        console.log('[DEBUG] Uninstalling Move Everything from:', hostIp);

        // Stop move-anything service
        console.log('[DEBUG] Stopping move-anything service...');
        await sshExec(hostIp, 'systemctl stop move-anything 2>/dev/null || killall move-anything 2>/dev/null || true');

        // Remove shim from /usr/lib if it exists
        console.log('[DEBUG] Removing shim library...');
        await sshExec(hostIp, 'rm -f /usr/lib/move-anything-shim.so');

        // Remove Move Everything directory
        console.log('[DEBUG] Removing Move Everything files...');
        await sshExec(hostIp, 'rm -rf /data/UserData/move-anything');

        // Restore original Move binary if backup exists
        console.log('[DEBUG] Restoring original Move binary...');
        const restoreCmd = `
            if [ -f /opt/move/MoveOriginal ]; then
                mv /opt/move/MoveOriginal /opt/move/Move
                echo "restored"
            else
                echo "no_backup"
            fi
        `;
        const restoreResult = (await sshExec(hostIp, restoreCmd)).trim();

        if (restoreResult === 'no_backup') {
            console.log('[DEBUG] No backup found, original Move binary may already be in place');
        }

        // Restart the device
        console.log('[DEBUG] Restarting device...');
        await sshExec(hostIp, 'reboot');

        console.log('[DEBUG] Uninstall complete');
        return {
            success: true,
            message: 'Move Everything has been uninstalled. Your Move is restarting and will boot to stock firmware.'
        };
    } catch (err) {
        console.error('[DEBUG] Uninstall error:', err.message);
        throw new Error(`Failed to uninstall: ${err.message}`);
    }
}

async function testSshFormats(cookie) {
    const results = [];
    const testDir = path.join(os.tmpdir(), 'ssh-test');
    const sshpk = require('sshpk');

    try {
        await mkdir(testDir, { recursive: true });
    } catch (err) {
        // Ignore if exists
    }

    console.log('[TEST] Starting SSH format tests...');
    console.log('[TEST] Device IP:', cachedDeviceIp);

    if (!cachedDeviceIp) {
        return [{ error: 'Must connect to device first (cachedDeviceIp not set)' }];
    }

    const deviceUrl = `http://${cachedDeviceIp}`;

    // Test 1: Ed25519 with sshpk (OpenSSH format)
    try {
        console.log('[TEST] Testing Ed25519 with sshpk (OpenSSH format)...');
        const keyPath = path.join(testDir, 'test_ed25519_sshpk');

        const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');
        const privateKeyPem = privateKey.export({ type: 'pkcs8', format: 'pem' });
        const publicKeyPem = publicKey.export({ type: 'spki', format: 'pem' });

        const sshpkPrivateKey = sshpk.parsePrivateKey(privateKeyPem, 'pem');
        const sshpkPublicKey = sshpk.parseKey(publicKeyPem, 'pem');

        const privateKeyOpenSSH = sshpkPrivateKey.toString('openssh');
        const pubkey = sshpkPublicKey.toString('ssh') + ' test';

        fs.writeFileSync(keyPath, privateKeyOpenSSH);

        const result = {
            name: 'Ed25519 OpenSSH (via sshpk)',
            privateKeyLength: privateKeyOpenSSH.length,
            publicKey: pubkey.substring(0, 80) + '...',
            apiAccepted: false,
            nativeSshWorks: false,
            ssh2Works: false,
            errors: []
        };

        // Test API submission
        try {
            const pubkeyClean = pubkey.trim().split(' ').slice(0, 2).join(' ');
            const response = await httpClient.post(`${deviceUrl}/api/v1/ssh`, pubkeyClean, {
                headers: { 'Cookie': cookie, 'Content-Type': 'application/x-www-form-urlencoded' },
                timeout: 5000
            });
            result.apiAccepted = response.status === 200;
        } catch (err) {
            result.errors.push(`API: ${err.response?.status} - ${JSON.stringify(err.response?.data)}`);
        }

        // Test native SSH
        try {
            const { exec } = require('child_process');
            const { promisify } = require('util');
            const execAsync = promisify(exec);
            const sshCmd = `ssh -i "${keyPath}" -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=yes ableton@${cachedDeviceIp} "echo test"`;
            const { stdout } = await execAsync(sshCmd, { timeout: 5000 });
            result.nativeSshWorks = stdout.trim() === 'test';
        } catch (err) {
            result.errors.push(`Native SSH: ${(err.stderr || err.message).substring(0, 100)}`);
        }

        // Test ssh2
        try {
            const Client = require('ssh2').Client;
            const conn = new Client();
            const connected = await new Promise((resolve) => {
                const timeout = setTimeout(() => { conn.end(); resolve(false); }, 3000);
                conn.on('ready', () => { clearTimeout(timeout); conn.end(); resolve(true); });
                conn.on('error', () => { clearTimeout(timeout); resolve(false); });
                conn.connect({
                    host: cachedDeviceIp,
                    port: 22,
                    username: 'ableton',
                    privateKey: Buffer.from(privateKeyOpenSSH),
                    readyTimeout: 3000
                });
            });
            result.ssh2Works = connected;
        } catch (err) {
            result.errors.push(`ssh2: ${err.message}`);
        }

        results.push(result);
    } catch (err) {
        results.push({ name: 'Ed25519 OpenSSH (via sshpk)', error: err.message });
    }

    // Test 2: RSA with sshpk
    try {
        console.log('[TEST] Testing RSA with sshpk...');
        const keyPath = path.join(testDir, 'test_rsa_sshpk');

        const { publicKey, privateKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });
        const privateKeyPem = privateKey.export({ type: 'pkcs1', format: 'pem' });
        const publicKeyPem = publicKey.export({ type: 'spki', format: 'pem' });

        const sshpkPrivateKey = sshpk.parsePrivateKey(privateKeyPem, 'pem');
        const sshpkPublicKey = sshpk.parseKey(publicKeyPem, 'pem');

        const privateKeyOpenSSH = sshpkPrivateKey.toString('openssh');
        const pubkey = sshpkPublicKey.toString('ssh') + ' test';

        fs.writeFileSync(keyPath, privateKeyOpenSSH);

        const result = {
            name: 'RSA OpenSSH (via sshpk)',
            privateKeyLength: privateKeyOpenSSH.length,
            publicKey: pubkey.substring(0, 80) + '...',
            apiAccepted: false,
            nativeSshWorks: false,
            ssh2Works: false,
            errors: []
        };

        // Test API submission
        try {
            const pubkeyClean = pubkey.trim().split(' ').slice(0, 2).join(' ');
            const response = await httpClient.post(`${deviceUrl}/api/v1/ssh`, pubkeyClean, {
                headers: { 'Cookie': cookie, 'Content-Type': 'application/x-www-form-urlencoded' },
                timeout: 5000
            });
            result.apiAccepted = response.status === 200;
        } catch (err) {
            result.errors.push(`API: ${err.response?.status} - ${JSON.stringify(err.response?.data)}`);
        }

        // Test native SSH (skip - not submitted to device)
        result.nativeSshWorks = null;
        result.ssh2Works = null;

        results.push(result);
    } catch (err) {
        results.push({ name: 'RSA OpenSSH (via sshpk)', error: err.message });
    }

    // Test 3: Ed25519 PKCS8 PEM (old approach)
    try {
        console.log('[TEST] Testing Ed25519 PKCS8 PEM...');
        const keyPath = path.join(testDir, 'test_ed25519_pkcs8');

        const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519', {
            publicKeyEncoding: { type: 'spki', format: 'pem' },
            privateKeyEncoding: { type: 'pkcs8', format: 'pem' }
        });

        const publicKeyObj = crypto.createPublicKey(publicKey);
        const publicKeyDer = publicKeyObj.export({ type: 'spki', format: 'der' });
        const publicKeyRaw = publicKeyDer.slice(-32);

        const typeBytes = Buffer.from('ssh-ed25519');
        const typeLength = Buffer.alloc(4);
        typeLength.writeUInt32BE(typeBytes.length, 0);
        const keyLength = Buffer.alloc(4);
        keyLength.writeUInt32BE(publicKeyRaw.length, 0);
        const sshPublicKey = Buffer.concat([typeLength, typeBytes, keyLength, publicKeyRaw]);
        const pubkey = `ssh-ed25519 ${sshPublicKey.toString('base64')} test`;

        fs.writeFileSync(keyPath, privateKey);

        const result = {
            name: 'Ed25519 PKCS8 PEM',
            privateKeyLength: privateKey.length,
            publicKey: pubkey.substring(0, 80) + '...',
            apiAccepted: false,
            nativeSshWorks: false,
            ssh2Works: false,
            errors: []
        };

        // Test API submission
        try {
            const pubkeyClean = pubkey.trim().split(' ').slice(0, 2).join(' ');
            const response = await httpClient.post(`${deviceUrl}/api/v1/ssh`, pubkeyClean, {
                headers: { 'Cookie': cookie, 'Content-Type': 'application/x-www-form-urlencoded' },
                timeout: 5000
            });
            result.apiAccepted = response.status === 200;
        } catch (err) {
            result.errors.push(`API: ${err.response?.status} - ${JSON.stringify(err.response?.data)}`);
        }

        // Test native SSH
        try {
            const { exec } = require('child_process');
            const { promisify } = require('util');
            const execAsync = promisify(exec);
            const sshCmd = `ssh -i "${keyPath}" -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=yes ableton@${cachedDeviceIp} "echo test"`;
            const { stdout } = await execAsync(sshCmd, { timeout: 5000 });
            result.nativeSshWorks = stdout.trim() === 'test';
        } catch (err) {
            const errorMsg = err.stderr || err.message;
            if (errorMsg.includes('invalid format')) {
                result.errors.push('Native SSH: invalid key format');
            } else {
                result.errors.push(`Native SSH: ${errorMsg.substring(0, 100)}`);
            }
        }

        // Test ssh2
        try {
            const Client = require('ssh2').Client;
            const conn = new Client();
            const connected = await new Promise((resolve) => {
                const timeout = setTimeout(() => { conn.end(); resolve(false); }, 3000);
                conn.on('ready', () => { clearTimeout(timeout); conn.end(); resolve(true); });
                conn.on('error', () => { clearTimeout(timeout); resolve(false); });
                conn.connect({
                    host: cachedDeviceIp,
                    port: 22,
                    username: 'ableton',
                    privateKey: Buffer.from(privateKey),
                    readyTimeout: 3000
                });
            });
            result.ssh2Works = connected;
        } catch (err) {
            result.errors.push(`ssh2: ${err.message}`);
        }

        results.push(result);
    } catch (err) {
        results.push({ name: 'Ed25519 PKCS8 PEM', error: err.message });
    }

    console.log('[TEST] Test results:', JSON.stringify(results, null, 2));
    return results;
}

module.exports = {
    setMainWindow,
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
    checkGitBashAvailable,
    getModuleCatalog,
    getLatestRelease,
    downloadRelease,
    installMain,
    installModulePackage,
    checkCoreInstallation,
    checkInstalledVersions,
    compareVersions,
    getDiagnostics,
    getScreenReaderStatus,
    setScreenReaderState,
    uninstallMoveEverything,
    testSshFormats
};
