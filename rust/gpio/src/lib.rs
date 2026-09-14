//! Safe ownership for the versioned CM119 and parallel-port GPIO adapter.
//!
//! The external adapter retains all USB HID and operating-system I/O. This
//! crate validates its descriptor, owns device handles, and exposes typed
//! control and snapshot operations. Only lock-free publication and snapshot
//! methods may be called from a native audio tick; [`Cm119Device::service`]
//! and [`ParallelDevice::service`] belong to one non-real-time service owner.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::fmt;
use std::mem::size_of;
use std::ptr::{self, NonNull};

const ABI_VERSION: u32 = 1;
const CAPABILITY: &CStr = c"rptadv.cm119-hid-gpio";
const RESULT_OK: c_int = 0;
const SERIAL_CAPACITY: usize = 128;
const DEVICE_CAPACITY: usize = 16;
/// Number of physical 16-bit words in one CM119 EEPROM image.
pub const EEPROM_WORD_COUNT: usize = 64;

#[repr(C)]
struct OpaqueCm119Device {
    _private: [u8; 0],
}

#[repr(C)]
struct OpaqueParallelDevice {
    _private: [u8; 0],
}

#[repr(C)]
struct RawDeviceConfig {
    struct_size: u32,
    abi_version: u32,
    usb_port_path: *const c_char,
    vendor_id: u16,
    product_id: u16,
    profile: u32,
    ptt_inverted: u32,
    gpio_output_enable_mask: u32,
    gpio_output_initial_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawDeviceInfo {
    struct_size: u32,
    abi_version: u32,
    present: u32,
    vendor_id: u16,
    product_id: u16,
    usb_bus: u32,
    usb_port_number_count: u32,
    usb_port_numbers: [u8; 7],
    serial: [c_char; SERIAL_CAPACITY],
}

impl Default for RawDeviceInfo {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
            present: 0,
            vendor_id: 0,
            product_id: 0,
            usb_bus: 0,
            usb_port_number_count: 0,
            usb_port_numbers: [0; 7],
            serial: [0; SERIAL_CAPACITY],
        }
    }
}

#[repr(C)]
struct RawDeviceList {
    struct_size: u32,
    abi_version: u32,
    matching_device_count: u32,
    returned_device_count: u32,
    devices: [RawDeviceInfo; DEVICE_CAPACITY],
}

impl Default for RawDeviceList {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
            matching_device_count: 0,
            returned_device_count: 0,
            devices: [RawDeviceInfo::default(); DEVICE_CAPACITY],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawEepromImage {
    struct_size: u32,
    abi_version: u32,
    checksum_valid: u32,
    magic_valid: u32,
    words: [u16; EEPROM_WORD_COUNT],
}

impl Default for RawEepromImage {
    fn default() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            abi_version: ABI_VERSION,
            checksum_valid: 0,
            magic_valid: 0,
            words: [0; EEPROM_WORD_COUNT],
        }
    }
}

#[repr(C)]
struct RawParallelConfig {
    struct_size: u32,
    abi_version: u32,
    transport: u32,
    ppdev_path: *const c_char,
    raw_io_base: u32,
    output_enable_mask: u32,
    output_initial_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawParallelInputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    status_mask: u32,
}

#[repr(C)]
struct RawParallelOutputs {
    struct_size: u32,
    abi_version: u32,
    output_mask: u32,
    pulse_mask: u32,
    pulse_duration_milliseconds: u32,
    cancel_pulse: u32,
}

