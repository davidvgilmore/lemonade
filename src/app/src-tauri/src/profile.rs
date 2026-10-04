use std::ffi::OsString;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::OnceLock;

static PROFILE: OnceLock<IsolatedProfile> = OnceLock::new();

#[derive(Debug, Clone)]
pub(crate) struct IsolatedProfile {
    pub directory: PathBuf,
    pub server_url: String,
}

pub(crate) fn current() -> Option<&'static IsolatedProfile> {
    PROFILE.get()
}

pub(crate) fn parse(
    args: impl IntoIterator<Item = OsString>,
) -> Result<Option<IsolatedProfile>, String> {
    let args: Vec<_> = args.into_iter().collect();
    if !args.iter().any(|a| {
        a.to_string_lossy().starts_with("--isolated-profile")
            || a.to_string_lossy().starts_with("--server-url")
    }) {
        return Ok(None);
    }
    let mut directory = None;
    let mut server_url = None;
    let mut args = args.into_iter();
    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or("Isolated mode requires --isolated-profile PATH --server-url URL")?;
        if flag == "--isolated-profile" && directory.is_none() {
            directory = Some(PathBuf::from(value));
        } else if flag == "--server-url" && server_url.is_none() {
            server_url = Some(
                value
                    .into_string()
                    .map_err(|_| "Server URL must be UTF-8")?,
            );
        } else {
            return Err("Unknown or repeated argument in isolated mode".into());
        }
    }
    let directory = directory.ok_or("--isolated-profile is required with --server-url")?;
    if !directory.is_absolute() {
        return Err("Isolated profile must be an absolute path".into());
    }
    let server_url = server_url.ok_or("--server-url is required with --isolated-profile")?;
    let url =
        url::Url::parse(&server_url).map_err(|_| "Server URL must be an explicit HTTP(S) URL")?;
    if !matches!(url.scheme(), "http" | "https")
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err("Server URL must be HTTP(S), without credentials, query or fragment".into());
    }
    Ok(Some(IsolatedProfile {
        directory,
        server_url: url.as_str().trim_end_matches('/').into(),
    }))
}

pub(crate) struct ProfileGuard {
    lock: PathBuf,
}

impl Drop for ProfileGuard {
    fn drop(&mut self) {
        if let Err(error) = fs::remove_file(&self.lock) {
            log::warn!("Could not remove isolated profile lock: {error}");
        }
    }
}

fn resolve_directory(path: &std::path::Path) -> Result<PathBuf, String> {
    if !path.is_absolute()
        || path
            .components()
            .any(|part| part == std::path::Component::ParentDir)
    {
        return Err("Profile directory must be absolute and cannot contain ..".into());
    }
    let mut ancestor = path;
    let mut suffix = Vec::new();
    while !ancestor.exists() {
        suffix.push(
            ancestor
                .file_name()
                .ok_or("Cannot resolve profile directory")?,
        );
        ancestor = ancestor.parent().ok_or("Cannot resolve profile parent")?;
    }
    let mut resolved = ancestor
        .canonicalize()
        .map_err(|e| format!("Cannot resolve profile directory: {e}"))?;
    for part in suffix.into_iter().rev() {
        resolved.push(part);
    }
    Ok(resolved)
}

impl IsolatedProfile {
    fn acquire(mut self, forbidden: &[PathBuf]) -> Result<(Self, ProfileGuard), String> {
        self.directory = resolve_directory(&self.directory)?;
        for path in forbidden {
            if self.directory == resolve_directory(path)? {
                return Err(
                    "Isolated profile cannot be the normal or legacy settings directory".into(),
                );
            }
        }
        fs::create_dir_all(&self.directory)
            .map_err(|e| format!("Cannot create isolated profile: {e}"))?;
        let lock = self.directory.join("desktop.lock");
        let mut file = OpenOptions::new().write(true).create_new(true).open(&lock)
            .map_err(|e| format!("Cannot own isolated profile ({e}); inspect desktop.lock and its process before manually removing a stale lock"))?;
        let guard = ProfileGuard { lock };
        writeln!(file, "{}", std::process::id())
            .map_err(|e| format!("Cannot write isolated profile lock: {e}"))?;
        Ok((self, guard))
    }
}

