//! Native ABI projection onto the shared configuration, physical and update owners.

#[cfg(test)]
#[path = "native_remote_tests.rs"]
mod remote_tests;

use crate::{
    AbiHeader, Status, UrpAstChannelCommand, UrpAstDirectCallbacks, UrpAstProviderManifest,
    copy_abi, ffi_status, output_pointer, required_utf8, write_abi,
};
use std::ffi::{c_int, c_void};
use std::mem::{align_of, size_of};
use usbradioplus_audio::AudioProvider;
use usbradioplus_core::{
    ChannelConfiguration, ConfigDocument, GpioMode, HardwareInterfaceType, NativeDeviceSelection,
    ResolvedNativeConfiguration, ResolvedRemoteConfiguration,
};
use usbradioplus_driver::{HardwareMaintenance, HardwarePreflight, HardwareService, RemoteSetup};
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_gpio::GpioAdapter;
use usbradioplus_radio::{
    RadioProvider, ReceiveSignaling, SessionConfig, SubaudibleSource, TransmitSignaling,
};
use usbradioplus_rnnoise::DenoiseProvider;
use usbradioplus_runtime::{ExplicitNativeProcessingPlan, NativeProcessingFactory};
use usbradioplus_station::{
    HardwarePlan, NativeStationMedia, StationControlHost, StationPlan, StationRuntime,
    StationUpdate, StationUpdateError, apply_native_radio_tuning,
    apply_native_tuning_configuration, native_hardware_plan,
};

/// Current resolved native station request ABI.
pub const URP_NATIVE_CONFIG_ABI_VERSION: u32 = 4;
/// Select hardware capture gain at native startup.
pub const URP_NATIVE_HARDWARE_GAIN_INPUT: u32 = 1;
/// Select hardware output-A gain at native startup.
pub const URP_NATIVE_HARDWARE_GAIN_OUTPUT_A: u32 = 2;
/// Select hardware output-B gain at native startup.
pub const URP_NATIVE_HARDWARE_GAIN_OUTPUT_B: u32 = 4;

/// Resolved native station configuration. Byte spans and the released radio
/// configuration are borrowed only for the synchronous native-create call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpNativeStationConfig {
    /// Exact structure size.
    pub struct_size: u32,
    /// Native request ABI, currently four.
    pub abi_version: u32,
    /// Pointer to the released `rptadv_radio_session_config` ABI-5 template.
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
    /// Selected hardware gain bits; zero preserves every current control.
    pub hardware_gain_mask: u32,
    /// Hardware capture gain, finite -30 through 30 dB when selected.
    pub hardware_input_gain_db: f64,
    /// Hardware output-A gain, finite -30 through 30 dB when selected.
    pub hardware_output_a_gain_db: f64,
    /// Hardware output-B gain, finite -30 through 30 dB when selected.
    pub hardware_output_b_gain_db: f64,
    /// Original UTF-8 native configuration, including shared profiles and physical
    /// settings, not NUL terminated. Generated normalized aliases are not source.
    pub processing_configuration: *const u8,
    /// Processing-configuration byte count; zero preserves raw graphs.
    pub processing_configuration_length: u32,
    /// UTF-8 node used to select scoped/named processing profiles.
    pub processing_node: *const u8,
    /// Processing-node byte count.
    pub processing_node_length: u32,
    /// Apply pre-emphasis when a voice/telemetry profile is present, zero or one.
    pub transmit_preemphasis: u32,
    /// Transmit pre-emphasis corner, greater than zero through 500 Hz.
    pub transmit_preemphasis_corner_hz: f64,
    /// Receive de-emphasis corner, greater than zero through 500 Hz.
    pub receive_deemphasis_corner_hz: f64,
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
    /// Released radio, graph, audio and GPIO descriptors; RNNoise is needed only when enabled.
    pub providers: *const UrpAstProviderManifest,
    /// Exact-frame native callback pair, retained until synchronous destroy.
    pub callbacks: *const UrpAstDirectCallbacks,
    /// Nonzero lifecycle generation.
    pub generation_id: u64,
    /// Prepared controller frame bound; device callbacks remain at most 960 frames.
    pub maximum_frames: u32,
}