#[repr(C)]
struct RawParallelScheduledPulse {
    struct_size: u32,
    abi_version: u32,
    invert_mask: u32,
    pulse_duration_milliseconds: u32,
    cancel_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawParallelStats {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    io_error_count: u64,
    online: u32,
    last_io_error: i32,
    applied_output_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawCm119Inputs {
    struct_size: u32,
    abi_version: u32,
    online: u32,
    cor_active: u32,
    ctcss_active: u32,
    gpio_input_mask: u32,
    hid_report: [u8; 4],
}

#[repr(C)]
struct RawCm119Outputs {
    struct_size: u32,
    abi_version: u32,
    ptt_asserted: u32,
    gpio_output_mask: u32,
}

#[repr(C)]
struct RawCm119ScheduledPulse {
    struct_size: u32,
    abi_version: u32,
    ptt_invert: u32,
    gpio_invert_mask: u32,
    pulse_duration_milliseconds: u32,
    ptt_cancel: u32,
    gpio_cancel_mask: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawCm119Stats {
    struct_size: u32,
    abi_version: u32,
    input_read_count: u64,
    output_apply_count: u64,
    usb_error_count: u64,
    ptt_applied: u32,
    online: u32,
    last_usb_error: i32,
    eeprom_read_count: u64,
    eeprom_write_count: u64,
}

type DeviceProbe = unsafe extern "C" fn(*const RawDeviceConfig, *mut RawDeviceInfo) -> c_int;
type DeviceOpen =
    unsafe extern "C" fn(*const RawDeviceConfig, *mut *mut OpaqueCm119Device) -> c_int;
type DevicePublish = unsafe extern "C" fn(*mut OpaqueCm119Device, *const RawCm119Outputs) -> c_int;
type DeviceControl = unsafe extern "C" fn(*mut OpaqueCm119Device) -> c_int;
type DeviceInputs = unsafe extern "C" fn(*const OpaqueCm119Device, *mut RawCm119Inputs) -> c_int;
type DeviceStats = unsafe extern "C" fn(*const OpaqueCm119Device, *mut RawCm119Stats) -> c_int;
type DeviceClose = unsafe extern "C" fn(*mut OpaqueCm119Device);
type DeviceDiscover = unsafe extern "C" fn(*mut RawDeviceList) -> c_int;
type DeviceEeprom = unsafe extern "C" fn(*mut OpaqueCm119Device, *mut RawEepromImage) -> c_int;

type ParallelOpen =
    unsafe extern "C" fn(*const RawParallelConfig, *mut *mut OpaqueParallelDevice) -> c_int;
type ParallelPublish =
    unsafe extern "C" fn(*mut OpaqueParallelDevice, *const RawParallelOutputs) -> c_int;
type ParallelControl = unsafe extern "C" fn(*mut OpaqueParallelDevice) -> c_int;
type ParallelWrite = unsafe extern "C" fn(*mut OpaqueParallelDevice, u32) -> c_int;
type ParallelGetInputs =
    unsafe extern "C" fn(*const OpaqueParallelDevice, *mut RawParallelInputs) -> c_int;
type ParallelStats =
    unsafe extern "C" fn(*const OpaqueParallelDevice, *mut RawParallelStats) -> c_int;
type ParallelClose = unsafe extern "C" fn(*mut OpaqueParallelDevice);
type Cm119ScheduledPulse =
    unsafe extern "C" fn(*mut OpaqueCm119Device, *const RawCm119ScheduledPulse) -> c_int;
type ParallelScheduledPulse =
    unsafe extern "C" fn(*mut OpaqueParallelDevice, *const RawParallelScheduledPulse) -> c_int;
type ParallelSetBinaryChannel = unsafe extern "C" fn(*mut OpaqueParallelDevice, u8) -> c_int;
type ParallelProgramRtx =
    unsafe extern "C" fn(*mut OpaqueParallelDevice, u32, u32, u32, u32) -> c_int;
type ParallelClearRtxTransmit = unsafe extern "C" fn(*mut OpaqueParallelDevice) -> c_int;

#[repr(C)]
#[derive(Clone, Copy)]
struct Descriptor {
    struct_size: u32,
    abi_version: u32,
    capability_name: *const c_char,
    device_probe: Option<DeviceProbe>,
    device_open: Option<DeviceOpen>,
    device_publish_outputs: Option<DevicePublish>,
    device_service: Option<DeviceControl>,
    device_get_inputs: Option<DeviceInputs>,
    device_get_stats: Option<DeviceStats>,
    device_close: Option<DeviceClose>,
    device_discover: Option<DeviceDiscover>,
    device_read_eeprom: Option<DeviceEeprom>,
    device_write_eeprom: Option<DeviceEeprom>,
    parallel_open: Option<ParallelOpen>,
    parallel_publish_outputs: Option<ParallelPublish>,
    parallel_service: Option<ParallelControl>,
    parallel_control_write_data: Option<ParallelWrite>,
    parallel_get_inputs: Option<ParallelGetInputs>,
    parallel_get_stats: Option<ParallelStats>,
    parallel_close: Option<ParallelClose>,
    device_publish_inverting_pulse: Option<unsafe extern "C" fn() -> c_int>,
    parallel_publish_inverting_pulse: Option<unsafe extern "C" fn() -> c_int>,
    device_schedule_inverting_pulse: Option<Cm119ScheduledPulse>,
    parallel_schedule_inverting_pulse: Option<ParallelScheduledPulse>,
    parallel_set_binary_channel: Option<ParallelSetBinaryChannel>,
    parallel_program_rtx: Option<ParallelProgramRtx>,
    parallel_clear_rtx_transmit: Option<ParallelClearRtxTransmit>,
}

/// Failure reported while preparing or operating a GPIO adapter.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GpioError {
    /// The supplied descriptor is incompatible with this client.
    IncompatibleAdapter,
    /// An argument or returned adapter value is invalid.
    InvalidArgument,
    /// The adapter could not reserve setup memory.
    NoMemory,
    /// A CM119 USB operation failed.
    Usb,
    /// The selected device or operation is unsupported.
    Unsupported,
    /// A parallel-port operation failed.
    Io,
    /// The adapter returned an undocumented result code.
    AdapterFailure,
}

impl fmt::Display for GpioError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::IncompatibleAdapter => "incompatible GPIO adapter",
            Self::InvalidArgument => "invalid GPIO-adapter argument",
            Self::NoMemory => "GPIO-adapter allocation failed",
            Self::Usb => "CM119 USB operation failed",
            Self::Unsupported => "unsupported GPIO device or operation",
            Self::Io => "parallel-port operation failed",
            Self::AdapterFailure => "GPIO adapter returned an unknown failure",
        };
        formatter.write_str(message)
    }
}

