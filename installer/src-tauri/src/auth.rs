use reqwest::{Client, cookie::Jar};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

const COOKIE_NAME: &str = "Ableton-Challenge-Response-Token";

#[derive(Debug, Serialize, Deserialize)]
pub struct AuthCookie {
    pub value: String,
    pub expires: String,
}

pub struct AuthClient {
    client: Client,
    base_url: String,
}

impl AuthClient {
    pub fn new(base_url: String) -> Self {
        let jar = Arc::new(Jar::default());
        let client = Client::builder()
            .cookie_provider(jar)
            .build()
            .unwrap();

        Self { client, base_url }
    }

    /// Submit 6-digit code and obtain auth cookie
    pub async fn submit_code(&self, code: &str) -> Result<AuthCookie, String> {
        let endpoint = format!("{}/api/v1/challenge-response", self.base_url);

        let response = self.client
            .post(&endpoint)
            .json(&serde_json::json!({"secret": code}))
            .send()
            .await
            .map_err(|e| format!("Failed to submit code: {}", e))?;

        if !response.status().is_success() {
            return Err(format!("Invalid code: HTTP {}", response.status()));
        }

        // Extract cookie from response
        let cookies = response.cookies();
        for cookie in cookies {
            if cookie.name() == COOKIE_NAME {
                return Ok(AuthCookie {
                    value: cookie.value().to_string(),
                    expires: cookie.expires()
                        .map(|e| format!("{:?}", e))
                        .unwrap_or_default(),
                });
            }
        }

        Err("No auth cookie in response".to_string())
    }

    /// Submit SSH public key with authentication
    pub async fn submit_ssh_key(&self, pubkey: &str) -> Result<(), String> {
        let endpoint = format!("{}/api/v1/ssh", self.base_url);

        let response = self.client
            .post(&endpoint)
            .json(&serde_json::json!({"publicKey": pubkey}))
            .send()
            .await
            .map_err(|e| format!("Failed to submit key: {}", e))?;

        match response.status().as_u16() {
            200 | 204 => Ok(()),
            401 | 403 => Err("Unauthorized - cookie expired or invalid".to_string()),
            400 => Err("Invalid key format".to_string()),
            code => Err(format!("Unexpected response: HTTP {}", code)),
        }
    }
}
