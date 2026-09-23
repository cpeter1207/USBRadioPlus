//! Safe control-plane ownership around the PortAudio/ALSA adapter descriptor.
//!
//! The product radio session supplies separate bounded receive and transmit
//! workers. This crate owns descriptor validation, USB selection, stream
//! lifecycle, timing, statistics, and CM119 mixer handles without exposing
//! PortAudio or ALSA types to the adapter-neutral radio core.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::mem::{offset_of, size_of};
use std::ptr::NonNull;

const ABI_VERSION: u32 = 2;
const CAPABILITY: &CStr = c"rptadv.portaudio-alsa-audio";
const NATIVE_RATE_HZ: u32 = 48_000;
const DEVICE_PATH_CAPACITY: usize = 256;
const SERIAL_CAPACITY: usize = 256;
const MIXER_PATH_CAPACITY: usize = 2;
const MIXER_ELEMENT_CAPACITY: usize = 64;
const RESULT_OK: c_int = 0;

#[repr(C)]
struct OpaqueStream {
    _private: [u8; 0],
}

#[repr(C)]
struct OpaqueMixer {
    _private: [u8; 0],
}

/// Input-paced worker invoked on PortAudio's capture callback thread.
pub type ReceiveWorker = unsafe extern "C" fn(*mut c_void, *const f32, u32) -> i32;

/// DAC-paced worker invoked on PortAudio's playback callback thread.
pub type TransmitWorker = unsafe extern "C" fn(*mut c_void, *mut f32, u32) -> i32;

