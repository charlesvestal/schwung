use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct MoveDevice {
    pub hostname: String,
    pub ip: String,
}

/// Discover Move device on local network
pub async fn discover_move() -> Result<MoveDevice, String> {
    // First, try direct hostname resolution for move.local
    let hostname = "move.local";

    eprintln!("[DEBUG] Starting device discovery for {}", hostname);

    // Use DNS lookup to resolve hostname to IP
    use std::net::ToSocketAddrs;
    match format!("{}:80", hostname).to_socket_addrs() {
        Ok(mut addrs) => {
            if let Some(addr) = addrs.next() {
                let ip = addr.ip().to_string();
                eprintln!("[DEBUG] Resolved {} to {}", hostname, ip);

                // Validate it's actually a Move device by checking HTTP
                let base_url = format!("http://{}", ip);
                eprintln!("[DEBUG] Validating device at {}", base_url);

                match validate_device(&base_url).await {
                    Ok(_) => {
                        eprintln!("[DEBUG] Device validated successfully");
                        return Ok(MoveDevice {
                            hostname: hostname.to_string(),
                            ip,
                        });
                    }
                    Err(e) => {
                        eprintln!("[DEBUG] Device validation failed: {}", e);
                    }
                }
            } else {
                eprintln!("[DEBUG] No addresses returned for {}", hostname);
            }
        }
        Err(e) => {
            eprintln!("[DEBUG] Hostname resolution failed: {}", e);
            eprintln!("[DEBUG] Trying mDNS service discovery...");

            // Hostname resolution failed, try mDNS service discovery as fallback
            if let Ok(device) = discover_via_mdns().await {
                eprintln!("[DEBUG] Found device via mDNS: {:?}", device);
                return Ok(device);
            }
        }
    }

    eprintln!("[DEBUG] All discovery methods failed");
    Err("Move device not found. Please ensure Move is on the same WiFi network.".to_string())
}

/// Fallback: Discover via mDNS service browsing
async fn discover_via_mdns() -> Result<MoveDevice, String> {
    let mdns = ServiceDaemon::new().map_err(|e| format!("Failed to start mDNS: {}", e))?;

    let receiver = mdns
        .browse("_http._tcp.local.")
        .map_err(|e| format!("Failed to browse: {}", e))?;

    let timeout = Duration::from_secs(5);
    let start = std::time::Instant::now();

    while start.elapsed() < timeout {
        if let Ok(event) = receiver.recv_timeout(Duration::from_millis(100)) {
            match event {
                ServiceEvent::ServiceResolved(info) => {
                    if info.get_hostname().contains("move") {
                        let ip = info.get_addresses().iter().next()
                            .ok_or("No IP address found")?
                            .to_string();
                        return Ok(MoveDevice {
                            hostname: "move.local".to_string(),
                            ip,
                        });
                    }
                }
                _ => {}
            }
        }
    }

    Err("Move device not found via mDNS".to_string())
}

/// Validate device by checking HTTP endpoint
pub async fn validate_device(base_url: &str) -> Result<bool, String> {
    let client = reqwest::Client::new();
    match client.get(base_url).send().await {
        Ok(_) => Ok(true),
        Err(e) => Err(format!("Cannot reach device: {}", e)),
    }
}
