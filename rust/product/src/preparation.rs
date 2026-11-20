//! Sized, synchronous hardware-free controller preparation capability.

use crate::{
    Status, UrpNativeCreateArgs, UrpNativeProcessingDiagnostic, UrpNativeStationConfig, copy_abi,
    ffi_status, native, native_configuration, native_processing, output_pointer, required_utf8,
};
use std::ffi::{c_char, c_int, c_void};
use usbradioplus_core::{
    CarrierSource, HardwareOutputAssignment, NativeDeviceSelection, NativePhysicalOptions,
    ReceiveAudioSource,
};
use usbradioplus_radio::{OutputRoute, SessionConfig};
#[path = "preparation_radio.rs"]
pub(super) mod radio_metadata;

/// Typed radio/physical template plus the provider-owned settings DTO.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpTypedPreparation {
    /// Size in bytes, including this complete version-one prefix.
    pub struct_size: u32,
    /// One.
    pub abi_version: u32,
    /// Borrowed ABI-4 station template with an ABI-5 radio template.
    pub station: *const UrpNativeStationConfig,
    /// Provider DTO v1, UTF-8. See doc/typed-preparation.md.
    pub settings: *const u8,
    /// Positive length, up to one MiB.
    pub settings_length: u32,
}

/// Complete process-lifetime preparation operations. Every operation is required.
#[repr(C)]
pub struct UrpPreparationDescriptor {
    /// Complete descriptor size in bytes.
    pub struct_size: u32,
    /// One.
    pub abi_version: u32,
    /// NUL-terminated exact capability identity.
    pub capability_name: *const c_char,
    /// Resolve an owned candidate without device access.
    pub prepare: unsafe extern "C" fn(
        *const UrpTypedPreparation,
        Option<UrpNativeProcessingDiagnostic>,
        *mut c_void,
        *mut *mut c_void,
    ) -> c_int,
    /// Compare effective settings, ordered lists and explicit role presence.
    pub equal: unsafe extern "C" fn(*const c_void, *const c_void, *mut u32) -> c_int,
    /// Create a station with the existing callback/provider lifetime contract.
    pub create:
        unsafe extern "C" fn(*const c_void, *const UrpNativeCreateArgs, *mut *mut c_void) -> c_int,
    /// Prepare an update token for the existing transaction.
    pub prepare_update: unsafe extern "C" fn(
        *mut c_void,
        *const c_void,
        *const UrpNativeCreateArgs,
        *mut *mut c_void,
    ) -> c_int,
    /// Prepare an independent per-peer graph with the released graph provider.
    pub link_prepare:
        unsafe extern "C" fn(*const c_void, *const c_void, u32, u32, *mut *mut c_void) -> c_int,
}

// SAFETY: the immutable descriptor contains only process-lifetime code and one static string.
unsafe impl Sync for UrpPreparationDescriptor {}

static DESCRIPTOR: UrpPreparationDescriptor = UrpPreparationDescriptor {
    struct_size: size_of::<UrpPreparationDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"usbradioplus.preparation1".as_ptr(),
    prepare,
    equal,
    create,
    prepare_update,
    link_prepare,
};

pub(super) fn descriptor() -> &'static UrpPreparationDescriptor {
    &DESCRIPTOR
}

/// Return the independent immutable version-one preparation descriptor.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_preparation_descriptor() -> *const UrpPreparationDescriptor {
    descriptor()
}