#[repr(C)]
struct RawStreamConfig {
    struct_size: u32,
    abi_version: u32,
    native_sample_rate_hz: u32,
    maximum_receive_frame_count: u32,
    maximum_transmit_frame_count: u32,
    input_device_index: i32,
    output_device_index: i32,
    input_device_channels: u32,
    output_device_channels: u32,
    receive_worker: Option<ReceiveWorker>,
    receive_worker_context: *mut c_void,
    transmit_worker: Option<TransmitWorker>,
    transmit_worker_context: *mut c_void,
    extra_output_buffer_milliseconds: u32,
    extra_input_buffer_milliseconds: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawStreamStatistics {
    struct_size: u32,
    abi_version: u32,
    callback_count: u64,
    callback_frame_count: u64,
    oversized_callback_count: u64,
    worker_failure_count: u64,
    input_overflow_count: u64,
    output_underflow_count: u64,
    device_error_count: u64,
    input_clip_sample_count: u64,
    output_clip_sample_count: u64,
    input_peak: f32,
    input_rms: f32,
    output_peak: f32,
    output_rms: f32,
    last_portaudio_error: i32,
    callback_last_duration_ns: u64,
    callback_max_duration_ns: u64,
    callback_last_start_delay_ns: u64,
    callback_max_start_delay_ns: u64,
    callback_late_start_count: u64,
    callback_late_start_tolerance_ns: u64,
    last_input_xrun_monotonic_ns: u64,
    last_output_xrun_monotonic_ns: u64,
    callback_clock_error_count: u64,
    capture_callback_count: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawStreamTiming {
    struct_size: u32,
    abi_version: u32,
    input_latency_seconds: f64,
    output_latency_seconds: f64,
    sample_rate_hz: f64,
}

#[repr(C)]
struct RawMixerConfig {
    struct_size: u32,
    card: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct RawUsbMixerConfig {
    struct_size: u32,
    usb_interface_path: *const c_char,
    element: *const c_char,
    element_index: u32,
    channel: u32,
    direction: u32,
}

#[repr(C)]
struct RawDeviceIdentity {
    struct_size: u32,
    usb_interface_path: *const c_char,
    usb_serial: *const c_char,
    input_device_channels: u32,
    output_device_channels: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawDeviceSelection {
    struct_size: u32,
    abi_version: u32,
    alsa_card_index: u32,
    input_device_index: i32,
    output_device_index: i32,
}

#[repr(C)]
struct RawDeviceSelector {
    struct_size: u32,
    selection_policy: u32,
    device_identifier: *const c_char,
    usb_serial: *const c_char,
    input_device_channels: u32,
    output_device_channels: u32,
}

#[repr(C)]
struct RawDeviceMatch {
    struct_size: u32,
    abi_version: u32,
    usb_interface_path: [c_char; DEVICE_PATH_CAPACITY],
    usb_serial: [c_char; SERIAL_CAPACITY],
    selection: RawDeviceSelection,
}

impl Default for RawDeviceMatch {
    fn default() -> Self {
        Self {
            struct_size: 0,
            abi_version: 0,
            usb_interface_path: [0; DEVICE_PATH_CAPACITY],
            usb_serial: [0; SERIAL_CAPACITY],
            selection: RawDeviceSelection::default(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawMixerPath {
    element: [c_char; MIXER_ELEMENT_CAPACITY],
    element_index: u32,
    channel: u32,
    direction: u32,
    capabilities: u32,
}

impl Default for RawMixerPath {
    fn default() -> Self {
        Self {
            element: [0; MIXER_ELEMENT_CAPACITY],
            element_index: 0,
            channel: 0,
            direction: 0,
            capabilities: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawMixerPaths {
    struct_size: u32,
    abi_version: u32,
    rx_capture_path_count: u32,
    tx_playback_path_count: u32,
    sidetone_path_count: u32,
    rx_compatibility_switch_path_count: u32,
    rx_capture_paths: [RawMixerPath; MIXER_PATH_CAPACITY],
    tx_playback_paths: [RawMixerPath; MIXER_PATH_CAPACITY],
    sidetone_paths: [RawMixerPath; MIXER_PATH_CAPACITY],
    rx_compatibility_switch_paths: [RawMixerPath; MIXER_PATH_CAPACITY],
}

type StreamCreate = unsafe extern "C" fn(*const RawStreamConfig, *mut *mut OpaqueStream) -> c_int;
type StreamControl = unsafe extern "C" fn(*mut OpaqueStream) -> c_int;
type StreamGetStatistics =
    unsafe extern "C" fn(*const OpaqueStream, *mut RawStreamStatistics) -> c_int;
type StreamDestroy = unsafe extern "C" fn(*mut OpaqueStream);
type MixerCreate = unsafe extern "C" fn(*const RawMixerConfig, *mut *mut OpaqueMixer) -> c_int;
type MixerRange = unsafe extern "C" fn(*const OpaqueMixer, *mut i64, *mut i64) -> c_int;
type MixerGetI64 = unsafe extern "C" fn(*const OpaqueMixer, *mut i64) -> c_int;
type MixerSetI64 = unsafe extern "C" fn(*mut OpaqueMixer, i64) -> c_int;
type MixerDestroy = unsafe extern "C" fn(*mut OpaqueMixer);
type UsbMixerCreate =
    unsafe extern "C" fn(*const RawUsbMixerConfig, *mut *mut OpaqueMixer) -> c_int;
type MixerGetU32 = unsafe extern "C" fn(*const OpaqueMixer, *mut u32) -> c_int;
type MixerSetU32 = unsafe extern "C" fn(*mut OpaqueMixer, u32) -> c_int;
type DeviceResolve =
    unsafe extern "C" fn(*const RawDeviceIdentity, *mut RawDeviceSelection) -> c_int;
type DeviceSelect = unsafe extern "C" fn(*const RawDeviceSelector, *mut RawDeviceMatch) -> c_int;
type StreamGetTiming = unsafe extern "C" fn(*const OpaqueStream, *mut RawStreamTiming) -> c_int;
type MixerPathsResolve = unsafe extern "C" fn(*const c_char, *mut RawMixerPaths) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct Descriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    stream_create: Option<StreamCreate>,
    stream_start: Option<StreamControl>,
    stream_stop: Option<StreamControl>,
    stream_get_stats: Option<StreamGetStatistics>,
    stream_destroy: Option<StreamDestroy>,
    mixer_create: Option<MixerCreate>,
    mixer_get_range_centibels: Option<MixerRange>,
    mixer_get_centibels: Option<MixerGetI64>,
    mixer_set_centibels: Option<MixerSetI64>,
    mixer_destroy: Option<MixerDestroy>,
    mixer_create_for_usb_interface: Option<UsbMixerCreate>,
    mixer_get_range_steps: Option<MixerRange>,
    mixer_get_steps: Option<MixerGetI64>,
    mixer_set_steps: Option<MixerSetI64>,
    mixer_get_normalized: Option<MixerGetU32>,
    mixer_set_normalized: Option<MixerSetU32>,
    mixer_get_switch: Option<MixerGetU32>,
    mixer_set_switch: Option<MixerSetU32>,
    usb_device_resolve: Option<DeviceResolve>,
    usb_device_select: Option<DeviceSelect>,
    stream_get_timing: Option<StreamGetTiming>,
    cm119_mixer_paths_resolve: Option<MixerPathsResolve>,
}

const REQUIRED_DESCRIPTOR_SIZE: usize =
    offset_of!(Descriptor, cm119_mixer_paths_resolve) + size_of::<Option<MixerPathsResolve>>();

/// Audio-adapter setup or control failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AudioError {
    /// Product composition supplied an incompatible descriptor.
    IncompatibleAdapter,
    /// An argument or returned adapter value was invalid.
    InvalidArgument,
    /// Control-plane allocation failed.
    NoMemory,
    /// PortAudio rejected a stream operation.
    PortAudio,
    /// ALSA rejected a mixer operation.
    Alsa,
    /// The requested device or mixer path is unsupported.
    Unsupported,
    /// Another stream already owns the physical device.
    DeviceBusy,
    /// The adapter returned an undocumented result.
    AdapterFailure,
}

impl fmt::Display for AudioError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::IncompatibleAdapter => "incompatible PortAudio/ALSA adapter",
            Self::InvalidArgument => "invalid audio-adapter argument",
            Self::NoMemory => "audio-adapter allocation failed",
            Self::PortAudio => "PortAudio operation failed",
            Self::Alsa => "ALSA mixer operation failed",
            Self::Unsupported => "unsupported audio device or mixer path",
            Self::DeviceBusy => "audio device is already in use",
            Self::AdapterFailure => "audio adapter returned an unknown failure",
        };
        formatter.write_str(message)
    }
}

impl std::error::Error for AudioError {}

/// Physical channel count accepted by the current CM119 adapter.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ChannelCount {
    /// One physical channel, duplicated or averaged at the canonical boundary.
    Mono = 1,
    /// Two physical channels.
    Stereo = 2,
}

/// Policy for selecting one stable USB audio interface.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SelectionPolicy {
    /// Select the exact configured identifier and/or serial number.
    Exact = 0,
    /// Select the usable USB audio card with the lowest ALSA index.
    Automatic = 1,
}

/// Typed request for one USB audio device.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceSelector {
    /// Selection policy.
    pub policy: SelectionPolicy,
    /// Stable USB topology or native `hw:` identifier.
    pub identifier: Option<String>,
    /// Exact USB serial number.
    pub serial: Option<String>,
    /// Required physical input channels.
    pub input_channels: ChannelCount,
    /// Required physical output channels.
    pub output_channels: ChannelCount,
}

/// Exact PortAudio/ALSA device resolved from a stable USB identity.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SelectedDevice {
    /// Canonical Linux USB interface path.
    pub interface_path: String,
    /// USB serial number when the device reports one.
    pub serial: Option<String>,
    /// Resolved ALSA card index.
    pub alsa_card_index: u32,
    /// Exact PortAudio input device index.
    pub input_device_index: i32,
    /// Exact PortAudio output device index.
    pub output_device_index: i32,
    /// Physical input channel count used during selection.
    pub input_channels: ChannelCount,
    /// Physical output channel count used during selection.
    pub output_channels: ChannelCount,
}

/// Immutable setup for the fixed-48-kHz PortAudio stream.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StreamConfig {
    /// Largest capture block preallocated by the receive worker.
    pub maximum_receive_frame_count: u32,
    /// Largest playback block preallocated by the transmit worker.
    pub maximum_transmit_frame_count: u32,
    /// Exact selected input-device index.
    pub input_device_index: i32,
    /// Exact selected output-device index.
    pub output_device_index: i32,
    /// Selected physical input channel count.
    pub input_channels: ChannelCount,
    /// Selected physical output channel count.
    pub output_channels: ChannelCount,
    /// Additional PortAudio capture-buffer request in milliseconds (0-500).
    pub extra_input_buffer_milliseconds: u32,
    /// Additional PortAudio playback-buffer request in milliseconds (0-500).
    pub extra_output_buffer_milliseconds: u32,
}

/// Borrowed receive-worker endpoint retained until its stream is destroyed.
pub struct ReceiveWorkerEndpoint<'a> {
    function: ReceiveWorker,
    context: NonNull<c_void>,
    _lifetime: PhantomData<&'a mut c_void>,
}

