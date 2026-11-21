//! Exclude shared tty aliases before either CAT or modem setup touches a device.
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{File, OpenOptions};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt};
use std::sync::{LazyLock, Mutex};

use super::{GpioError, HardwareOperation, ServiceFailure};

static ACTIVE: LazyLock<Mutex<Vec<TransportClaim>>> = LazyLock::new(|| Mutex::new(Vec::new()));

#[derive(Clone, Default, Eq, PartialEq)]
pub(super) struct TransportClaim {
    serial: Option<u64>,
    cat: BTreeSet<u64>,
    unspecified_cat: bool,
}

const CAT_PATHS: [&str; 3] = ["rig_pathname", "ptt_pathname", "dcd_pathname"];

/// O_PATH resolves symlinks without opening the tty driver or changing line levels.
fn pin(path: &str, pins: &mut BTreeMap<String, File>) -> std::io::Result<Option<u64>> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_PATH)
        .open(path)?;
    let meta = file.metadata()?;
    if !meta.file_type().is_char_device() {
        return Ok(None);
    }
    let id = meta.rdev();
    pins.insert(path.to_owned(), file);
    Ok(Some(id))
}

impl TransportClaim {
    pub(super) fn acquire(
        serial: Option<&str>,
        cat: Option<(&str, &BTreeMap<String, String>)>,
    ) -> Result<Lease, ServiceFailure> {
        let failure = || ServiceFailure(HardwareOperation::Serial, GpioError::Unsupported);
        let mut claim = Self::default();
        let mut pins = BTreeMap::new();
        if let Some(path) = serial {
            let id = pin(path, &mut pins)
                .map_err(|_| ServiceFailure(HardwareOperation::Serial, GpioError::Io))?;
            claim.serial = Some(id.ok_or_else(failure)?);
        }
        if let Some((model, options)) = cat {
            // Some rigs have a model-dependent default serial transport. Its identity cannot
            // safely coexist with a separately owned tty unless the rig path is explicit.
            claim.unspecified_cat =
                model != "RIG_MODEL_DUMMY" && !options.contains_key("rig_pathname");
            for key in CAT_PATHS {
                let Some(path) = options.get(key) else {
                    continue;
                };
                if let Ok(device) = pin(path, &mut pins) {
                    if let Some(id) = device {
                        claim.cat.insert(id);
                    }
                } else if !path.rsplit_once(':').is_some_and(|(host, port)| {
                    !host.is_empty()
                        && !host.contains('/')
                        && port.parse::<u16>().is_ok_and(|port| port != 0)
                }) {
                    // A missing local pathname might resolve to an alias when hardware appears.
                    claim.unspecified_cat = true;
                }
            }
        }
        if claim.serial.is_none() && claim.cat.is_empty() && !claim.unspecified_cat {
            return Ok(Lease { claim: None, pins });
        }
        let mut active = ACTIVE.lock().map_err(|_| failure())?;
        let conflicts = |a: &Self, b: &Self| {
            a.serial
                .is_some_and(|id| b.unspecified_cat || b.cat.contains(&id))
        };
        if conflicts(&claim, &claim)
            || active.iter().any(|other| {
                conflicts(other, &claim)
                    || conflicts(&claim, other)
                    || claim.serial.is_some() && claim.serial == other.serial
                    || claim.unspecified_cat && (other.unspecified_cat || !other.cat.is_empty())
                    || other.unspecified_cat && !claim.cat.is_empty()
                    || !claim.cat.is_disjoint(&other.cat)
            })
        {
            return Err(failure());
        }
        active.push(claim.clone());
        Ok(Lease {
            claim: Some(claim),
            pins,
        })
    }
}

pub(super) struct Lease {
    claim: Option<TransportClaim>,
    pins: BTreeMap<String, File>,
}
impl Lease {
    pub(super) fn path(&self, path: &mut String) {
        if let Some(file) = self.pins.get(path) {
            *path = format!("/proc/self/fd/{}", file.as_raw_fd());
        }
    }
    pub(super) fn cat_options(
        &self,
        options: &BTreeMap<String, String>,
    ) -> Option<BTreeMap<String, String>> {
        if !CAT_PATHS.iter().any(|key| {
            options
                .get(*key)
                .is_some_and(|path| self.pins.contains_key(path))
        }) {
            return None;
        }
        let mut pinned = options.clone();
        for key in CAT_PATHS {
            if let Some(path) = pinned.get_mut(key) {
                self.path(path);
            }
        }
        Some(pinned)
    }
}
impl Drop for Lease {
    fn drop(&mut self) {
        if let Some(claim) = self.claim.take() {
            let mut active = ACTIVE.lock().unwrap_or_else(|error| error.into_inner());
            if let Some(i) = active.iter().position(|c| *c == claim) {
                active.swap_remove(i);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn physical_implicit_cat_excludes_other_unresolved_or_explicit_local_cat() {
        let unspecified = BTreeMap::new();
        let local = BTreeMap::from([("rig_pathname".into(), "/dev/zero".into())]);
        let implicit = Some(("RIG_MODEL_IC7300", &unspecified));
        let explicit = Some(("RIG_MODEL_IC7300", &local));
        for (first, second) in [
            (implicit, implicit),
            (implicit, explicit),
            (explicit, implicit),
        ] {
            let held = TransportClaim::acquire(None, first).unwrap();
            assert!(
                TransportClaim::acquire(None, second).is_err(),
                "uncertain local CAT ownership overlapped"
            );
            drop(held);
            assert!(TransportClaim::acquire(None, second).is_ok());
        }
    }

    #[test]
    fn physical_alias_claim_blocks_both_cat_orders_without_opening_a_tty() {
        let target = "/dev/null";
        let alias = std::env::temp_dir().join(format!("urp-modem-alias-{}", std::process::id()));
        std::os::unix::fs::symlink(target, &alias).unwrap();
        let options = BTreeMap::from([("rig_pathname".into(), alias.to_str().unwrap().into())]);
        let cat = Some(("RIG_MODEL_DUMMY", &options));
        assert!(TransportClaim::acquire(Some(target), cat).is_err());
        let owner = TransportClaim::acquire(None, cat).unwrap();
        assert!(TransportClaim::acquire(Some(target), None).is_err());
        drop(owner);
        let owner = TransportClaim::acquire(Some(target), None).unwrap();
        assert!(TransportClaim::acquire(None, cat).is_err());
        drop(owner);
        let owner = TransportClaim::acquire(None, cat).unwrap();
        let pinned = owner
            .cat_options(&options)
            .unwrap()
            .remove("rig_pathname")
            .unwrap();
        std::fs::remove_file(&alias).unwrap();
        std::os::unix::fs::symlink("/dev/zero", &alias).unwrap();
        assert_eq!(
            std::fs::metadata(&pinned).unwrap().rdev(),
            std::fs::metadata(target).unwrap().rdev()
        );
        assert_ne!(
            std::fs::metadata(&alias).unwrap().rdev(),
            std::fs::metadata(&pinned).unwrap().rdev()
        );
        std::fs::remove_file(alias).unwrap();
    }
}
