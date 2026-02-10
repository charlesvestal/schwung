use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::time::Duration;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct MoveDevice {
    pub hostname: String,
    pub ip: String,
}

/// Discover Move device on local network via mDNS
pub async fn discover_move() -> Result<MoveDevice, String> {
    let mdns = ServiceDaemon::new().map_err(|e| format!("Failed to start mDNS: {}", e))?;

    // Try to resolve move.local
    let receiver = mdns
        .browse("_http._tcp.local.")
        .map_err(|e| format!("Failed to browse: {}", e))?;

    // Wait up to 5 seconds for discovery
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

    Err("Move device not found on network".to_string())
}

/// Validate device by checking HTTP endpoint
pub async fn validate_device(base_url: &str) -> Result<bool, String> {
    let client = reqwest::Client::new();
    match client.get(base_url).send().await {
        Ok(_) => Ok(true),
        Err(e) => Err(format!("Cannot reach device: {}", e)),
    }
}
