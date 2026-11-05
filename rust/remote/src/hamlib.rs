//! Owned dependency calls; RF policy and scan state stay in librptadvremote.

use crate::{HamlibProvider, raw};
use std::{ffi::c_void, fmt, mem::size_of, ptr::NonNull, sync::Arc};

/// Preserved dependency status, without secret-bearing transport values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CallError(pub i32);
impl fmt::Display for CallError {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(out, "remote radio operation failed ({})", self.0)
    }
}
impl std::error::Error for CallError {}
pub(crate) fn check(code: i32) -> Result<(), CallError> {
    if code == 0 {
        Ok(())
    } else {
        Err(CallError(code))
    }
}

/// One owned adapter-neutral operating capability.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Setting {
    /// Stable selector, independent of Hamlib flags.
    pub selector: u32,
    /// Stable provider name, localized when presented to the operator.
    pub name: String,
    /// Actual getter is available.
    pub readable: bool,
    /// Actual setter is available; complete-state writes supply companion values.
    pub writable: bool,
    /// Integer units are divided by ten to this power.
    pub decimal_places: u32,
    /// Inclusive lower bound in declared units.
    pub minimum: i64,
    /// Inclusive upper bound in declared units.
    pub maximum: i64,
    /// A nonempty list restricts accepted values to these choices.
    pub choices: Vec<i64>,
}

