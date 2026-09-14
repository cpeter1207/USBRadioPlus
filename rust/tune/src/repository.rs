//! Durable storage for the unified `USBRadioPlus` configuration.

use std::fmt;
use std::fs::{self, File, Metadata, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const NEW_CONFIG_MODE: u32 = 0o640;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

/// Result of ensuring that the configured file exists.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CreationStatus {
    /// The configuration already existed and was not changed.
    Existing,
    /// A complete configuration was created from a shipped sample.
    Created,
}

/// Failure while reading or durably changing configuration storage.
#[derive(Debug)]
pub enum RepositoryError {
    /// The selected configuration path has no final file name.
    InvalidConfigurationPath(PathBuf),
    /// None of the configured uncompressed sample files was available.
    ShippedSampleUnavailable(Vec<PathBuf>),
    /// A filesystem operation failed at the named path.
    Io {
        /// Concise description of the attempted operation.
        operation: &'static str,
        /// File or directory involved in the failure.
        path: PathBuf,
        /// Operating-system error returned by the operation.
        source: io::Error,
    },
}

impl RepositoryError {
    fn io(operation: &'static str, path: &Path, source: io::Error) -> Self {
        Self::Io {
            operation,
            path: path.to_path_buf(),
            source,
        }
    }
}

impl fmt::Display for RepositoryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfigurationPath(path) => write!(
                formatter,
                "configuration path has no file name: {}",
                path.display()
            ),
            Self::ShippedSampleUnavailable(candidates) => write!(
                formatter,
                "no uncompressed shipped configuration sample was found in: {}",
                candidates
                    .iter()
                    .map(|path| path.display().to_string())
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
            Self::Io {
                operation,
                path,
                source,
            } => write!(formatter, "{operation} {}: {source}", path.display()),
        }
    }
}

impl std::error::Error for RepositoryError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}

/// Filesystem repository for one unified `USBRadioPlus` configuration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ConfigRepository {
    config_path: PathBuf,
    sample_candidates: Vec<PathBuf>,
}

impl ConfigRepository {
    /// Construct a repository with ordered, uncompressed sample candidates.
    pub fn new<I, P>(config_path: impl Into<PathBuf>, sample_candidates: I) -> Self
    where
        I: IntoIterator<Item = P>,
        P: Into<PathBuf>,
    {
        Self {
            config_path: config_path.into(),
            sample_candidates: sample_candidates.into_iter().map(Into::into).collect(),
        }
    }

    /// Create a missing configuration from the first readable shipped sample.
    ///
    /// The completed temporary inode is linked into place atomically, so a
    /// concurrent creator is never overwritten or exposed to partial content.
    ///
    /// # Errors
    ///
    /// Returns [`RepositoryError`] when the destination is invalid or its
    /// parent, a shipped sample, or the filesystem cannot be accessed.
    pub fn ensure_exists(&self) -> Result<CreationStatus, RepositoryError> {
        let parent = self.parent_directory()?;
        fs::create_dir_all(parent)
            .map_err(|error| RepositoryError::io("create directory", parent, error))?;
        if path_exists(&self.config_path)? {
            return Ok(CreationStatus::Existing);
        }
        let contents = self.read_first_sample()?;
        creation_status(self.write_new(contents.as_bytes()))
    }

    /// Read the current configuration as UTF-8 text.
    ///
    /// # Errors
    ///
    /// Returns [`RepositoryError`] when the file cannot be read as UTF-8.
    pub fn read(&self) -> Result<String, RepositoryError> {
        fs::read_to_string(&self.config_path)
            .map_err(|error| RepositoryError::io("read configuration", &self.config_path, error))
    }

    /// Atomically replace the configuration while retaining ownership and mode.
    ///
    /// Data and metadata are synchronized before replacement, followed by the
    /// containing directory. Atomic replacement is guaranteed on the supported
    /// Unix platforms when source and destination share this directory.
    ///
    /// # Errors
    ///
    /// Returns [`RepositoryError`] when metadata, temporary-file, replacement,
    /// or directory synchronization operations fail.
    pub fn write_atomic(&self, contents: &str) -> Result<(), RepositoryError> {
        let metadata = fs::metadata(&self.config_path).map_err(|error| {
            RepositoryError::io("read configuration metadata", &self.config_path, error)
        })?;
        let mut pending = self.write_temporary(contents.as_bytes(), Some(&metadata))?;
        replace_file(pending.path(), &self.config_path).map_err(|error| {
            RepositoryError::io("replace configuration", &self.config_path, error)
        })?;
        pending.commit();
        self.sync_parent()
    }