impl std::error::Error for GpioError {}

/// CM119 interface wiring profile.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Cm119Profile {
    /// Standard DudeUSB/URI wiring.
    DudeUsb = 0,
    /// SPH USB interface wiring.
    SphUsb = 1,
    /// NHRC/N1KDO interface wiring.
    Nhrc = 2,
    /// Current custom USBRadioPlus wiring.
    Custom = 3,
}

/// Exact setup for one CM119 HID interface.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cm119Config {
    /// Stable Linux USB topology such as `3-1.2`.
    pub usb_port_path: String,
    /// USB vendor identifier, or zero for the C-Media default.
    pub vendor_id: u16,
    /// USB product identifier, or zero for any supported CM108/CM119 device.
    pub product_id: u16,
    /// Physical wiring profile.
    pub profile: Cm119Profile,
    /// Whether physical PTT polarity is inverted.
    pub ptt_inverted: bool,
    /// GPIO bits configured as ordinary outputs.
    pub output_enable_mask: u8,
    /// Initial values for ordinary outputs.
    pub output_initial_mask: u8,
}

/// Stable information about one discovered CM119 interface.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cm119DeviceInfo {
    /// Whether the requested interface is present.
    pub present: bool,
    /// USB vendor identifier.
    pub vendor_id: u16,
    /// USB product identifier.
    pub product_id: u16,
    /// USB bus number.
    pub usb_bus: u32,
    /// USB topology port chain.
    pub usb_port_numbers: Vec<u8>,
    /// Reported serial number, when present.
    pub serial: Option<String>,
}

/// Bounded CM119 discovery result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cm119DeviceList {
    /// Total number of matching devices, including any omitted by the bound.
    pub matching_device_count: u32,
    /// Returned device records.
    pub devices: Vec<Cm119DeviceInfo>,
}

/// Complete physical-word tuning EEPROM image.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EepromImage {
    /// Whether the image has a valid established checksum.
    pub checksum_valid: bool,
    /// Whether the image has the established tuning magic value.
    pub magic_valid: bool,
    /// EEPROM words indexed by physical address.
    pub words: [u16; EEPROM_WORD_COUNT],
}

impl Default for EepromImage {
    fn default() -> Self {
        Self {
            checksum_valid: false,
            magic_valid: false,
            words: [0; EEPROM_WORD_COUNT],
        }
    }
}

/// Latest CM119 input state.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Cm119Inputs {
    /// Whether the HID interface is online.
    pub online: bool,
    /// Logical carrier indication.
    pub cor_active: bool,
    /// Logical external CTCSS indication.
    pub ctcss_active: bool,
    /// Logical GPIO input bits.
    pub gpio_input_mask: u8,
    /// Complete raw HID input report.
    pub hid_report: [u8; 4],
}

/// Prepared CM119 PTT and ordinary-output state.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Cm119Outputs {
    /// Logical PTT assertion.
    pub ptt_asserted: bool,
    /// Ordinary logical GPIO output bits.
    pub gpio_output_mask: u8,
}

/// Independently scheduled logical CM119 output inversions.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Cm119Pulse {
    /// Whether to invert PTT for the duration.
    pub invert_ptt: bool,
    /// Ordinary GPIO bits to invert for the duration.
    pub invert_gpio_mask: u8,
    /// Pulse duration in milliseconds; zero schedules nothing.
    pub duration_milliseconds: u32,
    /// Whether to cancel a pending PTT inversion.
    pub cancel_ptt: bool,
    /// Ordinary GPIO pulse deadlines to cancel.
    pub cancel_gpio_mask: u8,
}

/// Best-effort CM119 service statistics.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Cm119Statistics {
    /// HID input reads attempted.
    pub input_read_count: u64,
    /// HID output writes attempted.
    pub output_apply_count: u64,
    /// HID transfer errors after open.
    pub usb_error_count: u64,
    /// Last PTT state applied to hardware.
    pub ptt_applied: bool,
    /// Whether the HID interface is online.
    pub online: bool,
    /// Last libusb error code.
    pub last_usb_error: i32,
    /// EEPROM words read.
    pub eeprom_read_count: u64,
    /// EEPROM words written.
    pub eeprom_write_count: u64,
}

/// Linux parallel-port transport.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParallelTransport {
    /// Try ppdev and then the explicitly configured raw I/O range.
    Automatic = 0,
    /// Use only Linux ppdev.
    Ppdev = 1,
    /// Use only the explicitly configured raw x86 I/O range.
    RawIo = 2,
}