fn require_profile(
    profile: Option<IsolatedProfile>,
    required: bool,
) -> Result<Option<IsolatedProfile>, String> {
    if required && profile.is_none() {
        return Err("This build requires --isolated-profile PATH and --server-url URL; normal-profile startup is disabled".into());
    }
    Ok(profile)
}

pub(crate) fn initialize() -> Result<Option<ProfileGuard>, String> {
    let Some(profile) = require_profile(
        parse(std::env::args_os().skip(1))?,
        cfg!(feature = "isolated-profile-required"),
    )?
    else {
        return Ok(None);
    };
    let mut forbidden = Vec::new();
    if let Some(path) = dirs::config_dir() {
        forbidden.push(path.join("lemonade"));
    }
    if let Some(path) = dirs::home_dir() {
        forbidden.push(path.join(".cache/lemonade"));
    }
    let (profile, guard) = profile.acquire(&forbidden)?;
    PROFILE
        .set(profile)
        .map_err(|_| "Desktop profile already initialized")?;
    Ok(Some(guard))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn arguments(args: &[&str]) -> Result<Option<IsolatedProfile>, String> {
        parse(args.iter().map(OsString::from))
    }
    #[test]
    fn required_profile_refuses_normal_startup_before_any_profile_io() {
        assert!(require_profile(None, true).is_err());
        assert!(require_profile(None, false).unwrap().is_none());
        let profile = IsolatedProfile {
            directory: PathBuf::from("/unused"),
            server_url: "http://localhost".into(),
        };
        assert!(require_profile(Some(profile), true).unwrap().is_some());
    }

    #[test]
    fn normal_protocol_launch_is_unchanged() {
        assert!(arguments(&["lemonade://open?view=logs"]).unwrap().is_none());
    }
    #[test]
    fn isolated_arguments_fail_closed() {
        for args in [
            vec!["--isolated-profile", "/tmp/profile"],
            vec!["--server-url", "http://localhost"],
            vec![
                "--isolated-profile",
                "relative",
                "--server-url",
                "http://localhost",
            ],
            vec![
                "--isolated-profile",
                "/tmp/profile",
                "--server-url",
                "localhost",
            ],
            vec![
                "--isolated-profile",
                "/tmp/profile",
                "--server-url",
                "http://secret@localhost",
            ],
            vec![
                "--isolated-profile",
                "/tmp/profile",
                "--server-url",
                "http://localhost",
                "lemonade://open",
            ],
        ] {
            assert!(arguments(&args).is_err(), "{args:?}");
        }
    }
    #[test]
    fn profile_lock_is_exclusive_and_released_without_touching_other_settings() {
        let root = std::env::temp_dir().join(format!(
            "lemonade-profile-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let normal = root.join("normal");
        fs::create_dir_all(&normal).unwrap();
        fs::write(normal.join("app_settings.json"), b"untouched").unwrap();
        let spec = IsolatedProfile {
            directory: root.join("isolated"),
            server_url: "http://127.0.0.1:1234".into(),
        };
        let (_, guard) = spec.clone().acquire(&[normal.clone()]).unwrap();
        assert!(spec.clone().acquire(&[normal.clone()]).is_err());
        assert!(IsolatedProfile {
            directory: normal.clone(),
            ..spec.clone()
        }
        .acquire(&[normal.clone()])
        .is_err());
        drop(guard);
        drop(spec.acquire(&[normal.clone()]).unwrap());
        assert_eq!(
            fs::read(normal.join("app_settings.json")).unwrap(),
            b"untouched"
        );
        let missing_normal = root.join("missing-normal");
        assert!(IsolatedProfile {
            directory: missing_normal.clone(),
            server_url: "http://localhost".into()
        }
        .acquire(&[missing_normal.clone()])
        .is_err());
        assert!(!missing_normal.exists());
        fs::remove_dir_all(root).unwrap();
    }
}