unsafe extern "C" fn prepare(
    args: *const UrpTypedPreparation,
    diagnostic: Option<UrpNativeProcessingDiagnostic>,
    context: *mut c_void,
    out: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(out)?;
        let report = |field: &'static str| {
            if let Some(callback) = diagnostic {
                // SAFETY: the caller retains the control-plane callback/context for this call.
                unsafe {
                    callback(context, field.as_ptr(), field.len() as u32);
                }
            }
            Status::InvalidConfiguration
        };
        // SAFETY: the caller lends a readable common header and advertised bytes.
        let args = unsafe { copy_abi(args)? };
        // SAFETY: the borrowed station advertises its complete ABI-4 prefix.
        let mut template = unsafe { native::copy_native_config(args.station)? };
        if !(1..=1_048_576).contains(&args.settings_length) {
            return Err(report("settings"));
        }
        if template.device_identifier_length > 1_048_576 || template.usb_serial_length > 1_048_576 {
            return Err(report("device_identifier"));
        }
        if template.processing_configuration_length != 0 || template.processing_node_length != 0 {
            return Err(report("processing_configuration"));
        }
        if template.receive_graph_length != 0 || template.transmit_graph_length != 0 {
            return Err(report("graph"));
        }
        // SAFETY: caller supplies a complete readable span within the enforced size limit.
        let dto = unsafe { required_utf8(args.settings, args.settings_length) }
            .map_err(|_| report("settings"))?;
        let semantic = crate::preparation_settings::Settings::parse(dto)
            .map_err(report)?
            .resolve(template.receive_output_gain_db)
            .map_err(report)?;
        let mut channel = semantic.channel;
        let mut radio =
            // SAFETY: the bridge copies the sized released radio template before return.
            unsafe { SessionConfig::from_abi(template.radio) }.map_err(|_| report("radio"))?;
        let station = &mut channel.station;
        station.hardware.preemphasis_corner_hz = template.transmit_preemphasis_corner_hz;
        station.hardware.deemphasis_corner_hz = template.receive_deemphasis_corner_hz;
        station.hardware.output_a_assignment = route(radio.transmit.output_a.route);
        station.hardware.output_b_assignment = route(radio.transmit.output_b.route);
        station.transmit.preemphasis_enabled = template.transmit_preemphasis != 0;
        station.receive.audio_source = if radio.receive_input_gain == 0.0 {
            ReceiveAudioSource::Disabled
        } else if template.receive_deemphasis != 0 {
            ReceiveAudioSource::Flat
        } else {
            ReceiveAudioSource::Speaker
        };
        station.receive.cos_assignment = match radio.qualification.carrier_source {
            usbradioplus_radio::CarrierSource::Vox => return Err(report("carrier_source")),
            usbradioplus_radio::CarrierSource::DspNoise => CarrierSource::Dsp,
            usbradioplus_radio::CarrierSource::Disabled => CarrierSource::Disabled,
            usbradioplus_radio::CarrierSource::Open => CarrierSource::Open,
            usbradioplus_radio::CarrierSource::External => CarrierSource::External,
            usbradioplus_radio::CarrierSource::Usb => CarrierSource::Usb,
            usbradioplus_radio::CarrierSource::UsbInverted => CarrierSource::UsbInverted,
            usbradioplus_radio::CarrierSource::Parallel => CarrierSource::Parallel,
            usbradioplus_radio::CarrierSource::ParallelInverted => CarrierSource::ParallelInverted,
        };
        radio_metadata::apply(
            &mut channel,
            &mut radio,
            semantic.ctcss_supplied,
            semantic.dcs_supplied,
            semantic.profiles.local.is_some(),
        )
        .map_err(report)?;
        if channel.station.receive.vad_mode != usbradioplus_core::VadMode::Off
            && (channel.station.receive.cos_assignment == CarrierSource::Disabled
                || channel.station.receive.vad_mode == usbradioplus_core::VadMode::Kerchunk
                    && !channel.station.has_receive_release_gate())
        {
            return Err(report("vad_mode"));
        }
        channel
            .station
            .validate()
            .map_err(|_| report("signaling"))?;
        let physical = NativePhysicalOptions {
            device_selection: if template.device_selection == 0 {
                NativeDeviceSelection::Exact
            } else {
                NativeDeviceSelection::AutomaticLowestAlsaCard
            },
            input_device_channels: template.input_device_channels,
            output_device_channels: template.output_device_channels,
            hardware_input_gain_db: (template.hardware_gain_mask & 1 != 0)
                .then_some(template.hardware_input_gain_db),
            hardware_output_a_gain_db: (template.hardware_gain_mask & 2 != 0)
                .then_some(template.hardware_output_a_gain_db),
            hardware_output_b_gain_db: (template.hardware_gain_mask & 4 != 0)
                .then_some(template.hardware_output_b_gain_db),
            ..Default::default()
        };
        template.receive_graph = b"anull".as_ptr();
        template.receive_graph_length = 5;
        template.transmit_graph = b"anull".as_ptr();
        template.transmit_graph_length = 5;
        // SAFETY: the shared builder copies the entire borrowed template and semantic settings.
        let request = unsafe {
            native::build_request(
                &template,
                radio,
                channel,
                physical.clone(),
                semantic.profiles,
                semantic.remote,
            )
        }
        .map_err(|status| {
            if matches!(status, Status::InvalidConfiguration) {
                report("station")
            } else {
                status
            }
        })?;
        if let Some(cm119) = &request.hardware.cm119 {
            if u32::from(cm119.output_enable_mask) != template.gpio_output_enable_mask
                || u32::from(cm119.output_initial_mask) != template.gpio_output_initial_mask
                || u32::from(cm119.clip_led_mask) != template.clip_led_mask
            {
                return Err(report("gpio_masks"));
            }
        }
        if let Some(remote) = &request.remote {
            // The same admission as native create checks physical interlocks, RF policy
            // and the selected model. Only the activation worker opens the rig.
            usbradioplus_driver::RemoteSetup::prepare(
                remote.clone(),
                request.hardware.clone(),
                request.configuration.station.receive.cos_assignment,
                request.radio.qualification,
            )
            .map_err(|error| match error {
                usbradioplus_driver::RemoteSetupError::Provider(_) => Status::IncompatibleAbi,
                usbradioplus_driver::RemoteSetupError::Call(_) => report("remote"),
            })?;
        }
        let owner = Box::new(native_configuration::Resolved::typed(request, physical));
        // SAFETY: output is initialized and the product destroy operation owns the returned allocation.
        unsafe { output.as_ptr().write(Box::into_raw(owner).cast()) };
        Ok(())
    })
}