    /// Create a timestamped backup beside the configuration file.
    ///
    /// Backup files receive an independent inode and preserve the source
    /// ownership and permission mode.
    ///
    /// # Errors
    ///
    /// Returns [`RepositoryError`] when the source cannot be read, a unique
    /// destination cannot be allocated, or durable filesystem operations fail.
    pub fn create_backup(&self) -> Result<PathBuf, RepositoryError> {
        let metadata = fs::metadata(&self.config_path).map_err(|error| {
            RepositoryError::io("read configuration metadata", &self.config_path, error)
        })?;
        let contents = fs::read(&self.config_path)
            .map_err(|error| RepositoryError::io("read configuration", &self.config_path, error))?;
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let backup_path = append_to_file_name(
            &self.config_path,
            &format!(".bak.{timestamp}.{}.{}", std::process::id(), sequence),
        )?;
        let mut pending = self.write_temporary(&contents, Some(&metadata))?;
        link_file(pending.path(), &backup_path, "create backup")?;
        pending.remove()?;
        self.sync_parent()?;
        Ok(backup_path)
    }

    fn read_first_sample(&self) -> Result<String, RepositoryError> {
        for candidate in &self.sample_candidates {
            match fs::read_to_string(candidate) {
                Ok(contents) => return Ok(contents),
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(error) => {
                    return Err(RepositoryError::io(
                        "read shipped configuration sample",
                        candidate,
                        error,
                    ));
                }
            }
        }
        Err(RepositoryError::ShippedSampleUnavailable(
            self.sample_candidates.clone(),
        ))
    }

    fn write_new(&self, contents: &[u8]) -> Result<(), RepositoryError> {
        let mut pending = self.write_temporary(contents, None)?;
        link_file(pending.path(), &self.config_path, "create configuration")?;
        pending.remove()?;
        self.sync_parent()
    }

    fn write_temporary(
        &self,
        contents: &[u8],
        metadata: Option<&Metadata>,
    ) -> Result<PendingFile, RepositoryError> {
        let file_name = self
            .config_path
            .file_name()
            .ok_or_else(|| RepositoryError::InvalidConfigurationPath(self.config_path.clone()))?
            .to_string_lossy();
        let parent = self.parent_directory()?;
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let path = parent.join(format!(
            ".{file_name}.tmp.{}.{}",
            std::process::id(),
            sequence
        ));
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .map_err(|error| RepositoryError::io("create temporary file", &path, error))?;
        prepare_pending(file, path, contents, metadata)
    }

    fn parent_directory(&self) -> Result<&Path, RepositoryError> {
        if self.config_path.file_name().is_none() {
            return Err(RepositoryError::InvalidConfigurationPath(
                self.config_path.clone(),
            ));
        }
        Ok(self
            .config_path
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new(".")))
    }

    fn sync_parent(&self) -> Result<(), RepositoryError> {
        sync_directory(self.parent_directory()?)
    }
}

fn creation_status(result: Result<(), RepositoryError>) -> Result<CreationStatus, RepositoryError> {
    match result {
        Ok(()) => Ok(CreationStatus::Created),
        Err(RepositoryError::Io { source, .. })
            if source.kind() == io::ErrorKind::AlreadyExists =>
        {
            Ok(CreationStatus::Existing)
        }
        Err(error) => Err(error),
    }
}

fn prepare_pending(
    mut file: File,
    path: PathBuf,
    contents: &[u8],
    metadata: Option<&Metadata>,
) -> Result<PendingFile, RepositoryError> {
    if let Err(error) = prepare_file(&mut file, &path, contents, metadata) {
        drop(file);
        let _ = fs::remove_file(&path);
        return Err(error);
    }
    Ok(PendingFile {
        path,
        remove_on_drop: true,
    })
}

