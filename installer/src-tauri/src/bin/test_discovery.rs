use std::net::ToSocketAddrs;

#[tokio::main]
async fn main() {
    println!("Testing device discovery...");

    // Test 1: DNS resolution
    let hostname = "move.local";
    println!("\n[TEST 1] DNS resolution for {}", hostname);
    match format!("{}:80", hostname).to_socket_addrs() {
        Ok(mut addrs) => {
            if let Some(addr) = addrs.next() {
                println!("✓ Resolved to: {}", addr.ip());
            } else {
                println!("✗ No addresses returned");
            }
        }
        Err(e) => {
            println!("✗ DNS resolution failed: {}", e);
        }
    }

    // Test 2: HTTP check
    println!("\n[TEST 2] HTTP connectivity");
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(5))
        .build()
        .unwrap();

    match client.get("http://move.local/").send().await {
        Ok(response) => {
            println!("✓ HTTP GET succeeded: {}", response.status());
        }
        Err(e) => {
            println!("✗ HTTP GET failed: {}", e);
        }
    }

    println!("\nDone.");
}