/// Exact setup for one parallel-port GPIO transport.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParallelConfig {
    /// Transport selection.
    pub transport: ParallelTransport,
    /// Optional ppdev path such as `/dev/parport0`.
    pub ppdev_path: Option<String>,
    /// Optional raw I/O base such as `0x378`.
    pub raw_io_base: u32,
    /// Data-register bits available as outputs.
    pub output_enable_mask: u8,
    /// Initial persistent output byte.
    pub output_initial_mask: u8,
}

/// Latest parallel-port status input.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParallelInputs {
    /// Whether the transport is online.
    pub online: bool,
    /// Raw IEEE 1284 status-register byte.
    pub status_mask: u8,
}

/// Prepared persistent parallel outputs and optional pulse.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParallelOutputs {
    /// Complete persistent data-register byte.
    pub output_mask: u8,
    /// Bits asserted for a new active-high pulse.
    pub pulse_mask: u8,
    /// Pulse duration in milliseconds; zero starts no pulse.
    pub pulse_duration_milliseconds: u32,
    /// Whether to cancel the active pulse.
    pub cancel_pulse: bool,
}

/// Independently scheduled parallel output inversions.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParallelPulse {
    /// Data bits to invert for the duration.
    pub invert_mask: u8,
    /// Pulse duration in milliseconds; zero schedules nothing.
    pub duration_milliseconds: u32,
    /// Pending bit deadlines to cancel.
    pub cancel_mask: u8,
}

/// Established parallel-port RTX synthesizer request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ParallelRtx {
    /// Receiver frequency in whole hertz.
    pub receive_hz: u32,
    /// Transmitter frequency in whole hertz.
    pub transmit_hz: u32,
    /// Whether the transmitter is currently keyed.
    pub transmitting: bool,
    /// Whether the radio's high-power setting was requested.
    pub high_power: bool,
}

/// Best-effort parallel-port service statistics.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParallelStatistics {
    /// Status-register reads attempted.
    pub input_read_count: u64,
    /// Data-register writes attempted.
    pub output_apply_count: u64,
    /// I/O errors after open.
    pub io_error_count: u64,
    /// Whether the transport is online.
    pub online: bool,
    /// Last operating-system error code.
    pub last_io_error: i32,
    /// Last output byte applied.
    pub applied_output_mask: u8,
}

/// Validated function table for the process-loaded GPIO adapter.
#[derive(Clone, Copy)]
pub struct GpioAdapter {
    descriptor: Descriptor,
}

// SAFETY: construction requires a process-lifetime immutable descriptor; the
// copied table contains only immutable function and capability pointers.
unsafe impl Send for GpioAdapter {}
// SAFETY: descriptor calls which require serialization are exposed only on
// exclusively borrowed device handles, not through this copyable provider.
unsafe impl Sync for GpioAdapter {}

impl GpioAdapter {
    /// Validate and copy a process-lifetime adapter descriptor.
    ///
    /// # Safety
    ///
    /// `descriptor` must point to a correctly aligned descriptor whose shared
    /// object and function addresses remain loaded while any returned adapter
    /// or device exists.
    pub unsafe fn from_raw(descriptor: *const c_void) -> Result<Self, GpioError> {
        let pointer = NonNull::new(descriptor.cast_mut().cast::<Descriptor>())
            .ok_or(GpioError::IncompatibleAdapter)?;
        // SAFETY: the caller guarantees a readable descriptor header.
        let header = unsafe { pointer.as_ref() };
        if header.struct_size < size_of::<Descriptor>() as u32
            || header.abi_version != ABI_VERSION
            || header.capability_name.is_null()
        {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: the size check above proves that the full ABI-1 descriptor is readable.
        let descriptor = unsafe { *pointer.as_ptr() };
        // SAFETY: the descriptor contract requires a valid static C string.
        let capability = unsafe { CStr::from_ptr(descriptor.capability_name) };
        if capability != CAPABILITY
            || descriptor.device_probe.is_none()
            || descriptor.device_open.is_none()
            || descriptor.device_publish_outputs.is_none()
            || descriptor.device_service.is_none()
            || descriptor.device_get_inputs.is_none()
            || descriptor.device_get_stats.is_none()
            || descriptor.device_close.is_none()
            || descriptor.device_discover.is_none()
            || descriptor.device_read_eeprom.is_none()
            || descriptor.device_write_eeprom.is_none()
            || descriptor.parallel_open.is_none()
            || descriptor.parallel_publish_outputs.is_none()
            || descriptor.parallel_service.is_none()
            || descriptor.parallel_control_write_data.is_none()
            || descriptor.parallel_get_inputs.is_none()
            || descriptor.parallel_get_stats.is_none()
            || descriptor.parallel_close.is_none()
            || descriptor.device_schedule_inverting_pulse.is_none()
            || descriptor.parallel_schedule_inverting_pulse.is_none()
        {
            return Err(GpioError::IncompatibleAdapter);
        }
        Ok(Self { descriptor })
    }

    /// Enumerate the bounded set of supported CM119 devices.
    pub fn discover(&self) -> Result<Cm119DeviceList, GpioError> {
        let mut raw = RawDeviceList::default();
        // SAFETY: validation requires this function and `raw` is writable for the call.
        map_result(unsafe { self.descriptor.device_discover.unwrap_unchecked()(&mut raw) })?;
        validate_returned(raw.struct_size, raw.abi_version, size_of::<RawDeviceList>())?;
        // The supported Debian amd64 and arm64 targets have a 64-bit `usize`,
        // so this ABI u32 count cannot overflow the Rust slice index type.
        let returned = raw.returned_device_count as usize;
        if returned > DEVICE_CAPACITY || raw.returned_device_count > raw.matching_device_count {
            return Err(GpioError::InvalidArgument);
        }
        let devices = raw.devices[..returned]
            .iter()
            .map(device_info_from_raw)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Cm119DeviceList {
            matching_device_count: raw.matching_device_count,
            devices,
        })
    }

