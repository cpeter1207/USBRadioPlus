//! Thin dynamically loaded C clients. Radio policy remains in the remote-base DSO;
//! no Hamlib types, devices, sockets or controller implementation are embedded here.

use libloading::Library;
use std::{ffi::CStr, fmt, mem::size_of};

/// Installed C records. Their layout comes from the published development headers.
#[allow(
    missing_docs,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
pub mod raw {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

/// Provider load or contract-admission failure; no device has been opened.
#[derive(Debug)]
pub enum ProviderError {
    /// The runtime library or descriptor symbol is unavailable.
    Load(libloading::Error),
    /// The advertised table is incompatible or incomplete.
    Incompatible,
}

impl fmt::Display for ProviderError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Load(error) => error.fmt(formatter),
            Self::Incompatible => formatter.write_str("incompatible remote radio provider"),
        }
    }
}
impl std::error::Error for ProviderError {}

/// Loaded portable operating engine. Load only for an enabled remote composition.
/// Retain this owner until all contexts and callbacks have been destroyed.
pub struct RemoteProvider {
    _library: Library,
    api: raw::RptAdvRemoteApi,
}

impl RemoteProvider {
    /// Load the released SONAME through the system runtime search path.
    pub fn load() -> Result<Self, ProviderError> {
        Self::load_from("librptadvremote.so.1")
    }
    /// Load an explicit trusted library path, also used by isolated fixtures.
    pub fn load_from(path: &str) -> Result<Self, ProviderError> {
        // SAFETY: callers select trusted project libraries; their initialization
        // and descriptor follow the installed C contract.
        let library = unsafe { Library::new(path) }.map_err(ProviderError::Load)?;
        // SAFETY: the versioned symbol has the published descriptor signature,
        // and admission checks its complete prefix before copying the table.
        let api = unsafe {
            let entry = library
                .get::<unsafe extern "C" fn() -> *const raw::RptAdvRemoteApi>(
                    b"rptadv_remote_api_v1\0",
                )
                .map_err(ProviderError::Load)?;
            admit_remote(entry())?
        };
        Ok(Self {
            _library: library,
            api,
        })
    }
    /// Borrow the admitted function table while its executable storage is retained.
    pub fn api(&self) -> &raw::RptAdvRemoteApi {
        &self.api
    }
}

/// Loaded Hamlib dependency adapter. Parallel and ordinary radios do not load it.
pub struct HamlibProvider {
    _library: Library,
    api: raw::rptadv_hamlib_api,
}

impl HamlibProvider {
    /// Load the released SONAME, without opening any rig or transport.
    pub fn load() -> Result<Self, ProviderError> {
        Self::load_from("librptadv_hamlib_adapter.so.1")
    }
    /// Load an explicit trusted library path for a selected dependency or fixture.
    pub fn load_from(path: &str) -> Result<Self, ProviderError> {
        // SAFETY: only trusted project adapters are selected; the library remains
        // owned here through the final rig close and destroy on its creating thread.
        let library = unsafe { Library::new(path) }.map_err(ProviderError::Load)?;
        // SAFETY: the versioned symbol returns the installed descriptor, whose
        // complete shape and operations are checked before use.
        let api = unsafe {
            let entry = library
                .get::<unsafe extern "C" fn() -> *const raw::rptadv_hamlib_api>(
                    b"rptadv_hamlib_api_v1\0",
                )
                .map_err(ProviderError::Load)?;
            admit_hamlib(entry())?
        };
        Ok(Self {
            _library: library,
            api,
        })
    }
    /// Borrow typed operations; opened handles remain exclusive to their rig owner.
    pub fn api(&self) -> &raw::rptadv_hamlib_api {
        &self.api
    }
}

unsafe fn admit_remote(
    pointer: *const raw::RptAdvRemoteApi,
) -> Result<raw::RptAdvRemoteApi, ProviderError> {
    if pointer.is_null() || !pointer.is_aligned() {
        return Err(ProviderError::Incompatible);
    }
    // SAFETY: the descriptor lends its readable size/version prefix; the full
    // record is read only after exact current-size admission.
    let api = unsafe {
        if std::ptr::addr_of!((*pointer).struct_size).read()
            != size_of::<raw::RptAdvRemoteApi>() as u32
            || std::ptr::addr_of!((*pointer).abi_version).read() != 1
        {
            return Err(ProviderError::Incompatible);
        }
        pointer.read()
    };
    if api.capability_name.is_null()
        // SAFETY: a loaded provider supplies a readable NUL-terminated static name.
        || unsafe { CStr::from_ptr(api.capability_name) } != c"rptadv.remote"
        || api.create.is_none() || api.destroy.is_none() || api.configured_memory.is_none()
        || api.request.is_none() || api.advance.is_none() || api.snapshot.is_none()
        || api.take_completion.is_none() || api.retire.is_none() || api.scan_snapshot.is_none()
    {
        return Err(ProviderError::Incompatible);
    }
    Ok(api)
}

unsafe fn admit_hamlib(
    pointer: *const raw::rptadv_hamlib_api,
) -> Result<raw::rptadv_hamlib_api, ProviderError> {
    if pointer.is_null() || !pointer.is_aligned() {
        return Err(ProviderError::Incompatible);
    }
    // SAFETY: the readable prefix is checked before accessing the complete table.
    let api = unsafe {
        if std::ptr::addr_of!((*pointer).struct_size).read() != size_of::<raw::rptadv_hamlib_api>()
            || std::ptr::addr_of!((*pointer).abi_version).read() != 1
        {
            return Err(ProviderError::Incompatible);
        }
        pointer.read()
    };
    if api.name.is_null()
        // SAFETY: the admitted provider lends a readable static NUL-terminated name.
        || unsafe { CStr::from_ptr(api.name) } != c"rptadv.hamlib"
        || api.model_count.is_none() || api.model_name.is_none() || api.open.is_none()
        || api.close.is_none() || api.destroy.is_none() || api.setting.is_none()
        || api.choice.is_none() || api.get.is_none() || api.set.is_none()
        || api.transport_budget_ms.is_none() || api.range.is_none()
        || api.set_mode_width.is_none() || api.set_split_vfos.is_none() || api.set_power_mw.is_none()
    {
        return Err(ProviderError::Incompatible);
    }
    Ok(api)
}

#[cfg(test)]
mod tests;

mod hamlib;
pub use hamlib::{CallError, HamlibRig, Setting};
mod session;
pub use session::RemoteSession;
mod configuration;
pub use configuration::SelectedCapabilities;

// SAFETY: these owners retain immutable descriptor tables and loaded executable
// storage only. Rig/session contexts are separate, uniquely owned and not Send.
unsafe impl Send for RemoteProvider {}
// SAFETY: no mutable context is stored in the shared provider descriptor.
unsafe impl Sync for RemoteProvider {}
// SAFETY: Hamlib serializes its frontend lifecycle; per-rig contexts stay on
// their creating owner thread. This object contains only immutable API storage.
unsafe impl Send for HamlibProvider {}
// SAFETY: sharing the immutable function table never shares an opened rig.
unsafe impl Sync for HamlibProvider {}