impl ReceiveWorkerEndpoint<'_> {
    /// Bind a live receive context to its C-compatible entry point.
    ///
    /// # Safety
    ///
    /// `context` must remain valid until this endpoint and any stream created
    /// from it are dropped. `function` must accept that context and consume
    /// every supplied frame without allocating, locking, blocking, logging, or
    /// panicking. Its context must be disjoint from, or safe to access
    /// concurrently with, the stream's transmit context.
    pub unsafe fn from_raw(function: ReceiveWorker, context: NonNull<c_void>) -> Self {
        Self {
            function,
            context,
            _lifetime: PhantomData,
        }
    }
}

/// Borrowed transmit-worker endpoint retained until its stream is destroyed.
pub struct TransmitWorkerEndpoint<'a> {
    function: TransmitWorker,
    context: NonNull<c_void>,
    _lifetime: PhantomData<&'a mut c_void>,
}

impl TransmitWorkerEndpoint<'_> {
    /// Bind a live transmit context to its C-compatible entry point.
    ///
    /// # Safety
    ///
    /// `context` must remain valid until this endpoint and any stream created
    /// from it are dropped. `function` must accept that context, completely
    /// initialize every supplied output frame, and never allocate, lock, block,
    /// log, or panic. Its context must be disjoint from, or safe to access
    /// concurrently with, the stream's receive context.
    pub unsafe fn from_raw(function: TransmitWorker, context: NonNull<c_void>) -> Self {
        Self {
            function,
            context,
            _lifetime: PhantomData,
        }
    }
}