fn route(value: OutputRoute) -> HardwareOutputAssignment {
    match value {
        OutputRoute::Disabled => HardwareOutputAssignment::Off,
        OutputRoute::Voice => HardwareOutputAssignment::Voice,
        OutputRoute::Tone => HardwareOutputAssignment::Ctcss,
        OutputRoute::Composite => HardwareOutputAssignment::VoiceCtcss,
        OutputRoute::AuxiliaryVoice => HardwareOutputAssignment::AuxiliaryVoice,
    }
}

unsafe extern "C" fn equal(a: *const c_void, b: *const c_void, out: *mut u32) -> c_int {
    ffi_status(|| {
        if out.is_null() || !out.is_aligned() {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: both live immutable owners and the writable result are retained for this call.
        unsafe {
            out.write(u32::from(
                native_configuration::prepared(a)? == native_configuration::prepared(b)?,
            ));
        }
        Ok(())
    })
}

unsafe extern "C" fn create(
    owner: *const c_void,
    args: *const UrpNativeCreateArgs,
    out: *mut *mut c_void,
) -> c_int {
    // SAFETY: forward the caller's synchronous creation and prepared-owner lifetime.
    unsafe { native::create_from(args, out, native::RequestSource::Prepared(owner)) }
}

unsafe extern "C" fn prepare_update(
    station: *mut c_void,
    owner: *const c_void,
    args: *const UrpNativeCreateArgs,
    out: *mut *mut c_void,
) -> c_int {
    // SAFETY: station and candidate remain live throughout synchronous staging.
    unsafe {
        native::prepare_update_from(station, args, out, native::RequestSource::Prepared(owner))
    }
}

unsafe extern "C" fn link_prepare(
    owner: *const c_void,
    ffmpeg: *const c_void,
    sample_rate_hz: u32,
    frames: u32,
    out: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(out)?;
        // SAFETY: candidate survives return and provider survives peer destruction.
        unsafe {
            native_processing::prepare_link(
                native_configuration::prepared(owner)?.link.clone(),
                ffmpeg,
                sample_rate_hz,
                frames,
                output,
            )
        }
    })
}
