//! Transactional ownership of the Asterisk module lifecycle.

use std::ffi::{c_char, c_int, c_void};
use std::mem::size_of;

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::Mutex;

use crate::{URP_AST_CHANNEL_BUSY, URP_AST_NOT_READY, URP_AST_OK};

use super::{channel, cli, delivery, link, reload};
use crate::{
    ABI_VERSION, URP_AST_ASTERISK_FAILURE, URP_AST_INCOMPATIBLE_ABI, UrpAstDriverCreateArgs,
    UrpAstProviderManifest,
};

/// Loader ABI implemented by this Rust-owned Asterisk host.
pub(super) const LOADER_ABI_VERSION: u32 = 4;

const LOADER_OK: c_int = 0;
const LOADER_DECLINE: c_int = 1;
const LOADER_FAILURE: c_int = 2;
const LOADER_CAPABILITY: &std::ffi::CStr = c"usbradioplus.asterisk-loader";

/// Process-lifetime provider composition supplied by the metadata-only module.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct LoaderProviderManifest {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Must equal [`LOADER_ABI_VERSION`].
    pub abi_version: u32,
    /// Released FFmpeg graph adapter descriptor.
    pub ffmpeg: *const c_void,
    /// Released RNNoise adapter descriptor.
    pub rnnoise: *const c_void,
    /// Released F32 rate-adjusting ring descriptor.
    pub ring: *const c_void,
    /// Released radio-core descriptor.
    pub radio: *const c_void,
    /// Released libsamplerate adapter descriptor.
    pub samplerate: *const c_void,
    /// Released PortAudio/ALSA adapter descriptor.
    pub audio: *const c_void,
    /// Released CM119/parallel GPIO adapter descriptor.
    pub gpio: *const c_void,
}

/// Validate the complete outer loader manifest before constructing a driver.
pub(super) fn manifest_is_valid(manifest: &LoaderProviderManifest) -> bool {
    manifest.struct_size as usize >= size_of::<LoaderProviderManifest>()
        && manifest.abi_version == LOADER_ABI_VERSION
        && !manifest.ffmpeg.is_null()
        && !manifest.rnnoise.is_null()
        && !manifest.ring.is_null()
        && !manifest.radio.is_null()
        && !manifest.samplerate.is_null()
        && !manifest.audio.is_null()
        && !manifest.gpio.is_null()
}

/// Immutable lifecycle table consumed by the metadata-only Asterisk module.
#[repr(C)]
pub struct AsteriskLoaderDescriptor {
    /// Size of this structure in bytes.
    pub struct_size: u32,
    /// Loader ABI version.
    pub abi_version: u32,
    /// NUL-terminated capability name.
    pub capability: *const c_char,
    /// Start the complete Rust-owned host.
    pub load: Option<unsafe extern "C" fn(*const LoaderProviderManifest, *mut c_void) -> c_int>,
    /// Reload the active host in place.
    pub reload: Option<extern "C" fn() -> c_int>,
    /// Stop the host after its channels become idle.
    pub unload: Option<extern "C" fn() -> c_int>,
}

// SAFETY: the descriptor and everything it references have process lifetime.
unsafe impl Sync for AsteriskLoaderDescriptor {}

/// Host operations sequenced by [`LifecycleCoordinator`].
///
/// Each registration method is all-or-nothing. Channel unregistration doubles
/// as the unload admission check and returns busy while a channel is active.
pub(super) trait LifecycleOperations {
    /// Validate the complete provider composition before reading configuration.
    fn validate_providers(&mut self) -> i32;
    /// Read and validate configuration, then construct the Rust driver.
    fn create_driver(&mut self) -> i32;
    /// Destroy the previously constructed driver.
    fn destroy_driver(&mut self);
    /// Register both Asterisk channel technologies.
    fn register_channels(&mut self) -> i32;
    /// Unregister both technologies, or return busy without changing them.
    fn unregister_channels(&mut self) -> i32;
    /// Register the complete CLI command set.
    fn register_cli(&mut self) -> i32;
    /// Unregister the CLI command set.
    fn unregister_cli(&mut self);
    /// Start incoming-link discovery and attachment.
    fn start_links(&mut self) -> i32;
    /// Stop discovery and detach every incoming-link graph.
    fn stop_links(&mut self);
    /// Run one complete transactional configuration reload.
    fn reload(&mut self) -> i32;
}

/// Minimal state needed to make load, reload, and unload transactional.
pub(super) struct LifecycleCoordinator {
    loaded: bool,
}

impl LifecycleCoordinator {
    /// Construct an unloaded coordinator.
    pub(super) const fn new() -> Self {
        Self { loaded: false }
    }

