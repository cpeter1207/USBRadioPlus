//! Exclusive, non-real-time modem-line ownership through the staged GPIO SDK.
use super::*;

#[repr(C)]
struct RawConfig {
    size: u32,
    version: u32,
    path: *const c_char,
    baud: u32,
    data_bits: u32,
    parity: u32,
    stop_half_bits: u32,
    outputs: u32,
    idle: u32,
}

#[derive(Clone, Copy)]
#[repr(C)]
struct SerialDescriptor {
    size: u32,
    version: u32,
    name: *const c_char,
    open: Option<unsafe extern "C" fn(*const RawConfig, *mut *mut c_void) -> c_int>,
    service: Option<unsafe extern "C" fn(*mut c_void, u32, *mut u32, *mut u32) -> c_int>,
    close: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    service_outputs: Option<unsafe extern "C" fn(*mut c_void, u32, *mut u32, *mut u32) -> c_int>,
    open_retiring: Option<unsafe extern "C" fn(*const RawConfig, *mut *mut c_void) -> c_int>,
    quarantine: Option<unsafe extern "C" fn(*mut c_void)>,
}

/// Immutable UART settings and exclusive line assignments.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SerialConfig {
    /// Explicit dedicated tty pathname.
    pub path: String,
    /// Configured baud rate.
    pub baud: u32,
    /// Character size, 5 through 8.
    pub data_bits: u32,
    /// None, odd, even, mark or space (0 through 4).
    pub parity: u32,
    /// Twice the requested stop bit count.
    pub stop_half_bits: u32,
    /// Selected transmit outputs, bit 0 RTS and bit 1 DTR.
    pub outputs: u32,
    /// Physical levels of those outputs at idle; unused lines remain low.
    pub idle: u32,
}

/// Unique modem owner; only its non-real-time service thread calls the adapter.
pub struct SerialDevice {
    ops: SerialDescriptor,
    handle: Option<NonNull<c_void>>,
    startup: Result<(), GpioError>,
}

// SAFETY: the descriptor permits a sequential transfer; Rust requires exclusive
// access to every physical operation and retains the process-lifetime functions.
unsafe impl Send for SerialDevice {}

impl GpioAdapter {
    /// Check the optional modem-line table on the control plane before acquiring any station hardware.
    pub fn has_serial(&self) -> bool {
        self.serial_operations().is_ok()
    }
    fn serial_operations(&self) -> Result<SerialDescriptor, GpioError> {
        let getter = self.serial.ok_or(GpioError::IncompatibleAdapter)?;
        // SAFETY: the validated process-lifetime descriptor owns this getter and its result.
        let pointer = unsafe { getter() }.cast::<SerialDescriptor>();
        if pointer.is_null() || !pointer.is_aligned() {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: all advertised descriptor tables expose the two-word header.
        let header = unsafe { pointer.cast::<[u32; 2]>().read() };
        if header[0] < size_of::<SerialDescriptor>() as u32 || header[1] != 1 {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: the previous size check covers every slot.
        let ops = unsafe { pointer.read() };
        if ops.name.is_null()
            || ops.open.is_none()
            || ops.service.is_none()
            || ops.close.is_none()
            || ops.service_outputs.is_none()
            || ops.open_retiring.is_none()
            || ops.quarantine.is_none()
        {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: the capability contract supplies a static, nul-terminated name.
        if unsafe { CStr::from_ptr(ops.name) } != c"rptadv.serial-modem1" {
            return Err(GpioError::IncompatibleAdapter);
        }
        Ok(ops)
    }

    /// Open a fully configured dedicated modem. A failed startup remains safely retained by the adapter.
    pub fn open_serial(&self, config: &SerialConfig) -> Result<SerialDevice, GpioError> {
        let owner = self.open_serial_retiring(config)?;
        owner.startup()?;
        Ok(owner)
    }

    /// Acquire the modem, preserving a partially configured owner for the station's retirement queue.
    pub fn open_serial_retiring(&self, config: &SerialConfig) -> Result<SerialDevice, GpioError> {
        let ops = self.serial_operations()?;
        let path = CString::new(config.path.as_bytes()).map_err(|_| GpioError::InvalidArgument)?;
        let raw = RawConfig {
            size: size_of::<RawConfig>() as u32,
            version: 1,
            path: path.as_ptr(),
            baud: config.baud,
            data_bits: config.data_bits,
            parity: config.parity,
            stop_half_bits: config.stop_half_bits,
            outputs: config.outputs,
            idle: config.idle,
        };
        let mut result = ptr::null_mut();
        // SAFETY: the path/config remain alive; the adapter transfers exactly one owner on success.
        let startup = map_result(unsafe { ops.open_retiring.unwrap()(&raw, &mut result) });
        let Some(handle) = NonNull::new(result) else {
            return Err(startup.err().unwrap_or(GpioError::AdapterFailure));
        };
        Ok(SerialDevice {
            ops,
            handle: Some(handle),
            startup,
        })
    }
}

impl SerialDevice {
    /// A failed startup retains its sole owner for electrical retirement only.
    pub fn startup(&self) -> Result<(), GpioError> {
        self.startup
    }

    /// Apply independent logical PTT/encoder output bits and obtain fresh modem-line levels.
    pub fn service_outputs(&mut self, active: u32) -> Result<(u32, u32), GpioError> {
        let handle = self.handle.ok_or(GpioError::InvalidArgument)?;
        let (mut inputs, mut outputs) = (0, 0);
        // SAFETY: this service call borrows one live unique owner and distinct output words.
        map_result(unsafe {
            self.ops.service_outputs.unwrap()(handle.as_ptr(), active, &mut inputs, &mut outputs)
        })?;
        if inputs & !15 != 0 || outputs & !3 != 0 {
            return Err(GpioError::AdapterFailure);
        }
        Ok((inputs, outputs))
    }

    /// Release only after confirming idle; a failed operation retains the owner for retry.
    pub fn close(&mut self) -> Result<(), GpioError> {
        if let Some(handle) = self.handle {
            // SAFETY: a successful close consumes this handle; failure leaves it live.
            map_result(unsafe { self.ops.close.unwrap()(handle.as_ptr()) })?;
            self.handle = None;
        }
        Ok(())
    }
}
impl Drop for SerialDevice {
    fn drop(&mut self) {
        if self.close().is_err() {
            if let Some(handle) = self.handle.take() {
                // SAFETY: transfer sole ownership to the process-lifetime adapter. A future
                // open must retire this physical tty before configuring its successor.
                unsafe { self.ops.quarantine.unwrap()(handle.as_ptr()) };
            }
        }
    }
}