/// Lock-free, best-effort stream statistics snapshot.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StreamStatistics {
    /// PortAudio playback callbacks processed.
    pub callback_count: u64,
    /// Physical playback frames processed.
    pub callback_frame_count: u64,
    /// Host blocks split at the configured maximum.
    pub oversized_callback_count: u64,
    /// Receive- or transmit-worker calls that failed.
    pub worker_failure_count: u64,
    /// PortAudio input-overflow notifications.
    pub input_overflow_count: u64,
    /// PortAudio output-underflow notifications.
    pub output_underflow_count: u64,
    /// Control-plane device errors.
    pub device_error_count: u64,
    /// Raw input samples at or beyond full scale.
    pub input_clip_sample_count: u64,
    /// Output samples at or beyond full scale.
    pub output_clip_sample_count: u64,
    /// Maximum absolute raw input value.
    pub input_peak: f32,
    /// Raw input RMS.
    pub input_rms: f32,
    /// Maximum absolute output value.
    pub output_peak: f32,
    /// Output RMS.
    pub output_rms: f32,
    /// Last PortAudio control-plane error.
    pub last_portaudio_error: i32,
    /// Most recent callback duration in nanoseconds.
    pub callback_last_duration_ns: u64,
    /// Maximum callback duration in nanoseconds.
    pub callback_max_duration_ns: u64,
    /// Most recent callback start-gap excess in nanoseconds.
    pub callback_last_start_delay_ns: u64,
    /// Maximum callback start-gap excess in nanoseconds.
    pub callback_max_start_delay_ns: u64,
    /// Late callback starts.
    pub callback_late_start_count: u64,
    /// Late-start counting tolerance in nanoseconds.
    pub callback_late_start_tolerance_ns: u64,
    /// Latest input xrun monotonic timestamp in nanoseconds.
    pub last_input_xrun_monotonic_ns: u64,
    /// Latest output xrun monotonic timestamp in nanoseconds.
    pub last_output_xrun_monotonic_ns: u64,
    /// Callback monotonic-clock read failures.
    pub callback_clock_error_count: u64,
    /// Capture callbacks processed.
    pub capture_callback_count: u64,
}

/// Immutable latency and rate estimates for an open stream.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StreamTiming {
    /// PortAudio input-latency estimate in seconds.
    pub input_latency_seconds: f64,
    /// PortAudio output-latency estimate in seconds.
    pub output_latency_seconds: f64,
    /// Actual stream sample rate in hertz.
    pub sample_rate_hz: f64,
}

/// Physical ALSA mixer direction.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MixerDirection {
    /// ADC/capture path.
    Capture = 0,
    /// DAC/playback path.
    Playback = 1,
}

/// Physical ALSA mixer channel.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MixerChannel {
    /// First physical channel.
    Left = 0,
    /// Second physical channel.
    Right = 1,
}

/// One discovered semantic CM119 mixer path.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MixerPath {
    /// ALSA simple-mixer element name.
    pub element: String,
    /// ALSA element index.
    pub element_index: u32,
    /// Physical channel.
    pub channel: MixerChannel,
    /// Capture or playback direction.
    pub direction: MixerDirection,
    /// Whether native volume control is available.
    pub has_volume: bool,
    /// Whether an enable switch is available.
    pub has_switch: bool,
}

/// Mixer paths classified using the established CM119 semantics.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Cm119MixerPaths {
    /// ADC capture paths.
    pub receive: Vec<MixerPath>,
    /// DAC paths, TX A followed by TX B.
    pub transmit: Vec<MixerPath>,
    /// Microphone playback/sidetone paths.
    pub sidetone: Vec<MixerPath>,
    /// Optional receive compatibility switches.
    pub receive_compatibility_switch: Vec<MixerPath>,
}

/// Validated process-lifetime PortAudio/ALSA capability.
#[derive(Clone, Copy)]
pub struct AudioProvider {
    descriptor: &'static Descriptor,
}

impl AudioProvider {
    /// Validate a process-lifetime descriptor supplied by product composition.
    ///
    /// # Safety
    ///
    /// `raw_descriptor` must be null or point to immutable process-lifetime
    /// storage. Every advertised function must obey the adapter ABI.
    pub unsafe fn from_raw_descriptor(raw_descriptor: *const c_void) -> Result<Self, AudioError> {
        let descriptor = NonNull::new(raw_descriptor.cast_mut().cast::<Descriptor>())
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: The caller promises readable process-lifetime descriptor
        // storage; only the fixed prefix is inspected before its size is used.
        let descriptor = unsafe { descriptor.as_ref() };
        if (descriptor.struct_size as usize) < REQUIRED_DESCRIPTOR_SIZE
            || descriptor.abi_version != ABI_VERSION
            || descriptor.capability_name.is_null()
            || !descriptor_complete(descriptor)
        {
            return Err(AudioError::IncompatibleAdapter);
        }
        // SAFETY: The name is NUL terminated for the promised descriptor lifetime.
        if unsafe { CStr::from_ptr(descriptor.capability_name) } != CAPABILITY {
            return Err(AudioError::IncompatibleAdapter);
        }
        Ok(Self { descriptor })
    }