/// One rig retained on its creating physical worker. NonNull intentionally
/// prevents moving it to another thread; the provider outlives destruction.
pub struct HamlibRig {
    provider: Arc<HamlibProvider>,
    context: NonNull<c_void>,
}
impl HamlibRig {
    /// Configure/open one selected rig on the current physical owner.
    ///
    /// # Safety
    /// Config has complete readable borrowed spans for this synchronous call.
    pub unsafe fn open(
        provider: Arc<HamlibProvider>,
        config: &raw::rptadv_hamlib_config,
    ) -> Result<Self, CallError> {
        let mut context = std::ptr::null_mut();
        // SAFETY: admission checked the API; caller retains complete input spans.
        let code = unsafe { provider.api().open.unwrap()(config, &mut context) };
        if code != 0 {
            if !context.is_null() {
                // SAFETY: a partial transferred handle belongs to this same provider.
                unsafe { provider.api().destroy.unwrap()(context) };
            }
            return Err(CallError(code));
        }
        Ok(Self {
            provider,
            context: NonNull::new(context).ok_or(CallError(-1003))?,
        })
    }
    /// Read an actual supported value; unsupported readback remains an error.
    pub fn get(&mut self, selector: u32) -> Result<i64, CallError> {
        let mut value = 0;
        // SAFETY: one retained owner calls its admitted adapter synchronously.
        check(unsafe {
            self.provider.api().get.unwrap()(self.context.as_ptr(), selector, &mut value)
        })?;
        Ok(value)
    }
    /// Apply a scalar after common RF admission. Coupled fields use grouped calls.
    pub fn set(&mut self, selector: u32, value: i64) -> Result<(), CallError> {
        // SAFETY: the context stays uniquely owned on this thread.
        check(unsafe { self.provider.api().set.unwrap()(self.context.as_ptr(), selector, value) })
    }
    /// Apply complete mode/passband, including on a write-only rig.
    pub fn set_mode_width(&mut self, mode: i64, width: u32) -> Result<(), CallError> {
        // SAFETY: the admitted adapter validates values on the unique owner.
        check(unsafe {
            self.provider.api().set_mode_width.unwrap()(self.context.as_ptr(), mode, width)
        })
    }
    /// Apply complete split/VFO routing without fabricated readback.
    pub fn set_split_vfos(
        &mut self,
        receive: i64,
        enabled: bool,
        transmit: i64,
    ) -> Result<(), CallError> {
        // SAFETY: the admitted adapter validates values on the unique owner.
        check(unsafe {
            self.provider.api().set_split_vfos.unwrap()(
                self.context.as_ptr(),
                receive,
                u32::from(enabled),
                transmit,
            )
        })
    }
    /// Convert absolute power using effective TX frequency/mode supplied by policy.
    pub fn set_power_mw(
        &mut self,
        power: u32,
        transmit_hz: u64,
        mode: i64,
    ) -> Result<(), CallError> {
        // SAFETY: the admitted adapter validates values on the unique owner.
        check(unsafe {
            self.provider.api().set_power_mw.unwrap()(
                self.context.as_ptr(),
                power,
                transmit_hz,
                mode,
            )
        })
    }
    /// Copy every typed setting and discrete choice; no borrowed name escapes.
    pub fn settings(&self) -> Result<Vec<Setting>, CallError> {
        let mut result = Vec::new();
        loop {
            // SAFETY: this C record contains only scalar fields and nullable pointers.
            let mut entry: raw::rptadv_hamlib_setting = unsafe { std::mem::zeroed() };
            entry.struct_size = size_of::<raw::rptadv_hamlib_setting>();
            // SAFETY: unique context and complete writable record are retained.
            let code = unsafe {
                self.provider.api().setting.unwrap()(
                    self.context.as_ptr(),
                    result.len(),
                    &mut entry,
                )
            };
            if code == 1 {
                return Ok(result);
            }
            check(code)?;
            if entry.struct_size != size_of::<raw::rptadv_hamlib_setting>()
                || entry.name.data.is_null()
                || entry.access & !3 != 0
            {
                return Err(CallError(-1003));
            }
            // SAFETY: the conforming provider lends name bytes while its DSO is retained.
            let name = unsafe {
                std::slice::from_raw_parts(entry.name.data.cast::<u8>(), entry.name.size)
            };
            let name = std::str::from_utf8(name)
                .map_err(|_| CallError(-1003))?
                .to_owned();
            let mut choices = Vec::new();
            for index in 0..entry.choice_count {
                let mut value = 0;
                // SAFETY: the adapter advertised this choice index and lends no pointers.
                check(unsafe {
                    self.provider.api().choice.unwrap()(
                        self.context.as_ptr(),
                        entry.selector,
                        index,
                        &mut value,
                    )
                })?;
                choices.push(value);
            }
            result.push(Setting {
                selector: entry.selector,
                name,
                readable: entry.access & 1 != 0,
                writable: entry.access & 2 != 0,
                decimal_places: entry.decimal_places,
                minimum: entry.minimum,
                maximum: entry.maximum,
                choices,
            });
        }
    }
    /// Copy negotiated per-range modes and power without flattening them.
    pub fn coverage(&self, transmit: bool) -> Result<Vec<raw::rptadv_hamlib_range>, CallError> {
        let mut result = Vec::new();
        loop {
            // SAFETY: range contains only scalar fields.
            let mut range: raw::rptadv_hamlib_range = unsafe { std::mem::zeroed() };
            range.struct_size = size_of::<raw::rptadv_hamlib_range>();
            // SAFETY: complete output and the sole retained context are valid.
            let code = unsafe {
                self.provider.api().range.unwrap()(
                    self.context.as_ptr(),
                    u32::from(transmit),
                    result.len(),
                    &mut range,
                )
            };
            if code == 1 {
                return Ok(result);
            }
            check(code)?;
            if range.struct_size != size_of::<raw::rptadv_hamlib_range>() {
                return Err(CallError(-1003));
            }
            result.push(range);
        }
    }
    /// Configured nominal exchange budget; this does not promise call cancellation.
    pub fn transport_budget_ms(&self) -> Result<u64, CallError> {
        let mut value = 0;
        // SAFETY: complete output and the retained creating-thread context are valid.
        check(unsafe {
            self.provider.api().transport_budget_ms.unwrap()(self.context.as_ptr(), &mut value)
        })?;
        Ok(value)
    }
    /// Unkey/close on the owning thread, preserving the first backend error.
    pub fn close(&mut self) -> Result<(), CallError> {
        // SAFETY: close retains the adapter's allocation until destroy below.
        check(unsafe { self.provider.api().close.unwrap()(self.context.as_ptr()) })
    }
}
impl Drop for HamlibRig {
    fn drop(&mut self) {
        // SAFETY: this is the sole owner, on its creating thread; destroy closes
        // even if the caller omitted explicit close or an earlier close failed.
        unsafe { self.provider.api().destroy.unwrap()(self.context.as_ptr()) };
    }
}