    /// Validate and publish every module-owned service in dependency order.
    pub(super) fn load(&mut self, operations: &mut impl LifecycleOperations) -> i32 {
        if self.loaded {
            return URP_AST_CHANNEL_BUSY;
        }
        let status = operations.validate_providers();
        if status != URP_AST_OK {
            return status;
        }
        let status = operations.create_driver();
        if status != URP_AST_OK {
            return status;
        }
        let status = operations.register_cli();
        if status != URP_AST_OK {
            operations.destroy_driver();
            return status;
        }
        let status = operations.start_links();
        if status != URP_AST_OK {
            operations.unregister_cli();
            operations.destroy_driver();
            return status;
        }
        let status = operations.register_channels();
        if status != URP_AST_OK {
            operations.stop_links();
            operations.unregister_cli();
            operations.destroy_driver();
            return status;
        }
        self.loaded = true;
        URP_AST_OK
    }

    /// Forward reload only after a complete successful load.
    pub(super) fn reload(&self, operations: &mut impl LifecycleOperations) -> i32 {
        if self.loaded {
            operations.reload()
        } else {
            URP_AST_NOT_READY
        }
    }

    /// Refuse active channels, then release every owned service exactly once.
    pub(super) fn unload(&mut self, operations: &mut impl LifecycleOperations) -> i32 {
        if !self.loaded {
            return URP_AST_OK;
        }
        let status = operations.unregister_channels();
        if status != URP_AST_OK {
            return status;
        }
        operations.stop_links();
        operations.unregister_cli();
        operations.destroy_driver();
        self.loaded = false;
        URP_AST_OK
    }
}

#[derive(Clone, Copy)]
struct ProviderAddresses {
    ffmpeg: usize,
    rnnoise: usize,
    ring: usize,
    radio: usize,
    samplerate: usize,
    audio: usize,
    gpio: usize,
}

impl ProviderAddresses {
    fn from_manifest(manifest: &LoaderProviderManifest) -> Self {
        Self {
            ffmpeg: manifest.ffmpeg as usize,
            rnnoise: manifest.rnnoise as usize,
            ring: manifest.ring as usize,
            radio: manifest.radio as usize,
            samplerate: manifest.samplerate as usize,
            audio: manifest.audio as usize,
            gpio: manifest.gpio as usize,
        }
    }

    fn loader_manifest(self) -> LoaderProviderManifest {
        LoaderProviderManifest {
            struct_size: size_of::<LoaderProviderManifest>() as u32,
            abi_version: LOADER_ABI_VERSION,
            ffmpeg: self.ffmpeg as *const c_void,
            rnnoise: self.rnnoise as *const c_void,
            ring: self.ring as *const c_void,
            radio: self.radio as *const c_void,
            samplerate: self.samplerate as *const c_void,
            audio: self.audio as *const c_void,
            gpio: self.gpio as *const c_void,
        }
    }