    /// Resolve one exact or automatic USB device selection.
    pub fn select_device(&self, selector: &DeviceSelector) -> Result<SelectedDevice, AudioError> {
        if selector.policy == SelectionPolicy::Exact
            && selector.identifier.is_none()
            && selector.serial.is_none()
        {
            return Err(AudioError::InvalidArgument);
        }
        if selector.policy == SelectionPolicy::Automatic
            && (selector.identifier.is_some() || selector.serial.is_some())
        {
            return Err(AudioError::InvalidArgument);
        }
        let identifier = optional_c_string(selector.identifier.as_deref())?;
        let serial = optional_c_string(selector.serial.as_deref())?;
        let raw = RawDeviceSelector {
            struct_size: size_of::<RawDeviceSelector>() as u32,
            selection_policy: selector.policy as u32,
            device_identifier: optional_ptr(&identifier),
            usb_serial: optional_ptr(&serial),
            input_device_channels: selector.input_channels as u32,
            output_device_channels: selector.output_channels as u32,
        };
        let mut selected = RawDeviceMatch {
            struct_size: size_of::<RawDeviceMatch>() as u32,
            ..RawDeviceMatch::default()
        };
        let select = self
            .descriptor
            .usb_device_select
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: Raw request and writable result remain live for this call.
        map_result(unsafe { select(&raw, &mut selected) })?;
        if selected.struct_size < size_of::<RawDeviceMatch>() as u32
            || selected.selection.struct_size < size_of::<RawDeviceSelection>() as u32
            || selected.selection.input_device_index < 0
            || selected.selection.output_device_index < 0
        {
            return Err(AudioError::AdapterFailure);
        }
        validate_returned_abi(selected.abi_version)?;
        validate_returned_abi(selected.selection.abi_version)?;
        let interface_path = fixed_c_string(&selected.usb_interface_path)?;
        if interface_path.is_empty() {
            return Err(AudioError::AdapterFailure);
        }
        let serial = fixed_c_string(&selected.usb_serial)?;
        if selector
            .serial
            .as_deref()
            .is_some_and(|requested| requested != serial)
        {
            return Err(AudioError::AdapterFailure);
        }
        Ok(SelectedDevice {
            interface_path,
            serial: (!serial.is_empty()).then_some(serial),
            alsa_card_index: selected.selection.alsa_card_index,
            input_device_index: selected.selection.input_device_index,
            output_device_index: selected.selection.output_device_index,
            input_channels: selector.input_channels,
            output_channels: selector.output_channels,
        })
    }

    /// Open one stopped fixed-48-kHz stream around live receive and transmit workers.
    pub fn open_stream<'receive, 'transmit>(
        self,
        config: StreamConfig,
        receive: ReceiveWorkerEndpoint<'receive>,
        transmit: TransmitWorkerEndpoint<'transmit>,
    ) -> Result<AudioStream<'receive, 'transmit>, AudioError> {
        if config.maximum_receive_frame_count == 0 || config.maximum_transmit_frame_count == 0 {
            return Err(AudioError::InvalidArgument);
        }
        let raw = RawStreamConfig {
            struct_size: size_of::<RawStreamConfig>() as u32,
            abi_version: ABI_VERSION,
            native_sample_rate_hz: NATIVE_RATE_HZ,
            maximum_receive_frame_count: config.maximum_receive_frame_count,
            maximum_transmit_frame_count: config.maximum_transmit_frame_count,
            input_device_index: config.input_device_index,
            output_device_index: config.output_device_index,
            input_device_channels: config.input_channels as u32,
            output_device_channels: config.output_channels as u32,
            receive_worker: Some(receive.function),
            receive_worker_context: receive.context.as_ptr(),
            transmit_worker: Some(transmit.function),
            transmit_worker_context: transmit.context.as_ptr(),
            extra_output_buffer_milliseconds: config.extra_output_buffer_milliseconds,
            extra_input_buffer_milliseconds: config.extra_input_buffer_milliseconds,
        };
        let create = self
            .descriptor
            .stream_create
            .ok_or(AudioError::IncompatibleAdapter)?;
        let mut handle = std::ptr::null_mut();
        // SAFETY: The synchronous setup call borrows a complete config and
        // stores only the endpoint fields whose lifetime is tied to the result.
        let result = unsafe { create(&raw, &mut handle) };
        let Some(handle) = NonNull::new(handle) else {
            return Err(error_from_result(result));
        };
        if result != RESULT_OK {
            if let Some(destroy) = self.descriptor.stream_destroy {
                // SAFETY: A non-null partial handle belongs to this descriptor.
                unsafe { destroy(handle.as_ptr()) };
            }
            return Err(error_from_result(result));
        }
        Ok(AudioStream {
            provider: self,
            handle,
            started: false,
            _workers: PhantomData,
        })
    }

    /// Discover the selected interface's semantic CM119 mixer paths.
    pub fn cm119_mixer_paths(&self, interface_path: &str) -> Result<Cm119MixerPaths, AudioError> {
        let interface_path =
            CString::new(interface_path).map_err(|_| AudioError::InvalidArgument)?;
        if interface_path.as_bytes().is_empty() {
            return Err(AudioError::InvalidArgument);
        }
        let mut raw = RawMixerPaths {
            struct_size: size_of::<RawMixerPaths>() as u32,
            ..RawMixerPaths::default()
        };
        let resolve = self
            .descriptor
            .cm119_mixer_paths_resolve
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: Both request and writable result remain live for this call.
        map_result(unsafe { resolve(interface_path.as_ptr(), &mut raw) })?;
        if raw.struct_size < size_of::<RawMixerPaths>() as u32
            || raw.rx_capture_path_count == 0
            || raw.tx_playback_path_count == 0
        {
            return Err(AudioError::AdapterFailure);
        }
        validate_returned_abi(raw.abi_version)?;
        Ok(Cm119MixerPaths {
            receive: convert_paths(
                &raw.rx_capture_paths,
                raw.rx_capture_path_count,
                MixerDirection::Capture,
                1,
            )?,
            transmit: convert_paths(
                &raw.tx_playback_paths,
                raw.tx_playback_path_count,
                MixerDirection::Playback,
                1,
            )?,
            sidetone: convert_paths(
                &raw.sidetone_paths,
                raw.sidetone_path_count,
                MixerDirection::Playback,
                1,
            )?,
            receive_compatibility_switch: convert_paths(
                &raw.rx_compatibility_switch_paths,
                raw.rx_compatibility_switch_path_count,
                MixerDirection::Playback,
                2,
            )?,
        })
    }

