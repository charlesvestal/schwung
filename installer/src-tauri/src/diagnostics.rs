use serde::Serialize;
use std::time::SystemTime;

#[derive(Serialize)]
pub struct DiagnosticReport {
    pub timestamp: String,
    pub app_version: String,
    pub device_ip: Option<String>,
    pub errors: Vec<String>,
}

pub fn generate_report(device_ip: Option<String>, errors: Vec<String>) -> String {
    let report = DiagnosticReport {
        timestamp: format!("{:?}", SystemTime::now()),
        app_version: env!("CARGO_PKG_VERSION").to_string(),
        device_ip,
        errors,
    };

    serde_json::to_string_pretty(&report).unwrap_or_default()
}
