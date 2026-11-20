//! Owned shared native configuration inspection, before physical device access.

use crate::{
    Status, UrpNativeProcessingArgs, UrpNativeProcessingDiagnostic, UrpNativeStationConfig,
    copy_abi, ffi_status, native, output_pointer,
};
use std::ffi::{c_int, c_void};
use std::mem::size_of;
use usbradioplus_core::{
    ChannelConfiguration, ConfigDocument, NativeDeviceSelection, NativePhysicalOptions,
    ReceiveAudioSource, ResolvedNativeConfiguration, ResolvedRemoteConfiguration,
};

/// Controller-facing command settings copied from the shared remote resolver.
/// No rig, device or callback is opened by this inspection.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpRemoteCommandOptions {
    /// Exact complete record size.
    pub struct_size: u32,
    /// One.
    pub abi_version: u32,
    /// One only when this source enables a remote-base composition.
    pub enabled: u32,
    /// Zero disabled, one public administration, two source-specific unlock.
    pub access: u32,
    /// Number of prefix digits, excluding the leading star.
    pub prefix_length: u32,
    /// At most 63 ASCII digits; unused bytes are zero.
    pub prefix: [u8; 64],
}

pub(crate) unsafe extern "C" fn remote_options(
    configuration: *mut c_void,
    output: *mut UrpRemoteCommandOptions,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the caller serializes this live resolved configuration owner.
        let owner = unsafe { configuration.cast::<Resolved>().as_ref() };
        let owner = owner.ok_or(Status::InvalidArgument)?;
        let remote = owner.remote.as_ref();
        let mut options = UrpRemoteCommandOptions {
            struct_size: size_of::<UrpRemoteCommandOptions>() as u32,
            abi_version: 1,
            enabled: 0,
            access: 0,
            prefix_length: 0,
            prefix: [0; 64],
        };
        if let Some(remote) = remote {
            options.enabled = 1;
            options.access = match remote.dtmf_access {
                usbradioplus_core::RemoteDtmfAccess::Disabled => 0,
                usbradioplus_core::RemoteDtmfAccess::Open => 1,
                usbradioplus_core::RemoteDtmfAccess::Locked => 2,
            };
            let prefix = remote.dtmf_prefix.as_bytes();
            options.prefix_length = prefix.len() as u32;
            options.prefix[..prefix.len()].copy_from_slice(prefix);
        }
        // SAFETY: write_abi validates the caller's complete initialized output header.
        unsafe { crate::write_abi(output, options) }
    })
}
use usbradioplus_gpio::Cm119Profile;

/// A synchronous visitor of a borrowed, complete native configuration request.
pub type UrpNativeConfigurationSink =
    unsafe extern "C" fn(*mut c_void, *const UrpNativeStationConfig) -> c_int;

enum View {
    Ini {
        configuration: Box<ResolvedNativeConfiguration>,
        node: String,
    },
    Typed {
        physical: NativePhysicalOptions,
    },
}

pub(super) struct Resolved {
    view: View,
    remote: Option<ResolvedRemoteConfiguration>,
    // Legacy inspection permits an unbound exact selector and reports its
    // native admission failure only when a host requests a station owner.
    prepared: Result<native::ResolvedRequest, Status>,
}

impl Resolved {
    pub(super) fn typed(request: native::ResolvedRequest, physical: NativePhysicalOptions) -> Self {
        Self {
            remote: request.remote.clone(),
            view: View::Typed { physical },
            prepared: Ok(request),
        }
    }
}

/// Borrow a completed native plan on the synchronous control owner.
pub(super) unsafe fn prepared<'a>(
    owner: *const c_void,
) -> Result<&'a native::ResolvedRequest, Status> {
    // SAFETY: caller retains the live preparation owner through its synchronous operation.
    let owner = unsafe { owner.cast::<Resolved>().as_ref() }.ok_or(Status::InvalidArgument)?;
    owner.prepared.as_ref().map_err(|error| *error)
}

pub(super) fn validate_remote(remote: Option<&ResolvedRemoteConfiguration>) -> Result<(), Status> {
    if let Some(remote) = remote {
        let provider =
            usbradioplus_remote::RemoteProvider::load().map_err(|_| Status::IncompatibleAbi)?;
        usbradioplus_remote::RemoteSession::validate_policy(std::sync::Arc::new(provider), remote)
            .map_err(|_| Status::InvalidConfiguration)?;
    }
    Ok(())
}