    /// Open one discovered mixer path for the selected USB interface.
    pub fn open_mixer(self, interface_path: &str, path: &MixerPath) -> Result<Mixer, AudioError> {
        let interface_path =
            CString::new(interface_path).map_err(|_| AudioError::InvalidArgument)?;
        let element =
            CString::new(path.element.as_str()).map_err(|_| AudioError::InvalidArgument)?;
        if interface_path.as_bytes().is_empty() || element.as_bytes().is_empty() {
            return Err(AudioError::InvalidArgument);
        }
        let raw = RawUsbMixerConfig {
            struct_size: size_of::<RawUsbMixerConfig>() as u32,
            usb_interface_path: interface_path.as_ptr(),
            element: element.as_ptr(),
            element_index: path.element_index,
            channel: path.channel as u32,
            direction: path.direction as u32,
        };
        let create = self
            .descriptor
            .mixer_create_for_usb_interface
            .ok_or(AudioError::IncompatibleAdapter)?;
        let mut handle = std::ptr::null_mut();
        // SAFETY: Raw request and handle destination remain live for this call.
        let result = unsafe { create(&raw, &mut handle) };
        let Some(handle) = NonNull::new(handle) else {
            return Err(error_from_result(result));
        };
        if result != RESULT_OK {
            if let Some(destroy) = self.descriptor.mixer_destroy {
                // SAFETY: A non-null partial handle belongs to this descriptor.
                unsafe { destroy(handle.as_ptr()) };
            }
            return Err(error_from_result(result));
        }
        Ok(Mixer {
            provider: self,
            handle,
        })
    }
}

/// Exclusively owned PortAudio stream and callback lifetime.
pub struct AudioStream<'receive, 'transmit> {
    provider: AudioProvider,
    handle: NonNull<OpaqueStream>,
    started: bool,
    _workers: PhantomData<(
        ReceiveWorkerEndpoint<'receive>,
        TransmitWorkerEndpoint<'transmit>,
    )>,
}

impl AudioStream<'_, '_> {
    /// Start callbacks at the adapter's highest supported priority.
    pub fn start(&mut self) -> Result<(), AudioError> {
        if self.started {
            return Ok(());
        }
        let start = self
            .provider
            .descriptor
            .stream_start
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: This object exclusively owns the live stream handle.
        map_result(unsafe { start(self.handle.as_ptr()) })?;
        self.started = true;
        Ok(())
    }

    /// Stop callbacks; repeated calls are harmless.
    pub fn stop(&mut self) -> Result<(), AudioError> {
        if !self.started {
            return Ok(());
        }
        let stop = self
            .provider
            .descriptor
            .stream_stop
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: This object exclusively serializes stream lifecycle calls.
        map_result(unsafe { stop(self.handle.as_ptr()) })?;
        self.started = false;
        Ok(())
    }

    /// Read the lock-free stream statistics snapshot.
    pub fn statistics(&self) -> Result<StreamStatistics, AudioError> {
        let mut raw = RawStreamStatistics {
            struct_size: size_of::<RawStreamStatistics>() as u32,
            ..RawStreamStatistics::default()
        };
        let get = self
            .provider
            .descriptor
            .stream_get_stats
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: Handle is live and raw is writable for this synchronous call.
        map_result(unsafe { get(self.handle.as_ptr(), &mut raw) })?;
        validate_returned_abi(raw.abi_version)?;
        Ok(StreamStatistics::from(raw))
    }

    /// Read immutable PortAudio timing for the open stream.
    pub fn timing(&self) -> Result<StreamTiming, AudioError> {
        let mut raw = RawStreamTiming {
            struct_size: size_of::<RawStreamTiming>() as u32,
            ..RawStreamTiming::default()
        };
        let get = self
            .provider
            .descriptor
            .stream_get_timing
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: Handle is live and raw is writable for this synchronous call.
        map_result(unsafe { get(self.handle.as_ptr(), &mut raw) })?;
        validate_returned_abi(raw.abi_version)?;
        Ok(StreamTiming {
            input_latency_seconds: raw.input_latency_seconds,
            output_latency_seconds: raw.output_latency_seconds,
            sample_rate_hz: raw.sample_rate_hz,
        })
    }
}