    /// Probe one stable CM119 identity without claiming it.
    pub fn probe(&self, config: &Cm119Config) -> Result<Cm119DeviceInfo, GpioError> {
        let path = checked_c_string(&config.usb_port_path)?;
        let raw_config = raw_device_config(config, &path);
        let mut info = RawDeviceInfo::default();
        // SAFETY: validation requires this function and both structures live for the call.
        map_result(unsafe {
            self.descriptor.device_probe.unwrap_unchecked()(&raw_config, &mut info)
        })?;
        device_info_from_raw(&info)
    }

    /// Open and exclusively own one exact CM119 HID interface.
    pub fn open_cm119(&self, config: &Cm119Config) -> Result<Cm119Device, GpioError> {
        let path = checked_c_string(&config.usb_port_path)?;
        let raw_config = raw_device_config(config, &path);
        let mut device = ptr::null_mut();
        // SAFETY: validation requires this function and the adapter initializes `device`.
        map_result(unsafe {
            self.descriptor.device_open.unwrap_unchecked()(&raw_config, &mut device)
        })?;
        Ok(Cm119Device {
            descriptor: self.descriptor,
            device: NonNull::new(device).ok_or(GpioError::AdapterFailure)?,
        })
    }

    /// Open and exclusively own one explicit parallel-port transport.
    pub fn open_parallel(&self, config: &ParallelConfig) -> Result<ParallelDevice, GpioError> {
        let path = config
            .ppdev_path
            .as_deref()
            .map(checked_c_string)
            .transpose()?;
        let raw = RawParallelConfig {
            struct_size: size_of::<RawParallelConfig>() as u32,
            abi_version: ABI_VERSION,
            transport: config.transport as u32,
            ppdev_path: path.as_ref().map_or(ptr::null(), |value| value.as_ptr()),
            raw_io_base: config.raw_io_base,
            output_enable_mask: u32::from(config.output_enable_mask),
            output_initial_mask: u32::from(config.output_initial_mask),
        };
        let mut device = ptr::null_mut();
        // SAFETY: validation requires this function and the adapter initializes `device`.
        map_result(unsafe { self.descriptor.parallel_open.unwrap_unchecked()(&raw, &mut device) })?;
        Ok(ParallelDevice {
            descriptor: self.descriptor,
            device: NonNull::new(device).ok_or(GpioError::AdapterFailure)?,
        })
    }
}

/// Exclusively owned CM119 HID device.
pub struct Cm119Device {
    descriptor: Descriptor,
    device: NonNull<OpaqueCm119Device>,
}

// SAFETY: the descriptor guarantees lock-free publication/snapshots across
// threads, while all HID service and EEPROM methods require exclusive access.
unsafe impl Send for Cm119Device {}
// SAFETY: shared methods invoke only the descriptor's explicitly lock-free API.
unsafe impl Sync for Cm119Device {}

impl Cm119Device {
    /// Publish the newest prepared PTT and GPIO output state without HID I/O.
    pub fn publish_outputs(&self, outputs: Cm119Outputs) -> Result<(), GpioError> {
        let raw = RawCm119Outputs {
            struct_size: size_of::<RawCm119Outputs>() as u32,
            abi_version: ABI_VERSION,
            ptt_asserted: u32::from(outputs.ptt_asserted),
            gpio_output_mask: u32::from(outputs.gpio_output_mask),
        };
        // SAFETY: this handle remains live and validation requires the function.
        map_result(unsafe {
            self.descriptor.device_publish_outputs.unwrap_unchecked()(self.device.as_ptr(), &raw)
        })
    }

