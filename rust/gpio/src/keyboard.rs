//! Exclusive non-real-time observation of configured evdev key codes.
use super::*;
#[derive(Clone, Copy)]
#[repr(C)]
struct Api {
    size: u32,
    version: u32,
    name: *const c_char,
    open: Option<unsafe extern "C" fn(*const c_char, *const u32, u32, *mut *mut c_void) -> c_int>,
    sample: Option<unsafe extern "C" fn(*mut c_void, *mut u32) -> c_int>,
    close: Option<unsafe extern "C" fn(*mut c_void)>,
    sample_valid: Option<unsafe extern "C" fn(*mut c_void, *mut u32, *mut u32) -> c_int>,
}

/// A non-grabbing evdev observation handle. The service owner samples all selected keys together.
pub struct KeyboardDevice {
    api: Api,
    handle: NonNull<c_void>,
    count: u32,
}
// SAFETY: no calls overlap; the descriptor permits transfer to one physical owner.
unsafe impl Send for KeyboardDevice {}
impl GpioAdapter {
    fn keyboard_operations(&self) -> Result<Api, GpioError> {
        let get = self.keyboard.ok_or(GpioError::IncompatibleAdapter)?;
        // SAFETY: the optional getter belongs to the same retained immutable provider.
        let raw = unsafe { get() }.cast::<Api>();
        if raw.is_null() || !raw.is_aligned() {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: all descriptor results start with a readable common header.
        let header = unsafe { raw.cast::<[u32; 2]>().read() };
        if header[0] < size_of::<Api>() as u32 || header[1] != 1 {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: the size check covers the complete result.
        let api = unsafe { raw.read() };
        if api.name.is_null()
            || api.open.is_none()
            || api.sample.is_none()
            || api.close.is_none()
            || api.sample_valid.is_none()
        {
            return Err(GpioError::IncompatibleAdapter);
        }
        // SAFETY: the immutable descriptor's name is a terminated C string.
        if unsafe { CStr::from_ptr(api.name) } != c"rptadv.evdev-held-key1" {
            return Err(GpioError::IncompatibleAdapter);
        }
        Ok(api)
    }
    /// Check the exact evdev ABI and identity without touching a device.
    pub fn has_keyboard(&self) -> bool {
        self.keyboard_operations().is_ok()
    }
    /// Open an explicitly selected input endpoint; the provider checks every supplied key code.
    pub fn open_keyboard(&self, path: &str, keys: &[u32]) -> Result<KeyboardDevice, GpioError> {
        let api = self.keyboard_operations()?;
        let path = CString::new(path).map_err(|_| GpioError::InvalidArgument)?;
        let count = u32::try_from(keys.len()).map_err(|_| GpioError::InvalidArgument)?;
        let mut handle = ptr::null_mut();
        // SAFETY: the setup buffers span this synchronous call and the result transfers sole ownership.
        map_result(unsafe { api.open.unwrap()(path.as_ptr(), keys.as_ptr(), count, &mut handle) })?;
        Ok(KeyboardDevice {
            api,
            handle: NonNull::new(handle).ok_or(GpioError::AdapterFailure)?,
            count,
        })
    }
}
impl KeyboardDevice {
    /// Read one current key bitmap from the physical owner, including key releases.
    pub fn sample(&mut self) -> Result<u32, GpioError> {
        let (down, valid) = self.sample_valid()?;
        if valid == (1 << self.count) - 1 {
            Ok(down)
        } else {
            Err(GpioError::Unsupported)
        }
    }

    /// Return held and supported bits independently for every supplied EV_KEY code.
    pub fn sample_valid(&mut self) -> Result<(u32, u32), GpioError> {
        let (mut mask, mut valid) = (0, 0);
        // SAFETY: exclusive live evdev owner and writable output word.
        map_result(unsafe {
            self.api.sample_valid.unwrap()(self.handle.as_ptr(), &mut mask, &mut valid)
        })?;
        if valid >= 1 << self.count || mask & !valid != 0 {
            return Err(GpioError::AdapterFailure);
        }
        Ok((mask, valid))
    }
}
impl Drop for KeyboardDevice {
    fn drop(&mut self) {
        // SAFETY: this is the sole stopped observation owner and close consumes it.
        unsafe { self.api.close.unwrap()(self.handle.as_ptr()) };
    }
}