impl Drop for AudioStream<'_, '_> {
    fn drop(&mut self) {
        if self.started {
            if let Some(stop) = self.provider.descriptor.stream_stop {
                // SAFETY: Drop is the final exclusive stream owner.
                let _ = unsafe { stop(self.handle.as_ptr()) };
            }
        }
        if let Some(destroy) = self.provider.descriptor.stream_destroy {
            // SAFETY: The handle is destroyed exactly once after callbacks stop.
            unsafe { destroy(self.handle.as_ptr()) };
        }
    }
}

/// Exclusively owned ALSA mixer element.
pub struct Mixer {
    provider: AudioProvider,
    handle: NonNull<OpaqueMixer>,
}

// SAFETY: A mixer handle has exclusive ownership and the released adapter ABI
// gives it no thread affinity. Moving that sole owner to the non-real-time
// hardware service thread cannot introduce concurrent access.
unsafe impl Send for Mixer {}

impl Mixer {
    /// Set the established USB-radio 0-through-999 hardware level.
    ///
    /// The maximum ALSA step is intentionally reached only by a value of
    /// 1000, which is outside the accepted range. This preserves the current
    /// radio calibration scale rather than silently changing node levels.
    pub fn set_hardware_level(&mut self, value: u32) -> Result<(), AudioError> {
        if value > 999 {
            return Err(AudioError::InvalidArgument);
        }
        let range = self
            .provider
            .descriptor
            .mixer_get_range_steps
            .ok_or(AudioError::IncompatibleAdapter)?;
        let set = self
            .provider
            .descriptor
            .mixer_set_steps
            .ok_or(AudioError::IncompatibleAdapter)?;
        let (mut minimum, mut maximum) = (0_i64, 0_i64);
        // SAFETY: The handle is live and both range outputs are writable.
        map_result(unsafe { range(self.handle.as_ptr(), &mut minimum, &mut maximum) })?;
        if minimum > maximum || maximum < 0 {
            return Err(AudioError::AdapterFailure);
        }
        let value = i64::from(value);
        let steps = (maximum / 1000) * value + ((maximum % 1000) * value) / 1000;
        if steps < minimum {
            return Err(AudioError::AdapterFailure);
        }
        // SAFETY: This object exclusively serializes mixer mutation.
        map_result(unsafe { set(self.handle.as_ptr(), steps) })
    }

    /// Read the selected path on its normalized 0-through-999 scale.
    pub fn normalized(&self) -> Result<u32, AudioError> {
        let get = self
            .provider
            .descriptor
            .mixer_get_normalized
            .ok_or(AudioError::IncompatibleAdapter)?;
        let mut value = 0;
        // SAFETY: Handle is live and value is writable for this call.
        map_result(unsafe { get(self.handle.as_ptr(), &mut value) })?;
        if value <= 999 {
            Ok(value)
        } else {
            Err(AudioError::AdapterFailure)
        }
    }

    /// Set the selected path on its normalized 0-through-999 scale.
    pub fn set_normalized(&mut self, value: u32) -> Result<(), AudioError> {
        if value > 999 {
            return Err(AudioError::InvalidArgument);
        }
        let set = self
            .provider
            .descriptor
            .mixer_set_normalized
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: This object exclusively serializes mixer mutation.
        map_result(unsafe { set(self.handle.as_ptr(), value) })
    }

    /// Read whether the selected mixer path is enabled.
    pub fn enabled(&self) -> Result<bool, AudioError> {
        let get = self
            .provider
            .descriptor
            .mixer_get_switch
            .ok_or(AudioError::IncompatibleAdapter)?;
        let mut enabled = 0;
        // SAFETY: Handle is live and output is writable for this call.
        map_result(unsafe { get(self.handle.as_ptr(), &mut enabled) })?;
        match enabled {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(AudioError::AdapterFailure),
        }
    }

    /// Enable or disable the selected mixer path.
    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), AudioError> {
        let set = self
            .provider
            .descriptor
            .mixer_set_switch
            .ok_or(AudioError::IncompatibleAdapter)?;
        // SAFETY: This object exclusively serializes mixer mutation.
        map_result(unsafe { set(self.handle.as_ptr(), u32::from(enabled)) })
    }
}

impl Drop for Mixer {
    fn drop(&mut self) {
        if let Some(destroy) = self.provider.descriptor.mixer_destroy {
            // SAFETY: This object destroys its exclusively owned handle once.
            unsafe { destroy(self.handle.as_ptr()) };
        }
    }
}