    /// Schedule or cancel independent logical output inversions without HID I/O.
    pub fn schedule_pulse(&self, pulse: Cm119Pulse) -> Result<(), GpioError> {
        if pulse.invert_gpio_mask & pulse.cancel_gpio_mask != 0
            || (pulse.invert_ptt && pulse.cancel_ptt)
        {
            return Err(GpioError::InvalidArgument);
        }
        let raw = RawCm119ScheduledPulse {
            struct_size: size_of::<RawCm119ScheduledPulse>() as u32,
            abi_version: ABI_VERSION,
            ptt_invert: u32::from(pulse.invert_ptt),
            gpio_invert_mask: u32::from(pulse.invert_gpio_mask),
            pulse_duration_milliseconds: pulse.duration_milliseconds,
            ptt_cancel: u32::from(pulse.cancel_ptt),
            gpio_cancel_mask: u32::from(pulse.cancel_gpio_mask),
        };
        // SAFETY: validation requires this lock-free function and the handle is live.
        map_result(unsafe {
            self.descriptor
                .device_schedule_inverting_pulse
                .unwrap_unchecked()(self.device.as_ptr(), &raw)
        })
    }

    /// Flush outputs and sample inputs on the sole non-real-time service owner.
    pub fn service(&mut self) -> Result<(), GpioError> {
        // SAFETY: exclusive access enforces the adapter's single service-owner contract.
        map_result(unsafe {
            self.descriptor.device_service.unwrap_unchecked()(self.device.as_ptr())
        })
    }