fn prepare_file(
    file: &mut File,
    path: &Path,
    contents: &[u8],
    metadata: Option<&Metadata>,
) -> Result<(), RepositoryError> {
    file.write_all(contents)
        .map_err(|error| RepositoryError::io("write temporary file", path, error))?;
    if let Some(metadata) = metadata {
        preserve_owner(file, path, metadata)?;
        file.set_permissions(metadata.permissions())
            .map_err(|error| RepositoryError::io("preserve file permissions", path, error))?;
    } else {
        set_new_file_mode(file, path)?;
    }
    file.sync_all()
        .map_err(|error| RepositoryError::io("synchronize temporary file", path, error))
}

fn append_to_file_name(path: &Path, suffix: &str) -> Result<PathBuf, RepositoryError> {
    let file_name = path
        .file_name()
        .ok_or_else(|| RepositoryError::InvalidConfigurationPath(path.to_path_buf()))?;
    let mut backup_name = file_name.to_os_string();
    backup_name.push(suffix);
    Ok(path.with_file_name(backup_name))
}

fn path_exists(path: &Path) -> Result<bool, RepositoryError> {
    path.try_exists()
        .map_err(|error| RepositoryError::io("inspect path", path, error))
}

fn link_file(
    source: &Path,
    destination: &Path,
    operation: &'static str,
) -> Result<(), RepositoryError> {
    fs::hard_link(source, destination)
        .map_err(|error| RepositoryError::io(operation, destination, error))
}

struct PendingFile {
    path: PathBuf,
    remove_on_drop: bool,
}

impl PendingFile {
    fn path(&self) -> &Path {
        &self.path
    }

    fn commit(&mut self) {
        self.remove_on_drop = false;
    }

    fn remove(&mut self) -> Result<(), RepositoryError> {
        fs::remove_file(&self.path)
            .map_err(|error| RepositoryError::io("remove temporary file", &self.path, error))?;
        self.remove_on_drop = false;
        Ok(())
    }
}

impl Drop for PendingFile {
    fn drop(&mut self) {
        if self.remove_on_drop {
            let _ = fs::remove_file(&self.path);
        }
    }
}

#[cfg(unix)]
fn set_new_file_mode(file: &File, path: &Path) -> Result<(), RepositoryError> {
    use std::os::unix::fs::PermissionsExt;
    file.set_permissions(fs::Permissions::from_mode(NEW_CONFIG_MODE))
        .map_err(|error| RepositoryError::io("set configuration permissions", path, error))
}

#[cfg(not(unix))]
fn set_new_file_mode(_file: &File, _path: &Path) -> Result<(), RepositoryError> {
    Ok(())
}

#[cfg(unix)]
fn preserve_owner(file: &File, path: &Path, metadata: &Metadata) -> Result<(), RepositoryError> {
    use std::os::fd::AsRawFd;
    use std::os::unix::fs::MetadataExt;

    unsafe extern "C" {
        fn fchown(file_descriptor: i32, owner: u32, group: u32) -> i32;
    }

    let temporary_metadata = file
        .metadata()
        .map_err(|error| RepositoryError::io("read temporary file metadata", path, error))?;
    if temporary_metadata.uid() == metadata.uid() && temporary_metadata.gid() == metadata.gid() {
        return Ok(());
    }
    // SAFETY: The descriptor is borrowed from an open file for the duration of
    // this call, and Debian defines uid_t and gid_t as unsigned 32-bit values.
    if unsafe { fchown(file.as_raw_fd(), metadata.uid(), metadata.gid()) } == 0 {
        Ok(())
    } else {
        Err(RepositoryError::io(
            "preserve file ownership",
            path,
            io::Error::last_os_error(),
        ))
    }
}

#[cfg(not(unix))]
fn preserve_owner(_file: &File, _path: &Path, _metadata: &Metadata) -> Result<(), RepositoryError> {
    Ok(())
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), RepositoryError> {
    let directory = File::open(path)
        .map_err(|error| RepositoryError::io("open containing directory", path, error))?;
    directory
        .sync_all()
        .map_err(|error| RepositoryError::io("synchronize containing directory", path, error))
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), RepositoryError> {
    Ok(())
}

