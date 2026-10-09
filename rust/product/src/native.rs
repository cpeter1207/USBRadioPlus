//! Native product requests, independent of Asterisk delivery and configuration syntax.

use crate::{
    Status, UrpAstDirectCallbacks, UrpAstProviderManifest, copy_abi, ffi_status, output_pointer,
    required_utf8,
};
use std::ffi::{c_int, c_void};
use usbradioplus_audio::{AudioProvider, ChannelCount, DeviceSelector, SelectionPolicy};
use usbradioplus_driver::{HardwareService, select_native_hardware};
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_gpio::{Cm119Profile, GpioAdapter};
use usbradioplus_radio::{
    RadioProvider, ReceiveSignaling, SessionConfig, SubaudibleSource, TransmitSignaling,
};
use usbradioplus_runtime::{ExplicitNativeProcessingPlan, NativeProcessingFactory};
use usbradioplus_station::{
    Cm119Plan, HardwarePlan, NativeStationMedia, StationControl, StationRuntime,
};

/// Resolved native station configuration. Byte spans and the released radio
/// configuration are borrowed only for the synchronous native-create call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpNativeStationConfig {
    /// Exact structure size.
    pub struct_size: u32,
    /// Native request ABI, currently one.
    pub abi_version: u32,
    /// Pointer to the released `rptadv_radio_session_config` ABI-4 template.
    /// Effective settings are preserved except lifecycle generation, frame bounds
    /// and publication cadence; inactive signaling fields are normalized.
    pub radio: *const c_void,
    /// Released audio selection policy: exact zero or automatic one.
    pub device_selection: u32,
    /// Optional UTF-8 device identifier, not NUL terminated.
    pub device_identifier: *const u8,
    /// Device-identifier byte count.
    pub device_identifier_length: u32,
    /// Optional UTF-8 exact USB serial, not NUL terminated.
    pub usb_serial: *const u8,
    /// USB-serial byte count.
    pub usb_serial_length: u32,
    /// Physical capture channels, one or two.
    pub input_device_channels: u32,
    /// Physical playback channels, one or two.
    pub output_device_channels: u32,
    /// Additional capture buffering, zero through 500 milliseconds.
    pub input_extra_buffer_ms: u32,
    /// Additional playback buffering, zero through 500 milliseconds.
    pub output_extra_buffer_ms: u32,
    /// Released GPIO CM119 profile value.
    pub cm119_profile: u32,
    /// Dedicated PTT inversion, zero or one.
    pub ptt_inverted: u32,
    /// Ordinary GPIO output-enable bits, GPIO one in bit zero.
    pub gpio_output_enable_mask: u32,
    /// Ordinary GPIO initial-high bits.
    pub gpio_output_initial_mask: u32,
    /// Optional clipping-indicator GPIO bit, zero when disabled.
    pub clip_led_mask: u32,
    /// Explicit UTF-8 mono 48 kHz receive graph, not NUL terminated.
    pub receive_graph: *const u8,
    /// Receive-graph byte count.
    pub receive_graph_length: u32,
    /// Explicit UTF-8 mono 48 kHz transmit graph, not NUL terminated.
    pub transmit_graph: *const u8,
    /// Transmit-graph byte count.
    pub transmit_graph_length: u32,
    /// Enable the established 300 Hz receive deemphasis, zero or one.
    pub receive_deemphasis: u32,
    /// Receive output gain following filtering, in decibels.
    pub receive_output_gain_db: i32,
}

/// Synchronous stopped native station creation; all request data is copied.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpNativeCreateArgs {
    /// Exact structure size.
    pub struct_size: u32,
    /// Native request ABI, currently one.
    pub abi_version: u32,
    /// Resolved frontend configuration.
    pub config: *const UrpNativeStationConfig,
    /// Released radio, graph, audio and GPIO descriptors; other entries are unused.
    pub providers: *const UrpAstProviderManifest,
    /// Exact-frame native callback pair, retained until synchronous destroy.
    pub callbacks: *const UrpAstDirectCallbacks,
    /// Nonzero lifecycle generation.
    pub generation_id: u64,
    /// Prepared controller frame bound; device callbacks remain at most 960 frames.
    pub maximum_frames: u32,
}

struct NativeStation {
    service: HardwareService,
    runtime: StationRuntime,
    _control: StationControl,
}