    /// Read the latest lock-free best-effort input snapshot.
    pub fn inputs(&self) -> Result<Cm119Inputs, GpioError> {
        let mut raw = RawCm119Inputs {
            struct_size: size_of::<RawCm119Inputs>() as u32,
            abi_version: ABI_VERSION,
            ..RawCm119Inputs::default()
        };
        // SAFETY: validation requires this lock-free function and `raw` is writable.
        map_result(unsafe {
            self.descriptor.device_get_inputs.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(
            raw.struct_size,
            raw.abi_version,
            size_of::<RawCm119Inputs>(),
        )?;
        Ok(Cm119Inputs {
            online: raw.online != 0,
            cor_active: raw.cor_active != 0,
            ctcss_active: raw.ctcss_active != 0,
            gpio_input_mask: narrow_byte(raw.gpio_input_mask)?,
            hid_report: raw.hid_report,
        })
    }

    /// Read the latest lock-free best-effort activity statistics.
    pub fn statistics(&self) -> Result<Cm119Statistics, GpioError> {
        let mut raw = RawCm119Stats {
            struct_size: size_of::<RawCm119Stats>() as u32,
            abi_version: ABI_VERSION,
            ..RawCm119Stats::default()
        };
        // SAFETY: validation requires this lock-free function and `raw` is writable.
        map_result(unsafe {
            self.descriptor.device_get_stats.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(raw.struct_size, raw.abi_version, size_of::<RawCm119Stats>())?;
        Ok(Cm119Statistics {
            input_read_count: raw.input_read_count,
            output_apply_count: raw.output_apply_count,
            usb_error_count: raw.usb_error_count,
            ptt_applied: raw.ptt_applied != 0,
            online: raw.online != 0,
            last_usb_error: raw.last_usb_error,
            eeprom_read_count: raw.eeprom_read_count,
            eeprom_write_count: raw.eeprom_write_count,
        })
    }

    /// Read the established tuning EEPROM through the service owner.
    pub fn read_eeprom(&mut self) -> Result<EepromImage, GpioError> {
        let mut raw = RawEepromImage::default();
        // SAFETY: exclusive access serializes EEPROM I/O with device service.
        map_result(unsafe {
            self.descriptor.device_read_eeprom.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(
            raw.struct_size,
            raw.abi_version,
            size_of::<RawEepromImage>(),
        )?;
        Ok(EepromImage {
            checksum_valid: raw.checksum_valid != 0,
            magic_valid: raw.magic_valid != 0,
            words: raw.words,
        })
    }

    /// Write an established tuning EEPROM image through the service owner.
    pub fn write_eeprom(&mut self, image: &mut EepromImage) -> Result<(), GpioError> {
        let mut raw = RawEepromImage {
            struct_size: size_of::<RawEepromImage>() as u32,
            abi_version: ABI_VERSION,
            checksum_valid: u32::from(image.checksum_valid),
            magic_valid: u32::from(image.magic_valid),
            words: image.words,
        };
        // SAFETY: exclusive access serializes EEPROM I/O with device service.
        map_result(unsafe {
            self.descriptor.device_write_eeprom.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(
            raw.struct_size,
            raw.abi_version,
            size_of::<RawEepromImage>(),
        )?;
        image.checksum_valid = raw.checksum_valid != 0;
        image.magic_valid = raw.magic_valid != 0;
        image.words = raw.words;
        Ok(())
    }
}

impl Drop for Cm119Device {
    fn drop(&mut self) {
        // SAFETY: this object uniquely owns the live device handle.
        unsafe { self.descriptor.device_close.unwrap_unchecked()(self.device.as_ptr()) };
    }
}

/// Exclusively owned parallel-port GPIO device.
pub struct ParallelDevice {
    descriptor: Descriptor,
    device: NonNull<OpaqueParallelDevice>,
}

// SAFETY: the descriptor guarantees lock-free publication/snapshots across
// threads, while service and direct writes require exclusive access.
unsafe impl Send for ParallelDevice {}
// SAFETY: shared methods invoke only the descriptor's explicitly lock-free API.
unsafe impl Sync for ParallelDevice {}

impl ParallelDevice {
    /// Publish persistent outputs and an optional active-high pulse without I/O.
    pub fn publish_outputs(&self, outputs: ParallelOutputs) -> Result<(), GpioError> {
        let raw = RawParallelOutputs {
            struct_size: size_of::<RawParallelOutputs>() as u32,
            abi_version: ABI_VERSION,
            output_mask: u32::from(outputs.output_mask),
            pulse_mask: u32::from(outputs.pulse_mask),
            pulse_duration_milliseconds: outputs.pulse_duration_milliseconds,
            cancel_pulse: u32::from(outputs.cancel_pulse),
        };
        // SAFETY: validation requires this lock-free function and the handle is live.
        map_result(unsafe {
            self.descriptor.parallel_publish_outputs.unwrap_unchecked()(self.device.as_ptr(), &raw)
        })
    }

    /// Schedule or cancel independent data-bit inversions without port I/O.
    pub fn schedule_pulse(&self, pulse: ParallelPulse) -> Result<(), GpioError> {
        if pulse.invert_mask & pulse.cancel_mask != 0 {
            return Err(GpioError::InvalidArgument);
        }
        let raw = RawParallelScheduledPulse {
            struct_size: size_of::<RawParallelScheduledPulse>() as u32,
            abi_version: ABI_VERSION,
            invert_mask: u32::from(pulse.invert_mask),
            pulse_duration_milliseconds: pulse.duration_milliseconds,
            cancel_mask: u32::from(pulse.cancel_mask),
        };
        // SAFETY: validation requires this lock-free function and the handle is live.
        map_result(unsafe {
            self.descriptor
                .parallel_schedule_inverting_pulse
                .unwrap_unchecked()(self.device.as_ptr(), &raw)
        })
    }

    /// Apply outputs and sample inputs on the sole non-real-time service owner.
    pub fn service(&mut self) -> Result<(), GpioError> {
        // SAFETY: exclusive access enforces the adapter's single service-owner contract.
        map_result(unsafe {
            self.descriptor.parallel_service.unwrap_unchecked()(self.device.as_ptr())
        })
    }

    /// Perform one serialized immediate control-plane data-register write.
    pub fn write_data(&mut self, data: u8) -> Result<(), GpioError> {
        // SAFETY: exclusive access serializes this direct I/O with service.
        map_result(unsafe {
            self.descriptor
                .parallel_control_write_data
                .unwrap_unchecked()(self.device.as_ptr(), u32::from(data))
        })
    }

    /// Apply one serialized active-low four-bit channel selection.
    pub fn set_binary_channel(&mut self, channel: u8) -> Result<(), GpioError> {
        let operation = self
            .descriptor
            .parallel_set_binary_channel
            .ok_or(GpioError::Unsupported)?;
        // SAFETY: exclusive access serializes this direct I/O with service.
        map_result(unsafe { operation(self.device.as_ptr(), channel) })
    }

    /// Program the established parallel-port RTX synthesizer.
    pub fn program_rtx(&mut self, request: ParallelRtx) -> Result<(), GpioError> {
        let operation = self
            .descriptor
            .parallel_program_rtx
            .ok_or(GpioError::Unsupported)?;
        // SAFETY: exclusive access serializes this direct I/O with service.
        map_result(unsafe {
            operation(
                self.device.as_ptr(),
                request.receive_hz,
                request.transmit_hz,
                u32::from(request.transmitting),
                u32::from(request.high_power),
            )
        })
    }

    /// Immediately release the established parallel-port RTX transmitter.
    pub fn clear_rtx_transmit(&mut self) -> Result<(), GpioError> {
        let operation = self
            .descriptor
            .parallel_clear_rtx_transmit
            .ok_or(GpioError::Unsupported)?;
        // SAFETY: exclusive access serializes this direct I/O with service.
        map_result(unsafe { operation(self.device.as_ptr()) })
    }

    /// Read the latest lock-free best-effort input snapshot.
    pub fn inputs(&self) -> Result<ParallelInputs, GpioError> {
        let mut raw = RawParallelInputs {
            struct_size: size_of::<RawParallelInputs>() as u32,
            abi_version: ABI_VERSION,
            ..RawParallelInputs::default()
        };
        // SAFETY: validation requires this lock-free function and `raw` is writable.
        map_result(unsafe {
            self.descriptor.parallel_get_inputs.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(
            raw.struct_size,
            raw.abi_version,
            size_of::<RawParallelInputs>(),
        )?;
        Ok(ParallelInputs {
            online: raw.online != 0,
            status_mask: narrow_byte(raw.status_mask)?,
        })
    }

    /// Read the latest lock-free best-effort activity statistics.
    pub fn statistics(&self) -> Result<ParallelStatistics, GpioError> {
        let mut raw = RawParallelStats {
            struct_size: size_of::<RawParallelStats>() as u32,
            abi_version: ABI_VERSION,
            ..RawParallelStats::default()
        };
        // SAFETY: validation requires this lock-free function and `raw` is writable.
        map_result(unsafe {
            self.descriptor.parallel_get_stats.unwrap_unchecked()(self.device.as_ptr(), &mut raw)
        })?;
        validate_returned(
            raw.struct_size,
            raw.abi_version,
            size_of::<RawParallelStats>(),
        )?;
        Ok(ParallelStatistics {
            input_read_count: raw.input_read_count,
            output_apply_count: raw.output_apply_count,
            io_error_count: raw.io_error_count,
            online: raw.online != 0,
            last_io_error: raw.last_io_error,
            applied_output_mask: narrow_byte(raw.applied_output_mask)?,
        })
    }
}

impl Drop for ParallelDevice {
    fn drop(&mut self) {
        // SAFETY: this object uniquely owns the live device handle.
        unsafe { self.descriptor.parallel_close.unwrap_unchecked()(self.device.as_ptr()) };
    }
}

fn raw_device_config<'a>(config: &'a Cm119Config, path: &'a CString) -> RawDeviceConfig {
    RawDeviceConfig {
        struct_size: size_of::<RawDeviceConfig>() as u32,
        abi_version: ABI_VERSION,
        usb_port_path: path.as_ptr(),
        vendor_id: config.vendor_id,
        product_id: config.product_id,
        profile: config.profile as u32,
        ptt_inverted: u32::from(config.ptt_inverted),
        gpio_output_enable_mask: u32::from(config.output_enable_mask),
        gpio_output_initial_mask: u32::from(config.output_initial_mask),
    }
}

fn device_info_from_raw(raw: &RawDeviceInfo) -> Result<Cm119DeviceInfo, GpioError> {
    validate_returned(raw.struct_size, raw.abi_version, size_of::<RawDeviceInfo>())?;
    // The supported Debian amd64 and arm64 targets have a 64-bit `usize`,
    // so this ABI u32 count cannot overflow the Rust slice index type.
    let count = raw.usb_port_number_count as usize;
    if count > raw.usb_port_numbers.len() {
        return Err(GpioError::InvalidArgument);
    }
    Ok(Cm119DeviceInfo {
        present: raw.present != 0,
        vendor_id: raw.vendor_id,
        product_id: raw.product_id,
        usb_bus: raw.usb_bus,
        usb_port_numbers: raw.usb_port_numbers[..count].to_vec(),
        serial: string_from_array(&raw.serial)?,
    })
}

fn checked_c_string(value: &str) -> Result<CString, GpioError> {
    if value.is_empty() {
        return Err(GpioError::InvalidArgument);
    }
    CString::new(value).map_err(|_| GpioError::InvalidArgument)
}

fn string_from_array(value: &[c_char]) -> Result<Option<String>, GpioError> {
    let end = value.iter().position(|character| *character == 0);
    let bytes = end
        .map(|length| &value[..length])
        .ok_or(GpioError::InvalidArgument)?
        .iter()
        .map(|character| *character as u8)
        .collect::<Vec<_>>();
    if bytes.is_empty() {
        return Ok(None);
    }
    String::from_utf8(bytes)
        .map(Some)
        .map_err(|_| GpioError::InvalidArgument)
}

fn narrow_byte(value: u32) -> Result<u8, GpioError> {
    u8::try_from(value).map_err(|_| GpioError::InvalidArgument)
}

fn validate_returned(struct_size: u32, abi_version: u32, expected: usize) -> Result<(), GpioError> {
    if struct_size < expected as u32 || abi_version != ABI_VERSION {
        return Err(GpioError::InvalidArgument);
    }
    Ok(())
}

fn map_result(result: c_int) -> Result<(), GpioError> {
    match result {
        RESULT_OK => Ok(()),
        -1 => Err(GpioError::InvalidArgument),
        -2 => Err(GpioError::NoMemory),
        -3 => Err(GpioError::Usb),
        -4 => Err(GpioError::Unsupported),
        -5 => Err(GpioError::Io),
        _ => Err(GpioError::AdapterFailure),
    }
}

#[cfg(test)]
mod tests;