#[cfg(unix)]
fn replace_file(source: &Path, destination: &Path) -> io::Result<()> {
    fs::rename(source, destination)
}

#[cfg(not(unix))]
fn replace_file(source: &Path, destination: &Path) -> io::Result<()> {
    if destination.try_exists()? {
        fs::remove_file(destination)?;
    }
    fs::rename(source, destination)
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
mod tests {
    use super::*;

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new(label: &str) -> Self {
            let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!(
                "usbradioplus-tune-{label}-{}-{sequence}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn missing_configuration_uses_the_first_readable_plain_sample() {
        let directory = TestDirectory::new("create");
        let missing = directory.0.join("missing.sample");
        let sample = directory.0.join("shipped.sample");
        fs::write(&sample, "[general]\nchannel_enabled = yes\n").unwrap();
        let config = directory.0.join("etc/asterisk/usbradioplus.conf");
        let repository = ConfigRepository::new(&config, [&missing, &sample]);

        assert_eq!(repository.ensure_exists().unwrap(), CreationStatus::Created);
        assert_eq!(
            repository.read().unwrap(),
            fs::read_to_string(sample).unwrap()
        );
        assert_eq!(
            repository.ensure_exists().unwrap(),
            CreationStatus::Existing
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(config).unwrap().permissions().mode() & 0o777,
                0o640
            );
        }
    }

    #[test]
    fn unavailable_or_unreadable_samples_are_reported_without_a_partial_config() {
        let directory = TestDirectory::new("missing-sample");
        let config = directory.0.join("config");
        let missing = directory.0.join("missing");
        let repository = ConfigRepository::new(&config, [&missing]);
        let error = repository.ensure_exists().unwrap_err();
        assert!(matches!(
            error,
            RepositoryError::ShippedSampleUnavailable(_)
        ));
        assert!(error.to_string().contains(missing.to_str().unwrap()));
        assert!(!config.exists());

        let bad_sample = directory.0.join("bad-sample");
        fs::create_dir(&bad_sample).unwrap();
        let repository = ConfigRepository::new(&config, [&bad_sample]);
        let error = repository.ensure_exists().unwrap_err();
        assert!(matches!(error, RepositoryError::Io { .. }));
        assert!(std::error::Error::source(&error).is_some());
        assert!(!config.exists());
    }

    #[test]
    fn atomic_write_preserves_mode_and_cleans_its_temporary_file() {
        let directory = TestDirectory::new("replace");
        let config = directory.0.join("config");
        fs::write(&config, "old").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&config, fs::Permissions::from_mode(0o604)).unwrap();
        }
        let repository = ConfigRepository::new(&config, std::iter::empty::<PathBuf>());
        repository.write_atomic("new\n").unwrap();
        assert_eq!(repository.read().unwrap(), "new\n");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(&config).unwrap().permissions().mode() & 0o777,
                0o604
            );
        }
        assert_eq!(
            fs::read_dir(&directory.0)
                .unwrap()
                .filter_map(Result::ok)
                .filter(|entry| entry.file_name().to_string_lossy().contains(".tmp."))
                .count(),
            0
        );
    }

    #[test]
    fn backup_is_independent_and_timestamped() {
        let directory = TestDirectory::new("backup");
        let config = directory.0.join("radio.conf");
        fs::write(&config, "original\n").unwrap();
        let repository = ConfigRepository::new(&config, std::iter::empty::<PathBuf>());
        let backup = repository.create_backup().unwrap();
        assert!(
            backup
                .file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with("radio.conf.bak.")
        );
        assert_eq!(fs::read_to_string(&backup).unwrap(), "original\n");
        repository.write_atomic("changed\n").unwrap();
        assert_eq!(fs::read_to_string(&backup).unwrap(), "original\n");
    }

    #[test]
    fn invalid_paths_have_contextual_errors() {
        let repository = ConfigRepository::new("/", std::iter::empty::<PathBuf>());
        let error = repository.ensure_exists().unwrap_err();
        assert!(matches!(
            error,
            RepositoryError::InvalidConfigurationPath(_)
        ));
        assert!(error.to_string().contains("no file name"));
    }

    #[test]
    fn ordinary_filesystem_failures_retain_the_failed_operation() {
        let directory = TestDirectory::new("io-errors");
        let missing = directory.0.join("missing");
        let repository = ConfigRepository::new(&missing, std::iter::empty::<PathBuf>());
        assert!(
            repository
                .read()
                .unwrap_err()
                .to_string()
                .contains("read configuration")
        );
        assert!(
            repository
                .write_atomic("new")
                .unwrap_err()
                .to_string()
                .contains("read configuration metadata")
        );
        assert!(
            repository
                .create_backup()
                .unwrap_err()
                .to_string()
                .contains("read configuration metadata")
        );

        let destination_directory = directory.0.join("destination-directory");
        fs::create_dir(&destination_directory).unwrap();
        let repository =
            ConfigRepository::new(&destination_directory, std::iter::empty::<PathBuf>());
        assert!(
            repository
                .write_atomic("replacement")
                .unwrap_err()
                .to_string()
                .contains("replace configuration")
        );
        assert!(
            repository
                .create_backup()
                .unwrap_err()
                .to_string()
                .contains("read configuration")
        );

        let parent_file = directory.0.join("parent-file");
        fs::write(&parent_file, "x").unwrap();
        let sample = directory.0.join("sample");
        fs::write(&sample, "sample").unwrap();
        let nested = ConfigRepository::new(parent_file.join("config"), [&sample]);
        assert!(
            nested
                .ensure_exists()
                .unwrap_err()
                .to_string()
                .contains("create directory")
        );

        let existing = directory.0.join("existing");
        fs::write(&existing, "x").unwrap();
        let repository = ConfigRepository::new(&existing, std::iter::empty::<PathBuf>());
        assert!(
            repository
                .write_new(b"new")
                .unwrap_err()
                .to_string()
                .contains("create configuration")
        );
        assert!(
            sync_directory(&missing)
                .unwrap_err()
                .to_string()
                .contains("open containing directory")
        );
        assert!(replace_file(&missing, &existing).is_err());

        let already_exists = RepositoryError::io(
            "create configuration",
            &existing,
            io::Error::from(io::ErrorKind::AlreadyExists),
        );
        assert_eq!(
            creation_status(Err(already_exists)).unwrap(),
            CreationStatus::Existing
        );
        assert_eq!(creation_status(Ok(())).unwrap(), CreationStatus::Created);
        assert!(
            creation_status(Err(RepositoryError::io(
                "create configuration",
                &existing,
                io::Error::other("failed"),
            )))
            .is_err()
        );
        assert!(creation_status(Err(RepositoryError::InvalidConfigurationPath(existing))).is_err());

        let relative = ConfigRepository::new("radio.conf", std::iter::empty::<PathBuf>());
        assert_eq!(relative.parent_directory().unwrap(), Path::new("."));
        let invalid = ConfigRepository::new("/", std::iter::empty::<PathBuf>());
        assert!(invalid.write_temporary(b"x", None).is_err());
    }

    #[test]
    fn temporary_files_are_removed_on_failure_and_drop() {
        let directory = TestDirectory::new("pending");
        let path = directory.0.join("pending");
        fs::write(&path, "temporary").unwrap();
        drop(PendingFile {
            path: path.clone(),
            remove_on_drop: true,
        });
        assert!(!path.exists());
        let mut missing_pending = PendingFile {
            path,
            remove_on_drop: true,
        };
        assert!(missing_pending.remove().is_err());

        let no_parent = directory.0.join("missing-parent/config");
        let repository = ConfigRepository::new(&no_parent, std::iter::empty::<PathBuf>());
        let error = repository.write_temporary(b"x", None).err().unwrap();
        assert!(error.to_string().contains("create temporary file"));

        #[cfg(unix)]
        {
            let mut full = OpenOptions::new().write(true).open("/dev/full").unwrap();
            let error = prepare_file(&mut full, Path::new("/dev/full"), b"x", None).unwrap_err();
            assert!(error.to_string().contains("write temporary file"));

            let cleanup = directory.0.join("failed-pending");
            fs::write(&cleanup, "remove me").unwrap();
            let full = OpenOptions::new().write(true).open("/dev/full").unwrap();
            assert!(prepare_pending(full, cleanup.clone(), b"x", None).is_err());
            assert!(!cleanup.exists());
        }
    }

    #[cfg(unix)]
    #[test]
    fn invalid_unix_paths_and_special_files_cover_defensive_io() {
        use std::ffi::{CString, OsString};
        use std::mem::ManuallyDrop;
        use std::os::fd::AsRawFd;
        use std::os::unix::ffi::{OsStrExt, OsStringExt};
        use std::os::unix::fs::MetadataExt;

        unsafe extern "C" {
            fn chown(path: *const std::ffi::c_char, owner: u32, group: u32) -> i32;
            fn close(file_descriptor: i32) -> i32;
        }

        let invalid = PathBuf::from(OsString::from_vec(b"bad\0path".to_vec()));
        assert!(path_exists(&invalid).is_err());
        assert!(append_to_file_name(Path::new("/"), ".bak").is_err());

        let directory = TestDirectory::new("owner");
        let desired_path = directory.0.join("desired");
        fs::write(&desired_path, "x").unwrap();
        let desired_path_c = CString::new(desired_path.as_os_str().as_bytes()).unwrap();
        // SAFETY: the path is a live, NUL-terminated C string and `chown` does
        // not retain its pointer. Tuner tests run with the root policy required
        // by the installed utility.
        assert_eq!(unsafe { chown(desired_path_c.as_ptr(), 65_534, 65_534) }, 0);
        let desired = fs::metadata(&desired_path).unwrap();
        assert_eq!(desired.uid(), 65_534);

        let owned_path = directory.0.join("owned");
        fs::write(&owned_path, "x").unwrap();
        let owned = OpenOptions::new().write(true).open(&owned_path).unwrap();
        preserve_owner(&owned, &owned_path, &desired).unwrap();
        assert_eq!(owned.metadata().unwrap().uid(), desired.uid());

        let group_path = directory.0.join("group");
        fs::write(&group_path, "x").unwrap();
        let group_path_c = CString::new(group_path.as_os_str().as_bytes()).unwrap();
        let current_uid = fs::metadata(&group_path).unwrap().uid();
        // SAFETY: the same live, NUL-terminated path contract used above applies.
        let chown_result = unsafe { chown(group_path_c.as_ptr(), current_uid, 65_534) };
        assert_eq!(chown_result, 0);
        let desired_group = fs::metadata(&group_path).unwrap();
        let group_target_path = directory.0.join("group-target");
        fs::write(&group_target_path, "x").unwrap();
        let group_target = OpenOptions::new()
            .write(true)
            .open(&group_target_path)
            .unwrap();
        preserve_owner(&group_target, &group_target_path, &desired_group).unwrap();
        assert_eq!(group_target.metadata().unwrap().gid(), desired_group.gid());

        let read_only_path = Path::new("/sys/kernel/uevent_seqnum");
        let read_only = File::open(read_only_path).unwrap();
        assert!(preserve_owner(&read_only, read_only_path, &desired).is_err());
        assert!(set_new_file_mode(&read_only, read_only_path).is_err());

        let read_only_metadata = fs::metadata(read_only_path).unwrap();
        let mut read_only = File::open(read_only_path).unwrap();
        assert!(
            prepare_file(
                &mut read_only,
                read_only_path,
                b"",
                Some(&read_only_metadata),
            )
            .is_err()
        );

        let invalid_file = ManuallyDrop::new(File::open(&desired_path).unwrap());
        // SAFETY: the descriptor is intentionally invalidated to exercise the
        // defensive metadata error; `ManuallyDrop` prevents a second close.
        assert_eq!(unsafe { close(invalid_file.as_raw_fd()) }, 0);
        assert!(preserve_owner(&invalid_file, &desired_path, &desired).is_err());

        let null_metadata = fs::metadata("/dev/null").unwrap();
        let mut null = OpenOptions::new().write(true).open("/dev/null").unwrap();
        assert!(
            prepare_file(&mut null, Path::new("/dev/null"), b"", Some(&null_metadata)).is_err()
        );

        let error = RepositoryError::InvalidConfigurationPath(PathBuf::from("/"));
        assert!(std::error::Error::source(&error).is_none());
        assert!(sync_directory(Path::new("/proc")).is_err());
    }
}