struct NativeStation {
    maintenance: HardwareMaintenance,
    runtime: StationRuntime,
    control: StationControlHost,
    factory: NativeProcessingFactory,
    radio_provider: RadioProvider,
    file_configuration: ChannelConfiguration,
    gains: [Option<f64>; 3],
    pending: Option<NativeUpdateConfiguration>,
    remote: Option<ResolvedRemoteConfiguration>,
    remote_dsp: Option<usbradioplus_station::RemoteDspControl>,
}

impl NativeStation {
    fn start(&mut self) -> Result<(), Status> {
        if self.runtime.running() && !self.runtime.started() {
            // Failed startup can leave callbacks active. Confirm stop before
            // hardware initialization, including configured gain reapplication.
            self.runtime.stop().map_err(|_| Status::SetupFailed)?;
        }
        if self.runtime.started() {
            self.maintenance.start_preserving_gains()
        } else {
            self.maintenance.start()
        }
        .map_err(|_| Status::SetupFailed)?;
        if self.runtime.start().is_err() {
            let _ = self.maintenance.stop();
            return Err(Status::SetupFailed);
        }
        Ok(())
    }

    fn stop(&mut self) -> Result<(), Status> {
        let audio = self.runtime.stop();
        let hardware = self.maintenance.stop();
        audio.map_err(|_| Status::SetupFailed)?;
        self.finish_stopped_update()?;
        hardware.map_err(|_| Status::SetupFailed)
    }

    fn poll_stop(&mut self) -> Result<bool, Status> {
        self.runtime.stop().map_err(|_| Status::SetupFailed)?;
        let done = self
            .maintenance
            .poll_stop()
            .map_err(|_| Status::SetupFailed)?;
        self.finish_stopped_update()?;
        Ok(done)
    }

    fn finish_stopped_update(&mut self) -> Result<(), Status> {
        if self.pending.is_some() {
            // Successful stop quiesces both owners. The common mailbox drains
            // any unacknowledged forward slot before restoring its reverse.
            self.runtime
                .finish_update(&mut self.control, false)
                .map_err(update_error)?;
            self.maintenance
                .finish_mixer_update(false)
                .map_err(|_| Status::SetupFailed)?;
            self.pending = None;
        }
        Ok(())
    }
}

