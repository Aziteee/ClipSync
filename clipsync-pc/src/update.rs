use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct UpdateInfo {
    version: String,
}

#[derive(Debug, Clone)]
pub enum CheckResult {
    UpdateAvailable,
    UpToDate,
    Error,
}

const UPDATE_URL: &str = "https://raw.githubusercontent.com/Aziteee/ClipSync/master/update.json";
const RELEASE_URL: &str = "https://github.com/Aziteee/ClipSync/releases";

pub fn parse_version(s: &str) -> Option<(u32, u32, u32)> {
    let s = s.strip_prefix('v').unwrap_or(s);
    let mut parts = s.splitn(3, '.');
    let major = parts.next()?.parse().ok()?;
    let minor = parts.next()?.parse().ok()?;
    let patch = parts.next()?.parse().ok()?;
    Some((major, minor, patch))
}

pub fn is_newer(local: &str, remote: &str) -> bool {
    let local = match parse_version(local) {
        Some(v) => v,
        None => return false,
    };
    let remote = match parse_version(remote) {
        Some(v) => v,
        None => return false,
    };
    remote > local
}

pub async fn check_for_updates() -> CheckResult {
    let current = env!("CARGO_PKG_VERSION");

    let resp = match reqwest::get(UPDATE_URL).await {
        Ok(r) => r,
        Err(_) => return CheckResult::Error,
    };

    if let Err(_) = resp.error_for_status_ref() {
        return CheckResult::Error;
    }

    let info: UpdateInfo = match resp.json().await {
        Ok(info) => info,
        Err(_) => return CheckResult::Error,
    };

    let remote = &info.version;

    if is_newer(current, remote) {
        CheckResult::UpdateAvailable
    } else {
        CheckResult::UpToDate
    }
}

pub fn open_releases_page() {
    let _ = webbrowser::open(RELEASE_URL);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_version_without_v_prefix() {
        assert_eq!(parse_version("1.2.3"), Some((1, 2, 3)));
    }

    #[test]
    fn parse_version_with_v_prefix() {
        assert_eq!(parse_version("v1.2.3"), Some((1, 2, 3)));
    }

    #[test]
    fn parse_version_invalid_returns_none() {
        assert_eq!(parse_version("abc"), None);
        assert_eq!(parse_version("1.2"), None);
        assert_eq!(parse_version(""), None);
    }

    #[test]
    fn is_newer_returns_true_when_remote_is_newer() {
        assert!(is_newer("1.2.0", "1.3.0"));
        assert!(is_newer("1.2.0", "2.0.0"));
        assert!(is_newer("1.2.0", "1.2.1"));
    }

    #[test]
    fn is_newer_returns_false_when_same_or_older() {
        assert!(!is_newer("1.2.0", "1.2.0"));
        assert!(!is_newer("1.3.0", "1.2.0"));
        assert!(!is_newer("2.0.0", "1.9.9"));
    }

    #[test]
    fn is_newer_handles_v_prefix_on_remote() {
        assert!(is_newer("1.2.0", "v1.3.0"));
    }
}