    fn product_manifest(self) -> UrpAstProviderManifest {
        UrpAstProviderManifest {
            struct_size: size_of::<UrpAstProviderManifest>() as u32,
            abi_version: ABI_VERSION,
            ffmpeg: self.ffmpeg as *const c_void,
            rnnoise: self.rnnoise as *const c_void,
            ring: self.ring as *const c_void,
            radio: self.radio as *const c_void,
            samplerate: self.samplerate as *const c_void,
            audio: self.audio as *const c_void,
            gpio: self.gpio as *const c_void,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum LoadPhase {
    Validate,
    Create,
    Register,
}

struct ProductionOperations {
    providers: ProviderAddresses,
    module: usize,
    descriptor: usize,
    driver: usize,
    phase: LoadPhase,
}

impl ProductionOperations {
    fn new(providers: ProviderAddresses, module: *mut c_void) -> Self {
        Self {
            providers,
            module: module as usize,
            descriptor: ptr::from_ref(crate::product_descriptor()) as usize,
            driver: 0,
            phase: LoadPhase::Validate,
        }
    }
}

impl LifecycleOperations for ProductionOperations {
    fn validate_providers(&mut self) -> i32 {
        self.phase = LoadPhase::Validate;
        if manifest_is_valid(&self.providers.loader_manifest()) {
            URP_AST_OK
        } else {
            URP_AST_INCOMPATIBLE_ABI
        }
    }

    fn create_driver(&mut self) -> i32 {
        self.phase = LoadPhase::Create;
        let Ok(configuration) = reload::read_configuration() else {
            return URP_AST_ASTERISK_FAILURE;
        };
        // SAFETY: the internal descriptor has process lifetime.
        let descriptor = unsafe { &*(self.descriptor as *const crate::UrpAstDescriptor) };
        let Some(create) = descriptor.driver_create else {
            return URP_AST_INCOMPATIBLE_ABI;
        };
        let providers = self.providers.product_manifest();
        let operations = delivery::usbradioplus_asterisk_channel_host_operations();
        let agc = env!("USBRADIOPLUS_AGC_PLUGIN_PATH").as_bytes();
        let args = UrpAstDriverCreateArgs {
            struct_size: size_of::<UrpAstDriverCreateArgs>() as u32,
            abi_version: ABI_VERSION,
            config_source: configuration.source.as_ptr(),
            config_source_length: configuration.source.len() as u32,
            config_text: configuration.text.as_ptr(),
            config_text_length: configuration.text.len() as u32,
            agc_plugin_path: agc.as_ptr(),
            agc_plugin_path_length: agc.len() as u32,
            operations,
            providers: &raw const providers,
        };
        let mut driver = ptr::null_mut();
        // SAFETY: every pointer in args remains valid for this synchronous call.
        let status = unsafe { create(&raw const args, &raw mut driver) };
        if status == URP_AST_OK {
            self.driver = driver as usize;
        }
        status
    }

    fn destroy_driver(&mut self) {
        if self.driver == 0 {
            return;
        }
        // SAFETY: the internal descriptor has process lifetime.
        let descriptor = unsafe { &*(self.descriptor as *const crate::UrpAstDescriptor) };
        if let Some(destroy) = descriptor.driver_destroy {
            // SAFETY: this is the one live driver handle returned by driver_create.
            unsafe { destroy(self.driver as *mut c_void) };
        }
        self.driver = 0;
    }

    fn register_channels(&mut self) -> i32 {
        self.phase = LoadPhase::Register;
        // SAFETY: descriptor, driver, and module live until successful unload.
        unsafe {
            channel::usbradioplus_asterisk_channel_host_register(
                self.descriptor as *const crate::UrpAstDescriptor,
                self.driver as *mut c_void,
                self.module as *mut c_void,
            )
        }
    }

    fn unregister_channels(&mut self) -> i32 {
        channel::usbradioplus_asterisk_channel_host_unregister()
    }

    fn register_cli(&mut self) -> i32 {
        self.phase = LoadPhase::Register;
        cli::register(self.module as *mut crate::ffi::ast_module)
    }

    fn unregister_cli(&mut self) {
        cli::unregister();
    }

    fn start_links(&mut self) -> i32 {
        self.phase = LoadPhase::Register;
        link::start(
            self.driver as *mut c_void,
            self.module as *mut crate::ffi::ast_module,
        )
    }

    fn stop_links(&mut self) {
        link::stop();
    }

    fn reload(&mut self) -> i32 {
        reload::reload()
    }
}

struct LifecycleHost {
    coordinator: LifecycleCoordinator,
    operations: Option<ProductionOperations>,
}

static LIFECYCLE: Mutex<LifecycleHost> = Mutex::new(LifecycleHost {
    coordinator: LifecycleCoordinator::new(),
    operations: None,
});

unsafe extern "C" fn loader_load(
    providers: *const LoaderProviderManifest,
    module: *mut c_void,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        if providers.is_null() || module.is_null() {
            return LOADER_DECLINE;
        }
        // SAFETY: the metadata-only module supplies this readable structure for
        // the duration of the synchronous call.
        let providers = unsafe { &*providers };
        if !manifest_is_valid(providers) {
            return LOADER_DECLINE;
        }
        let mut host = LIFECYCLE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if host.operations.is_some() {
            return LOADER_FAILURE;
        }
        let mut operations =
            ProductionOperations::new(ProviderAddresses::from_manifest(providers), module);
        let status = host.coordinator.load(&mut operations);
        if status == URP_AST_OK {
            host.operations = Some(operations);
            LOADER_OK
        } else if operations.phase != LoadPhase::Register {
            LOADER_DECLINE
        } else {
            LOADER_FAILURE
        }
    }))
    .unwrap_or(LOADER_FAILURE)
}

extern "C" fn loader_reload() -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        let mut host = LIFECYCLE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let LifecycleHost {
            coordinator,
            operations,
        } = &mut *host;
        let Some(operations) = operations.as_mut() else {
            return URP_AST_NOT_READY;
        };
        coordinator.reload(operations)
    }))
    .unwrap_or(URP_AST_ASTERISK_FAILURE)
}

extern "C" fn loader_unload() -> c_int {
    catch_unwind(AssertUnwindSafe(|| {
        let mut host = LIFECYCLE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let LifecycleHost {
            coordinator,
            operations,
        } = &mut *host;
        let Some(installed) = operations.as_mut() else {
            return URP_AST_OK;
        };
        let status = coordinator.unload(installed);
        if status == URP_AST_OK {
            *operations = None;
        }
        status
    }))
    .unwrap_or(URP_AST_ASTERISK_FAILURE)
}

static LOADER_DESCRIPTOR: AsteriskLoaderDescriptor = AsteriskLoaderDescriptor {
    struct_size: size_of::<AsteriskLoaderDescriptor>() as u32,
    abi_version: LOADER_ABI_VERSION,
    capability: LOADER_CAPABILITY.as_ptr(),
    load: Some(loader_load),
    reload: Some(loader_reload),
    unload: Some(loader_unload),
};

/// Return the immutable Rust-owned Asterisk lifecycle descriptor.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_asterisk_loader_descriptor() -> *const AsteriskLoaderDescriptor {
    ptr::from_ref(&LOADER_DESCRIPTOR)
}