pub(super) unsafe fn site_service<'a>(handle: *mut c_void) -> Result<&'a HardwareService, Status> {
    // SAFETY: the synchronous caller owns a live native station and serializes control.
    unsafe { handle.cast::<NativeStation>().as_ref() }
        .map(|station| station.maintenance.service())
        .ok_or(Status::InvalidArgument)
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
            let config = copy_native_config(args.config)?;
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
        // SAFETY: every configuration span remains readable through creation.
        let ResolvedRequest {
            mut configuration,
            hardware,
            processing,
            mut radio,
            gains,
            remote,
        } = unsafe { resolve_request(&config, args.generation_id, args.maximum_frames)? };
        if remote.is_some() && callbacks.metadata.is_none() {
            return Err(Status::IncompatibleAbi);
        }
        let file_configuration = configuration.clone();
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
        let denoise = if providers.rnnoise.is_null() {
            None
        } else {
            Some(
                // SAFETY: the provider descriptor remains loaded through processing teardown.
                unsafe { DenoiseProvider::from_raw_descriptor(providers.rnnoise) }
                    .map_err(|_| Status::IncompatibleAbi)?,
            )
        };
        // Admit selected dynamic providers before preflight can acquire hardware.
        // Keep only the original-control pump here; actual rig contexts open on
        // the existing physical service worker after its outputs are safe.
        let (remote_setup, remote_dsp) = remote
            .as_ref()
            .map(|remote| {
                RemoteSetup::prepare(
                    remote.clone(),
                    hardware.clone(),
                    configuration.station.receive.cos_assignment,
                    radio.qualification,
                )
            })
            .transpose()
            .map_err(|_| Status::SetupFailed)?
            .unzip();
        let (preflight, selected) = HardwarePreflight::native(
            &hardware,
            configuration.station.hardware.eeprom_enabled,
            audio,
            gpio,
            args.maximum_frames.min(960),
        )
        .map_err(|_| Status::SetupFailed)?;
        if preflight.apply_startup_tuning(&mut configuration) {
            apply_native_radio_tuning(&configuration, &mut radio);
        }
        let plan =
            StationPlan::native("native", configuration, hardware.clone(), radio, processing)
                .with_remote_configuration(remote.clone());
        let factory = NativeProcessingFactory::new_explicit(
            graph,
            denoise,
            crate::native_processing::agc_plugin_path(),
            args.maximum_frames,
        )
        .map_err(|_| Status::SetupFailed)?;
        let processing = factory
            .prepare(plan.processing())
            .map_err(|_| Status::SetupFailed)?;
        let media =
            // SAFETY: the caller retains callback contexts through synchronous destroy.
            unsafe { NativeStationMedia::prepare(radio, processing, radio_provider, callbacks) }
                .map_err(|_| Status::SetupFailed)?;
        let (runtime, control) = StationRuntime::open_native_planned(media, plan, &selected, audio)
            .map_err(|_| Status::SetupFailed)?;
        let mut maintenance = HardwareMaintenance::native(
            &hardware,
            preflight,
            selected,
            runtime.hardware_state(),
            audio,
            gpio,
            gains,
        )
        .map_err(|_| Status::SetupFailed)?;
        if let Some(setup) = remote_setup {
            maintenance
                .bind_remote(setup)
                .map_err(|_| Status::SetupFailed)?;
        }
        let station = Box::new(NativeStation {
            maintenance,
            runtime,
            control,
            factory,
            radio_provider,
            file_configuration,
            gains,
            pending: None,
            remote,
            remote_dsp,
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

pub(crate) unsafe extern "C" fn service(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        let station =
            // SAFETY: the host serializes this live station's non-audio control calls.
            unsafe { handle.cast::<NativeStation>().as_mut() }.ok_or(Status::InvalidArgument)?;
        if let Some(control) = &mut station.remote_dsp {
            control.pump(
                &mut station.runtime,
                &mut station.control,
                &station.factory,
                station.radio_provider,
            );
        }
        Ok(())
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

pub(crate) unsafe extern "C" fn quiesce(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the serialized caller retains this owner until retirement succeeds.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        if station.poll_stop()? {
            Ok(())
        } else {
            Err(Status::ReloadPending)
        }
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

/// Collect the portable record once for binary and formatted SDK consumers.
pub(crate) unsafe fn snapshot(
    handle: *mut c_void,
) -> Result<crate::maintenance::UrpNativePhysicalStatus, Status> {
    // SAFETY: callers serialize access to their still-owned native handle.
    let station = unsafe { handle.cast::<NativeStation>().as_mut() };
    let station = station.ok_or(Status::InvalidArgument)?;
    let diagnostics = station
        .maintenance
        .diagnostics(&station.runtime)
        .map_err(|_| Status::SetupFailed)?;
    let radio = station
        .control
        .radio()
        .radio()
        .snapshot()
        .map_err(|_| Status::SetupFailed)?;
    crate::maintenance::collect_physical_status(
        &mut station.maintenance,
        diagnostics,
        &radio,
        station.runtime.running(),
    )
    .map_err(|_| Status::SetupFailed)
}

pub(crate) unsafe extern "C" fn get_status(
    handle: *mut c_void,
    output: *mut crate::maintenance::UrpNativePhysicalStatus,
) -> c_int {
    ffi_status(|| {
        // SAFETY: native ownership and the initialized writable record are borrowed
        // synchronously by the serialized control executor.
        unsafe { write_abi(output, snapshot(handle)?) }
    })
}

pub(crate) unsafe extern "C" fn feedback_bind(
    handle: *mut c_void,
    port: *const crate::UrpRadioFeedbackPort,
) -> c_int {
    ffi_status(|| {
        // SAFETY: both records are lent by this existing serialized native owner.
        let port = unsafe { crate::feedback_port(port)? };
        // SAFETY: this control call has exclusive access to the live native handle.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        // SAFETY: the caller retains this borrowed port through synchronous shutdown.
        unsafe { station.runtime.bind_feedback(port) }.map_err(|_| Status::ChannelBusy)
    })
}

pub(crate) unsafe extern "C" fn programme_retire(
    handle: *mut c_void,
    expected_generation: u64,
    request: u64,
) -> c_int {
    ffi_status(|| {
        if request == 0 || expected_generation == 0 {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: caller retains the unique native control owner through this call.
        let station =
            // SAFETY: serialized caller retains this live unique native station.
            unsafe { handle.cast::<NativeStation>().as_mut() }.ok_or(Status::InvalidArgument)?;
        if !station.runtime.started() {
            return Err(Status::NotReady);
        }
        if station.pending.is_some()
            || (station
                .remote_dsp
                .as_ref()
                .is_some_and(usbradioplus_station::RemoteDspControl::is_busy)
                && !station.runtime.programme_retirement_pending())
        {
            return Err(Status::ChannelBusy);
        }
        station
            .runtime
            .retire_programme(
                &mut station.control,
                &station.factory,
                station.radio_provider,
                expected_generation,
                request,
            )
            .map_err(update_error)
    })
}

/// Forward the existing lock-free callback observations, without introducing
/// another processing counter or reading callback-owned graph state.
pub(crate) unsafe extern "C" fn processing_status_text(
    handle: *mut c_void,
    sink: Option<crate::UrpNativeProcessingDiagnostic>,
    context: *mut c_void,
) -> c_int {
    ffi_status(|| {
        let sink = sink.ok_or(Status::InvalidArgument)?;
        // SAFETY: the serialized caller retains this native handle and sink.
        let station = unsafe { handle.cast::<NativeStation>().as_ref() };
        let station = station.ok_or(Status::InvalidArgument)?;
        let observation = station.runtime.hardware_state().callback_statistics();
        let text = crate::maintenance::processing_status_text(
            observation,
            station.control.receive_loudness(),
            station.control.transmit_loudness(),
        );
        // SAFETY: the synchronous sink borrows this complete UTF-8 span only.
        unsafe { sink(context, text.as_ptr(), text.len() as u32) };
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn command(
    handle: *mut c_void,
    command: *mut UrpAstChannelCommand,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the caller retains its unique handle and initialized command.
        let (station, mut value) = unsafe {
            (
                handle
                    .cast::<NativeStation>()
                    .as_mut()
                    .ok_or(Status::InvalidArgument)?,
                copy_abi(command.cast_const())?,
            )
        };
        if !station.runtime.started() {
            return Err(Status::NotReady);
        }
        crate::maintenance::execute(
            &mut station.maintenance,
            station.control.plan().configuration(),
            &mut value,
        )
        .map_err(|error| error.status())?;
        // SAFETY: admission above validated this same writable command record.
        unsafe { write_abi(command, value) }
    })
}

pub(crate) unsafe extern "C" fn set_transmit(
    handle: *mut c_void,
    keyed: u32,
    ctcss_tenths_hz: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the serialized caller retains the native station through return.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        if !station.runtime.started() {
            return Err(Status::NotReady);
        }
        let keyed = crate::switch(i64::from(keyed))?;
        let tone = if ctcss_tenths_hz == 0 {
            None
        } else {
            Some(
                u16::try_from(ctcss_tenths_hz)
                    .ok()
                    .and_then(usbradioplus_asl3::CtcssTone::from_tenths_hz)
                    .ok_or(Status::InvalidArgument)?,
            )
        };
        station.maintenance.set_transmit_request(keyed, true, tone);
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn clear_manual(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: cleanup is serialized before the caller destroys its handle.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        station.maintenance.clear_manual_requests();
        Ok(())
    })
}

struct NativeUpdateConfiguration {
    file: ChannelConfiguration,
    gains: [Option<f64>; 3],
}

struct NativeUpdate {
    prepared: Option<StationUpdate>,
    configuration: NativeUpdateConfiguration,
    changed_mixers: [bool; 3],
}

fn update_error(error: StationUpdateError) -> Status {
    match error {
        StationUpdateError::Pending => Status::ReloadPending,
        StationUpdateError::Busy => Status::ChannelBusy,
        StationUpdateError::Incompatible => Status::InvalidConfiguration,
        _ => Status::SetupFailed,
    }
}

/// Prepare only immutable radio/processing state. The existing native owner
/// retains its providers and callbacks; request provider/callback fields are not
/// retained, reopened or invoked by update preparation.
pub(crate) unsafe extern "C" fn prepare_update(
    handle: *mut c_void,
    args: *const UrpNativeCreateArgs,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        // SAFETY: the caller retains a serialized native owner and the complete
        // synchronous request, including every borrowed configuration span.
        let (station, args, config) = unsafe {
            let station = handle
                .cast::<NativeStation>()
                .as_mut()
                .ok_or(Status::InvalidArgument)?;
            let args = copy_abi(args)?;
            (station, args, copy_native_config(args.config)?)
        };
        if !station.runtime.started() {
            return Err(Status::NotReady);
        }
        if args.generation_id == 0
            || args.maximum_frames != station.control.plan().radio().maximum_receive_frame_count
        {
            return Err(Status::InvalidArgument);
        }
        if station.pending.is_some() {
            return Err(Status::ChannelBusy);
        }
        // SAFETY: validated request storage remains borrowed through this call.
        let ResolvedRequest {
            mut configuration,
            hardware,
            processing,
            mut radio,
            gains,
            remote,
        } = unsafe { resolve_request(&config, args.generation_id, args.maximum_frames)? };
        if remote != station.remote {
            // Changed remote definitions require their physical-owner generation
            // handoff; an ordinary DSP update must not silently retain old policy.
            return Err(Status::InvalidConfiguration);
        }
        if configuration.station.hardware.eeprom_enabled
            != station
                .control
                .plan()
                .configuration()
                .station
                .hardware
                .eeprom_enabled
        {
            return Err(Status::InvalidConfiguration);
        }
        let file = configuration.clone();
        HardwareMaintenance::preserve_startup_tuning(
            &station.file_configuration,
            station.control.plan().configuration(),
            &mut configuration,
        );
        if configuration.station.receive.squelch_level != file.station.receive.squelch_level
            || configuration.station.ctcss.transmit_peak_dbfs
                != file.station.ctcss.transmit_peak_dbfs
        {
            apply_native_radio_tuning(&configuration, &mut radio);
        }
        let plan = StationPlan::native("native", configuration, hardware, radio, processing)
            .with_remote_configuration(remote);
        let prepared = station
            .runtime
            .snapshot_update(&station.control, &station.factory, station.radio_provider)
            .map_err(update_error)?
            .prepare(plan)
            .map_err(update_error)?;
        let update = Box::new(NativeUpdate {
            prepared: Some(prepared),
            configuration: NativeUpdateConfiguration { file, gains },
            changed_mixers: std::array::from_fn(|index| station.gains[index] != gains[index]),
        });
        // SAFETY: the admitted caller receives ownership of this one update token.
        unsafe { output.as_ptr().write(Box::into_raw(update).cast()) };
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn begin_update(handle: *mut c_void, update: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the caller serializes both still-owned opaque handles.
        let (station, update) = unsafe {
            (
                handle
                    .cast::<NativeStation>()
                    .as_mut()
                    .ok_or(Status::InvalidArgument)?,
                update
                    .cast::<NativeUpdate>()
                    .as_mut()
                    .ok_or(Status::InvalidArgument)?,
            )
        };
        if !station.runtime.started() {
            return Err(Status::NotReady);
        }
        if station.pending.is_some() {
            return Err(Status::ChannelBusy);
        }
        let prepared = update.prepared.take().ok_or(Status::InvalidArgument)?;
        let old = station.control.plan().configuration().clone();
        let next = prepared.plan().configuration().clone();
        station
            .runtime
            .begin_update(&station.control, prepared)
            .map_err(update_error)?;
        station.pending = Some(NativeUpdateConfiguration {
            file: update.configuration.file.clone(),
            gains: update.configuration.gains,
        });
        station
            .maintenance
            .begin_mixer_update(&old, &next, update.changed_mixers)
            .map_err(|_| Status::SetupFailed)
    })
}

pub(crate) unsafe extern "C" fn accept_update(handle: *mut c_void) -> c_int {
    ffi_status(|| {
        // SAFETY: the owning control executor serializes this live station handle.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        station
            .runtime
            .accept_update(&mut station.control)
            .map_err(update_error)
    })
}

pub(crate) unsafe extern "C" fn finish_update(handle: *mut c_void, commit: u32) -> c_int {
    ffi_status(|| {
        // SAFETY: ownership remains with the serialized caller until destruction.
        let station = unsafe { handle.cast::<NativeStation>().as_mut() };
        let station = station.ok_or(Status::InvalidArgument)?;
        let commit = crate::switch(i64::from(commit))?;
        station
            .runtime
            .finish_update(&mut station.control, commit)
            .map_err(update_error)?;
        station
            .maintenance
            .finish_mixer_update(commit)
            .map_err(|_| Status::SetupFailed)?;
        if let Some(configuration) = station.pending.take() {
            if commit {
                station.file_configuration = configuration.file;
                station.gains = configuration.gains;
                station.maintenance.adopt_native_gain_intents(
                    station.gains,
                    station.control.plan().configuration(),
                );
            }
        }
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn destroy_update(update: *mut c_void) {
    let _ = ffi_status(|| {
        if !update.is_null() {
            // SAFETY: the caller transfers this token exactly once. After begin,
            // its prepared owner lives in the runtime's acknowledged mailbox.
            drop(unsafe { Box::from_raw(update.cast::<NativeUpdate>()) });
        }
        Ok(())
    });
}

pub(super) unsafe fn text(pointer: *const u8, length: u32) -> Result<String, Status> {
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

struct ResolvedRequest {
    configuration: ChannelConfiguration,
    hardware: HardwarePlan,
    processing: ExplicitNativeProcessingPlan,
    radio: SessionConfig,
    gains: [Option<f64>; 3],
    remote: Option<ResolvedRemoteConfiguration>,
}

unsafe fn resolve_request(
    config: &UrpNativeStationConfig,
    generation_id: u64,
    maximum_frames: u32,
) -> Result<ResolvedRequest, Status> {
    // SAFETY: the request points to the complete released radio template.
    let mut radio = unsafe { SessionConfig::from_abi(config.radio) }
        .map_err(|_| Status::InvalidConfiguration)?;
    radio.generation_id = generation_id;
    radio.maximum_receive_frame_count = maximum_frames;
    radio.maximum_transmit_frame_count = maximum_frames;
    radio.publication_interval_milliseconds = 50;
    // SAFETY: configuration spans are retained through this synchronous creation.
    let profiles = unsafe {
        crate::native_processing::Profiles::resolve(
            &text(
                config.processing_configuration,
                config.processing_configuration_length,
            )?,
            &text(config.processing_node, config.processing_node_length)?,
        )?
    };
    // SAFETY: the synchronous request owns every advertised text span.
    let (mut configuration, hardware, mut processing, remote) = unsafe { resolve(config, &radio)? };
    profiles.apply(&mut radio, &mut processing);
    apply_native_tuning_configuration(&radio, &mut configuration);
    let gain_values = [
        config.hardware_input_gain_db,
        config.hardware_output_a_gain_db,
        config.hardware_output_b_gain_db,
    ];
    let gains = std::array::from_fn(|index| {
        (config.hardware_gain_mask & (1 << index) != 0).then_some(gain_values[index])
    });
    Ok(ResolvedRequest {
        configuration,
        hardware,
        processing,
        radio,
        gains,
        remote,
    })
}

/// Admit the sized ABI-3 request before reading shared-processing fields.
unsafe fn copy_native_config(
    raw: *const UrpNativeStationConfig,
) -> Result<UrpNativeStationConfig, Status> {
    if raw.is_null() || (raw as usize) % align_of::<UrpNativeStationConfig>() != 0 {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: the native caller supplies at least the common readable ABI header.
    let header = unsafe { raw.cast::<AbiHeader>().read() };
    if header.abi_version != URP_NATIVE_CONFIG_ABI_VERSION
        || header.struct_size < size_of::<UrpNativeStationConfig>() as u32
    {
        return Err(Status::IncompatibleAbi);
    }
    // SAFETY: admission requires the complete current request before this read.
    Ok(unsafe { raw.read() })
}

unsafe fn resolve(
    config: &UrpNativeStationConfig,
    radio: &SessionConfig,
) -> Result<
    (
        ChannelConfiguration,
        HardwarePlan,
        ExplicitNativeProcessingPlan,
        Option<ResolvedRemoteConfiguration>,
    ),
    Status,
> {
    if !matches!(config.input_device_channels, 1 | 2)
        || !matches!(config.output_device_channels, 1 | 2)
        || config.input_extra_buffer_ms > 500
        || config.output_extra_buffer_ms > 500
        || config.ptt_inverted > 1
        || config.receive_deemphasis > 1
        || config.transmit_preemphasis > 1
        || !config.transmit_preemphasis_corner_hz.is_finite()
        || config.transmit_preemphasis_corner_hz <= 0.0
        || config.transmit_preemphasis_corner_hz > 500.0
        || !config.receive_deemphasis_corner_hz.is_finite()
        || config.receive_deemphasis_corner_hz <= 0.0
        || config.receive_deemphasis_corner_hz > 500.0
        || !(-30..=30).contains(&config.receive_output_gain_db)
        || config.gpio_output_enable_mask > 255
        || config.gpio_output_initial_mask > 255
        || config.clip_led_mask > 255
        || config.clip_led_mask.count_ones() > 1
        || config.hardware_gain_mask & !7 != 0
        || [
            config.hardware_input_gain_db,
            config.hardware_output_a_gain_db,
            config.hardware_output_b_gain_db,
        ]
        .iter()
        .enumerate()
        .any(|(index, gain)| {
            config.hardware_gain_mask & (1 << index) != 0 && !(-30.0..=30.0).contains(gain)
        })
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
        0 => NativeDeviceSelection::Exact,
        1 => NativeDeviceSelection::AutomaticLowestAlsaCard,
        _ => return Err(Status::InvalidConfiguration),
    };
    if policy == NativeDeviceSelection::Exact && identifier.is_empty() && serial.is_empty() {
        return Err(Status::InvalidConfiguration);
    }
    // SAFETY: these source spans are borrowed only during this request.
    let (source, node) = unsafe {
        (
            text(
                config.processing_configuration,
                config.processing_configuration_length,
            )?,
            text(config.processing_node, config.processing_node_length)?,
        )
    };
    let node = if node.is_empty() {
        "native"
    } else {
        node.as_str()
    };
    let document = ConfigDocument::new(if source.is_empty() {
        format!("[{node}]\n")
    } else {
        source
    });
    let resolved = ResolvedNativeConfiguration::from_document(&document, "rpt_advanced.conf", node)
        .map_err(|_| Status::InvalidConfiguration)?;
    let mut physical = resolved.physical().clone();
    physical.device_selection = policy;
    physical.input_device_channels = config.input_device_channels;
    physical.output_device_channels = config.output_device_channels;
    let mut channel = resolved.into_config();
    let hardware = &mut channel.station.hardware;
    hardware.device_identifier = identifier;
    hardware.serial = serial;
    hardware.input_extra_buffer_ms = config.input_extra_buffer_ms;
    hardware.output_extra_buffer_ms = config.output_extra_buffer_ms;
    hardware.ptt_inverted = config.ptt_inverted != 0;
    hardware.interface_type = match config.cm119_profile {
        0 => HardwareInterfaceType::DudeUsb,
        1 => HardwareInterfaceType::SphUsb,
        2 => HardwareInterfaceType::Nhrc,
        3 => HardwareInterfaceType::Custom,
        _ => return Err(Status::InvalidConfiguration),
    };
    hardware.gpio_modes = std::array::from_fn(|index| {
        let bit = 1 << index;
        if config.gpio_output_enable_mask & bit == 0 {
            GpioMode::Input
        } else if config.gpio_output_initial_mask & bit != 0 {
            GpioMode::OutputHigh
        } else {
            GpioMode::OutputLow
        }
    });
    hardware.clip_led_gpio =
        (config.clip_led_mask != 0).then(|| config.clip_led_mask.trailing_zeros() as u8 + 1);
    for (index, target) in [
        &mut hardware.input_gain_db,
        &mut hardware.output_a_gain_db,
        &mut hardware.output_b_gain_db,
    ]
    .into_iter()
    .enumerate()
    {
        if config.hardware_gain_mask & (1 << index) != 0 {
            *target = [
                config.hardware_input_gain_db,
                config.hardware_output_a_gain_db,
                config.hardware_output_b_gain_db,
            ][index];
        }
    }
    let remote = ResolvedRemoteConfiguration::from_document(
        &document,
        "rpt_advanced.conf",
        node,
        &channel.station,
    )
    .map_err(|_| Status::InvalidConfiguration)?;
    let mut hardware = native_hardware_plan(&channel, &physical);
    if remote
        .as_ref()
        .is_some_and(|remote| remote.audio_source == usbradioplus_core::RemoteAudioSource::Alsa)
    {
        hardware.cm119 = None;
    }
    let receive_ctcss_mask = match radio.receive.signaling {
        ReceiveSignaling::Ctcss(ctcss)
            if radio.qualification.subaudible_source == SubaudibleSource::Dsp =>
        {
            ctcss.tones.bits()
        }
        _ => 0,
    };
    Ok((
        channel,
        hardware,
        ExplicitNativeProcessingPlan {
            local: None,
            voice_telemetry: None,
            receive_input_gain: radio.receive_input_gain,
            receive_graph,
            transmit_graph,
            receive_deemphasis: config.receive_deemphasis != 0,
            receive_deemphasis_corner_hz: config.receive_deemphasis_corner_hz,
            transmit_preemphasis: config.transmit_preemphasis != 0,
            transmit_preemphasis_corner_hz: config.transmit_preemphasis_corner_hz,
            receive_output_gain_db: config.receive_output_gain_db,
            receive_ctcss_mask,
            transmit_dcs: matches!(radio.transmit.signaling, TransmitSignaling::Dcs(_)),
        },
        remote,
    ))
}