pub(crate) unsafe extern "C" fn resolve(
    args: *const UrpNativeProcessingArgs,
    diagnostic: Option<UrpNativeProcessingDiagnostic>,
    diagnostic_context: *mut c_void,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the caller retains the advertised request and text until return.
        let args = unsafe { copy_abi(args)? };
        // SAFETY: all borrowed spans were supplied by this synchronous caller.
        let (text, node) = unsafe {
            (
                native::text(args.configuration, args.configuration_length)?,
                native::text(args.node, args.node_length)?,
            )
        };
        let configuration = ResolvedNativeConfiguration::from_document(
            &ConfigDocument::new(text),
            "rpt_advanced.conf",
            &node,
        )
        .map_err(|_| Status::InvalidConfiguration)?;
        let remote = usbradioplus_core::ResolvedRemoteConfiguration::from_document(
            configuration.source_document(),
            "rpt_advanced.conf",
            &node,
            &configuration.config().station,
        )
        .map_err(|_| Status::InvalidConfiguration)?;
        validate_remote(remote.as_ref())?;
        if let Some(diagnostic) = diagnostic {
            for warning in configuration.warnings() {
                let message = warning.to_string();
                // SAFETY: diagnostic storage is borrowed only during this callback.
                unsafe { diagnostic(diagnostic_context, message.as_ptr(), message.len() as u32) };
            }
        }
        let prepared = ini_template(&configuration, &node, |request| {
            let profiles = crate::native_processing::Profiles::selected(&configuration, &node);
            let mut channel = configuration.config().clone();
            // SAFETY: the station builder lends a complete ABI-5 radio for this callback.
            let mut radio = unsafe { usbradioplus_radio::SessionConfig::from_abi(request.radio) }
                .map_err(|_| Status::InvalidConfiguration)?;
            crate::preparation::radio_metadata::apply(
                &mut channel,
                &mut radio,
                true,
                true,
                profiles.local.is_some(),
            )
            .map_err(|_| Status::InvalidConfiguration)?;
            // SAFETY: the synchronous template lends its complete radio and physical spans.
            unsafe {
                native::build_request(
                    request,
                    radio,
                    channel,
                    configuration.physical().clone(),
                    profiles,
                    remote.clone(),
                )
            }
        });
        let resolved = Box::new(Resolved {
            view: View::Ini {
                configuration: Box::new(configuration),
                node,
            },
            remote,
            prepared,
        });
        // SAFETY: ownership transfers to native_configuration_destroy.
        unsafe { output.as_ptr().write(Box::into_raw(resolved).cast()) };
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn visit(
    configuration: *const c_void,
    sink: Option<UrpNativeConfigurationSink>,
    context: *mut c_void,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the serial caller retains one live resolved owner through return.
        let owner = unsafe { configuration.cast::<Resolved>().as_ref() };
        let owner = owner.ok_or(Status::InvalidArgument)?;
        let sink = sink.ok_or(Status::InvalidArgument)?;
        let call = |request: &UrpNativeStationConfig| {
            // SAFETY: the visitor borrows this request and all dependent spans until return.
            if unsafe { sink(context, request) } == 0 {
                Ok(())
            } else {
                Err(Status::SetupFailed)
            }
        };
        match &owner.view {
            View::Ini {
                configuration,
                node,
            } => ini_template(configuration, node, call),
            View::Typed { physical } => {
                let request = owner.prepared.as_ref().map_err(|e| *e)?;
                template(
                    &request.configuration,
                    physical,
                    &request.radio,
                    "",
                    "",
                    call,
                )
            }
        }
    })
}

fn ini_template<T>(
    resolved: &ResolvedNativeConfiguration,
    node: &str,
    visit: impl FnOnce(&UrpNativeStationConfig) -> Result<T, Status>,
) -> Result<T, Status> {
    let radio = usbradioplus_station::native_radio_session_config(resolved.config(), 1, 4096)
        .map_err(|_| Status::InvalidConfiguration)?;
    template(
        resolved.config(),
        resolved.physical(),
        &radio,
        resolved.source_document().as_str(),
        node,
        visit,
    )
}