impl NativeStation {
    fn start(&mut self) -> Result<(), Status> {
        self.service.start().map_err(|_| Status::SetupFailed)?;
        if self.runtime.start().is_err() {
            let _ = self.service.stop();
            return Err(Status::SetupFailed);
        }
        Ok(())
    }

    fn stop(&mut self) -> Result<(), Status> {
        let audio = self.runtime.stop();
        let hardware = self.service.stop();
        audio.map_err(|_| Status::SetupFailed)?;
        hardware.map_err(|_| Status::SetupFailed)
    }
}

impl Drop for NativeStation {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

pub(crate) unsafe extern "C" fn create(
    args: *const UrpNativeCreateArgs,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the caller supplies advertised ABI request bytes during this call.
        let (args, config, providers, callbacks) = unsafe {
            let args = copy_abi(args)?;
            let config = copy_abi(args.config)?;
            let providers = copy_abi(args.providers)?;
            if args.callbacks.is_null() {
                return Err(Status::InvalidArgument);
            }
            if args.callbacks.cast::<u32>().read_unaligned()
                != std::mem::size_of::<UrpAstDirectCallbacks>() as u32
            {
                return Err(Status::IncompatibleAbi);
            }
            let callbacks = args.callbacks.read_unaligned();
            (args, config, providers, callbacks)
        };
        if args.generation_id == 0
            || args.maximum_frames == 0
            || args.maximum_frames > 4096
            || !callbacks.is_valid_native()
        {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the request points to the complete released radio template.
        let mut radio = unsafe { SessionConfig::from_abi(config.radio) }
            .map_err(|_| Status::InvalidConfiguration)?;
        radio.generation_id = args.generation_id;
        radio.maximum_receive_frame_count = args.maximum_frames;
        radio.maximum_transmit_frame_count = args.maximum_frames;
        radio.publication_interval_milliseconds = 50;
        // SAFETY: the synchronous request owns every advertised text span.
        let (hardware, processing) = unsafe { resolve(&config, &radio)? };
        // SAFETY: providers and executable functions remain loaded through destroy.
        let (graph, radio_provider, audio, gpio) = unsafe {
            (
                GraphProvider::from_raw_descriptor(providers.ffmpeg)
                    .map_err(|_| Status::IncompatibleAbi)?,
                RadioProvider::from_raw_descriptor(providers.radio)
                    .map_err(|_| Status::IncompatibleAbi)?,
                AudioProvider::from_raw_descriptor(providers.audio)
                    .map_err(|_| Status::IncompatibleAbi)?,
                GpioAdapter::from_raw(providers.gpio).map_err(|_| Status::IncompatibleAbi)?,
            )
        };
        let processing =
            NativeProcessingFactory::prepare_explicit(graph, &processing, args.maximum_frames)
                .map_err(|_| Status::SetupFailed)?;
        let media =
            // SAFETY: the caller retains callback contexts through synchronous destroy.
            unsafe { NativeStationMedia::prepare(radio, processing, radio_provider, callbacks) }
                .map_err(|_| Status::SetupFailed)?;
        let (_, selected) =
            select_native_hardware(&hardware, audio, gpio, args.maximum_frames.min(960))
                .map_err(|_| Status::SetupFailed)?;
        let (runtime, control) = StationRuntime::open_native(media, &selected, audio)
            .map_err(|_| Status::SetupFailed)?;
        let service = HardwareService::native(&hardware, selected, runtime.hardware_state(), gpio);
        let station = Box::new(NativeStation {
            service,
            runtime,
            _control: control,
        });
        // SAFETY: output storage was validated and the caller receives sole ownership.
        unsafe {
            output.as_ptr().write(Box::into_raw(station).cast());
        }
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn start(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: caller supplies its uniquely owned native station handle.
        unsafe { handle.cast::<NativeStation>().as_mut() }
            .ok_or(Status::InvalidArgument)?
            .start()
    })
}

pub(crate) unsafe extern "C" fn stop(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: caller supplies its uniquely owned native station handle.
        unsafe { handle.cast::<NativeStation>().as_mut() }
            .ok_or(Status::InvalidArgument)?
            .stop()
    })
}