impl From<RawStreamStatistics> for StreamStatistics {
    fn from(raw: RawStreamStatistics) -> Self {
        Self {
            callback_count: raw.callback_count,
            callback_frame_count: raw.callback_frame_count,
            oversized_callback_count: raw.oversized_callback_count,
            worker_failure_count: raw.worker_failure_count,
            input_overflow_count: raw.input_overflow_count,
            output_underflow_count: raw.output_underflow_count,
            device_error_count: raw.device_error_count,
            input_clip_sample_count: raw.input_clip_sample_count,
            output_clip_sample_count: raw.output_clip_sample_count,
            input_peak: raw.input_peak,
            input_rms: raw.input_rms,
            output_peak: raw.output_peak,
            output_rms: raw.output_rms,
            last_portaudio_error: raw.last_portaudio_error,
            callback_last_duration_ns: raw.callback_last_duration_ns,
            callback_max_duration_ns: raw.callback_max_duration_ns,
            callback_last_start_delay_ns: raw.callback_last_start_delay_ns,
            callback_max_start_delay_ns: raw.callback_max_start_delay_ns,
            callback_late_start_count: raw.callback_late_start_count,
            callback_late_start_tolerance_ns: raw.callback_late_start_tolerance_ns,
            last_input_xrun_monotonic_ns: raw.last_input_xrun_monotonic_ns,
            last_output_xrun_monotonic_ns: raw.last_output_xrun_monotonic_ns,
            callback_clock_error_count: raw.callback_clock_error_count,
            capture_callback_count: raw.capture_callback_count,
        }
    }
}

fn descriptor_complete(descriptor: &Descriptor) -> bool {
    descriptor.stream_create.is_some()
        && descriptor.stream_start.is_some()
        && descriptor.stream_stop.is_some()
        && descriptor.stream_get_stats.is_some()
        && descriptor.stream_destroy.is_some()
        && descriptor.mixer_destroy.is_some()
        && descriptor.mixer_create_for_usb_interface.is_some()
        && descriptor.mixer_get_range_steps.is_some()
        && descriptor.mixer_get_steps.is_some()
        && descriptor.mixer_set_steps.is_some()
        && descriptor.mixer_get_normalized.is_some()
        && descriptor.mixer_set_normalized.is_some()
        && descriptor.mixer_get_switch.is_some()
        && descriptor.mixer_set_switch.is_some()
        && descriptor.usb_device_resolve.is_some()
        && descriptor.usb_device_select.is_some()
        && descriptor.stream_get_timing.is_some()
        && descriptor.cm119_mixer_paths_resolve.is_some()
}

fn optional_c_string(value: Option<&str>) -> Result<Option<CString>, AudioError> {
    value
        .map(|value| CString::new(value).map_err(|_| AudioError::InvalidArgument))
        .transpose()
}

fn optional_ptr(value: &Option<CString>) -> *const c_char {
    value
        .as_ref()
        .map_or(std::ptr::null(), |value| value.as_ptr())
}

fn fixed_c_string<const N: usize>(bytes: &[c_char; N]) -> Result<String, AudioError> {
    let Some(nul) = bytes.iter().position(|value| *value == 0) else {
        return Err(AudioError::AdapterFailure);
    };
    let bytes: Vec<u8> = bytes[..nul].iter().map(|value| *value as u8).collect();
    String::from_utf8(bytes).map_err(|_| AudioError::AdapterFailure)
}

fn convert_paths(
    paths: &[RawMixerPath; MIXER_PATH_CAPACITY],
    count: u32,
    expected_direction: MixerDirection,
    required_capabilities: u32,
) -> Result<Vec<MixerPath>, AudioError> {
    if count > MIXER_PATH_CAPACITY as u32 {
        return Err(AudioError::AdapterFailure);
    }
    let count = count as usize;
    paths[..count]
        .iter()
        .map(|path| {
            let channel = match path.channel {
                0 => MixerChannel::Left,
                1 => MixerChannel::Right,
                _ => return Err(AudioError::AdapterFailure),
            };
            let direction = match path.direction {
                0 => MixerDirection::Capture,
                1 => MixerDirection::Playback,
                _ => return Err(AudioError::AdapterFailure),
            };
            let element = fixed_c_string(&path.element)?;
            if element.is_empty()
                || direction != expected_direction
                || path.capabilities & required_capabilities != required_capabilities
            {
                return Err(AudioError::AdapterFailure);
            }
            Ok(MixerPath {
                element,
                element_index: path.element_index,
                channel,
                direction,
                has_volume: path.capabilities & 1 != 0,
                has_switch: path.capabilities & 2 != 0,
            })
        })
        .collect()
}

fn validate_returned_abi(version: u32) -> Result<(), AudioError> {
    if version == ABI_VERSION {
        Ok(())
    } else {
        Err(AudioError::IncompatibleAdapter)
    }
}

fn map_result(result: c_int) -> Result<(), AudioError> {
    if result == RESULT_OK {
        Ok(())
    } else {
        Err(error_from_result(result))
    }
}

fn error_from_result(result: c_int) -> AudioError {
    match result {
        -1 => AudioError::InvalidArgument,
        -2 => AudioError::NoMemory,
        -3 => AudioError::PortAudio,
        -4 => AudioError::Alsa,
        -5 => AudioError::Unsupported,
        -6 => AudioError::DeviceBusy,
        _ => AudioError::AdapterFailure,
    }
}

// SAFETY: Provider storage is immutable and process-lifetime by contract.
unsafe impl Send for AudioProvider {}
// SAFETY: Provider storage contains only immutable function pointers.
unsafe impl Sync for AudioProvider {}

#[cfg(test)]
mod tests;