fn template<T>(
    channel: &ChannelConfiguration,
    physical: &NativePhysicalOptions,
    radio: &usbradioplus_radio::SessionConfig,
    configuration: &str,
    node: &str,
    visit: impl FnOnce(&UrpNativeStationConfig) -> Result<T, Status>,
) -> Result<T, Status> {
    let hardware = usbradioplus_station::native_hardware_plan(channel, physical);
    // This ABI retains the configured CM119 profile even when a selected
    // remote composition later omits HID ownership for generic ALSA audio.
    let cm119 = hardware
        .cm119
        .as_ref()
        .ok_or(Status::InvalidConfiguration)?;
    radio.with_abi(|radio| {
        let hardware_config = &channel.station.hardware;
        let identifier = hardware
            .audio_selector
            .identifier
            .as_deref()
            .unwrap_or_default();
        let serial = hardware
            .audio_selector
            .serial
            .as_deref()
            .unwrap_or_default();
        let (gains, mask) = (
            [
                physical.hardware_input_gain_db,
                physical.hardware_output_a_gain_db,
                physical.hardware_output_b_gain_db,
            ],
            u32::from(physical.hardware_input_gain_db.is_some())
                | (u32::from(physical.hardware_output_a_gain_db.is_some()) << 1)
                | (u32::from(physical.hardware_output_b_gain_db.is_some()) << 2),
        );
        let request = UrpNativeStationConfig {
            struct_size: size_of::<UrpNativeStationConfig>() as u32,
            abi_version: crate::URP_NATIVE_CONFIG_ABI_VERSION,
            radio,
            device_selection: match physical.device_selection {
                NativeDeviceSelection::Exact => 0,
                NativeDeviceSelection::AutomaticLowestAlsaCard => 1,
            },
            device_identifier: identifier.as_ptr(),
            device_identifier_length: identifier.len() as u32,
            usb_serial: serial.as_ptr(),
            usb_serial_length: serial.len() as u32,
            input_device_channels: physical.input_device_channels,
            output_device_channels: physical.output_device_channels,
            input_extra_buffer_ms: hardware.input_extra_buffer_ms,
            output_extra_buffer_ms: hardware.output_extra_buffer_ms,
            cm119_profile: match cm119.profile {
                Cm119Profile::DudeUsb => 0,
                Cm119Profile::SphUsb => 1,
                Cm119Profile::Nhrc => 2,
                Cm119Profile::Custom => 3,
            },
            ptt_inverted: u32::from(hardware.ptt_inverted),
            gpio_output_enable_mask: u32::from(cm119.output_enable_mask),
            gpio_output_initial_mask: u32::from(cm119.output_initial_mask),
            clip_led_mask: u32::from(cm119.clip_led_mask),
            receive_graph: physical.receive_graph.as_ptr(),
            receive_graph_length: physical.receive_graph.len() as u32,
            transmit_graph: physical.transmit_graph.as_ptr(),
            transmit_graph_length: physical.transmit_graph.len() as u32,
            receive_deemphasis: u32::from(
                channel.station.receive.audio_source == ReceiveAudioSource::Flat,
            ),
            receive_output_gain_db: channel.local.output_gain_db as i32,
            hardware_gain_mask: mask,
            hardware_input_gain_db: gains[0].unwrap_or_default(),
            hardware_output_a_gain_db: gains[1].unwrap_or_default(),
            hardware_output_b_gain_db: gains[2].unwrap_or_default(),
            processing_configuration: configuration.as_ptr(),
            processing_configuration_length: configuration.len() as u32,
            processing_node: node.as_ptr(),
            processing_node_length: node.len() as u32,
            transmit_preemphasis: u32::from(channel.station.transmit.preemphasis_enabled),
            transmit_preemphasis_corner_hz: hardware_config.preemphasis_corner_hz,
            receive_deemphasis_corner_hz: hardware_config.deemphasis_corner_hz,
        };
        visit(&request)
    })
}

pub(crate) unsafe extern "C" fn destroy(configuration: *mut c_void) {
    if !configuration.is_null() {
        // SAFETY: the caller transfers the unique owner produced by resolve.
        drop(unsafe { Box::from_raw(configuration.cast::<Resolved>()) });
    }
}

#[cfg(test)]
#[path = "native_configuration_tests.rs"]
mod tests;