pub(crate) unsafe extern "C" fn destroy(handle: *mut c_void) {
    let _ = ffi_status(|| {
        if !handle.is_null() {
            // SAFETY: the caller transfers its sole native handle exactly once.
            drop(unsafe { Box::from_raw(handle.cast::<NativeStation>()) });
        }
        Ok(())
    });
}

unsafe fn text(pointer: *const u8, length: u32) -> Result<String, Status> {
    if length == 0 {
        return Ok(String::new());
    }
    // SAFETY: forwarded from the enclosing synchronous request contract.
    let value = unsafe { required_utf8(pointer, length)? };
    if value.contains('\0') {
        return Err(Status::InvalidConfiguration);
    }
    Ok(value.to_owned())
}

unsafe fn resolve(
    config: &UrpNativeStationConfig,
    radio: &SessionConfig,
) -> Result<(HardwarePlan, ExplicitNativeProcessingPlan), Status> {
    let channels = |value| match value {
        1 => Ok(ChannelCount::Mono),
        2 => Ok(ChannelCount::Stereo),
        _ => Err(Status::InvalidConfiguration),
    };
    if config.input_extra_buffer_ms > 500
        || config.output_extra_buffer_ms > 500
        || config.ptt_inverted > 1
        || config.receive_deemphasis > 1
        || !(-30..=30).contains(&config.receive_output_gain_db)
        || config.gpio_output_enable_mask > 255
        || config.gpio_output_initial_mask > 255
        || config.clip_led_mask > 255
        || config.clip_led_mask.count_ones() > 1
    {
        return Err(Status::InvalidConfiguration);
    }
    // SAFETY: forwarded from the synchronous request's readable byte-span contract.
    let (identifier, serial, receive_graph, transmit_graph) = unsafe {
        (
            text(config.device_identifier, config.device_identifier_length)?,
            text(config.usb_serial, config.usb_serial_length)?,
            text(config.receive_graph, config.receive_graph_length)?,
            text(config.transmit_graph, config.transmit_graph_length)?,
        )
    };
    if receive_graph.is_empty() || transmit_graph.is_empty() {
        return Err(Status::InvalidConfiguration);
    }
    let policy = match config.device_selection {
        0 => SelectionPolicy::Exact,
        1 => SelectionPolicy::Automatic,
        _ => return Err(Status::InvalidConfiguration),
    };
    if policy == SelectionPolicy::Exact && identifier.is_empty() && serial.is_empty() {
        return Err(Status::InvalidConfiguration);
    }
    let serial = (!serial.is_empty()).then_some(serial);
    let output_enable_mask = (config.gpio_output_enable_mask | config.clip_led_mask) as u8;
    let hardware = HardwarePlan {
        audio_selector: DeviceSelector {
            policy,
            identifier: (!identifier.is_empty()).then_some(identifier),
            serial: serial.clone(),
            input_channels: channels(config.input_device_channels)?,
            output_channels: channels(config.output_device_channels)?,
        },
        input_extra_buffer_ms: config.input_extra_buffer_ms,
        output_extra_buffer_ms: config.output_extra_buffer_ms,
        cm119: Cm119Plan {
            required_usb_port_path: None,
            required_serial: serial,
            profile: match config.cm119_profile {
                0 => Cm119Profile::DudeUsb,
                1 => Cm119Profile::SphUsb,
                2 => Cm119Profile::Nhrc,
                3 => Cm119Profile::Custom,
                _ => return Err(Status::InvalidConfiguration),
            },
            ptt_inverted: config.ptt_inverted != 0,
            output_enable_mask,
            output_initial_mask: config.gpio_output_initial_mask as u8,
            input_mask: !output_enable_mask,
            clip_led_mask: config.clip_led_mask as u8,
        },
        parallel: None,
    };
    let receive_ctcss_mask = match radio.receive.signaling {
        ReceiveSignaling::Ctcss(ctcss)
            if radio.qualification.subaudible_source == SubaudibleSource::Dsp =>
        {
            ctcss.tones.bits()
        }
        _ => 0,
    };
    Ok((
        hardware,
        ExplicitNativeProcessingPlan {
            receive_graph,
            transmit_graph,
            receive_deemphasis: config.receive_deemphasis != 0,
            receive_output_gain_db: config.receive_output_gain_db,
            receive_ctcss_mask,
            transmit_dcs: matches!(radio.transmit.signaling, TransmitSignaling::Dcs(_)),
        },
    ))
}
