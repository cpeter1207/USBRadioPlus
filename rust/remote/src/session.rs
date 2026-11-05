//! Unique dynamic portable-engine context. No tune or scan policy is duplicated here.

use crate::{CallError, RemoteProvider, hamlib::check, raw};
use std::{ffi::c_void, mem::size_of, ptr::NonNull, sync::Arc};

/// Synchronous operations supplied by one existing non-audio physical owner.
/// The dynamic engine owns all tune/scan policy. Calls must not reenter it;
/// audio/control callbacks never implement or invoke this interface.
pub trait OperatingIo {
    /// Actual fields, qualified by the session's admitted readback mask.
    fn read_state(&mut self) -> Result<raw::RptAdvRemoteState, CallError>;
    /// Apply complete CAT/GPIO state and acknowledge the existing DSP update.
    fn apply_state(&mut self, state: &raw::RptAdvRemoteState) -> Result<(), CallError>;
    /// Synchronously service the selected physical PTT output.
    fn set_ptt(&mut self, keyed: bool) -> Result<(), CallError>;
    /// Publish existing bounded manual key/tone intent for native rendering.
    fn set_tune(&mut self, enabled: bool) -> Result<(), CallError>;
    /// Owner clock after potentially blocking operations, in advance's time base.
    fn monotonic_ms(&self) -> u64;
    /// Callback-published raw transmit rising-edge count.
    fn transmit_epoch(&self) -> u64;
}

struct BorrowedBackend<'a> {
    io: &'a mut dyn OperatingIo,
    panicked: bool,
}

fn invoke<T>(
    context: *mut c_void,
    call: impl FnOnce(&mut dyn OperatingIo) -> Result<T, CallError>,
) -> Result<T, i32> {
    if context.is_null() || !context.cast::<BorrowedBackend<'_>>().is_aligned() {
        return Err(-1003);
    }
    // SAFETY: advance_with exclusively lends this stack owner during synchronous advance.
    let borrowed = unsafe { &mut *context.cast::<BorrowedBackend<'_>>() };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| call(borrowed.io))) {
        Ok(result) => result.map_err(|error| error.0),
        Err(_) => {
            borrowed.panicked = true;
            Err(-1003)
        }
    }
}
unsafe extern "C" fn read(context: *mut c_void, out: *mut raw::RptAdvRemoteState) -> i32 {
    if out.is_null() || !out.is_aligned() {
        return -1003;
    }
    invoke(context, |io| {
        let state = io.read_state()?;
        // SAFETY: the admitted engine lends one complete writable state.
        unsafe { out.write(state) };
        Ok(())
    })
    .map_or_else(|error| error, |()| 0)
}
unsafe extern "C" fn apply(context: *mut c_void, state: *const raw::RptAdvRemoteState) -> i32 {
    if state.is_null() || !state.is_aligned() {
        return -1003;
    }
    // SAFETY: the admitted engine lends one complete state throughout this synchronous call.
    invoke(context, |io| io.apply_state(unsafe { &*state })).map_or_else(|error| error, |()| 0)
}
unsafe extern "C" fn ptt(context: *mut c_void, enabled: u32) -> i32 {
    if enabled > 1 {
        return -1003;
    }
    invoke(context, |io| io.set_ptt(enabled != 0)).map_or_else(|error| error, |()| 0)
}
unsafe extern "C" fn tune(context: *mut c_void, enabled: u32) -> i32 {
    if enabled > 1 {
        return -1003;
    }
    invoke(context, |io| io.set_tune(enabled != 0)).map_or_else(|error| error, |()| 0)
}
unsafe extern "C" fn clock(context: *mut c_void) -> u64 {
    invoke(context, |io| Ok(io.monotonic_ms())).unwrap_or(0)
}
unsafe extern "C" fn epoch(context: *mut c_void) -> u64 {
    invoke(context, |io| Ok(io.transmit_epoch())).unwrap_or(0)
}

/// One physical owner's remote state machine. The opaque handle is deliberately
/// not Send; create, service, retire and destroy it on that same worker.
pub struct RemoteSession {
    provider: Arc<RemoteProvider>,
    context: NonNull<c_void>,
}
impl RemoteSession {
    /// Project the creating worker's borrowed operations onto the installed C
    /// callback contract. No Rust trait or reference is retained by the provider.
    /// A dependency panic is contained before the C boundary and reported as a
    /// failed operation; the physical owner must retain closed admission.
    pub fn advance_with(
        &mut self,
        now: u64,
        activity: &raw::RptAdvRemoteActivity,
        io: &mut dyn OperatingIo,
    ) -> Result<(), CallError> {
        let mut borrowed = BorrowedBackend {
            io,
            panicked: false,
        };
        let backend = raw::RptAdvRemoteBackend {
            struct_size: size_of::<raw::RptAdvRemoteBackend>() as u32,
            abi_version: 1,
            context: (&mut borrowed as *mut BorrowedBackend<'_>).cast(),
            read_state: Some(read),
            apply_state: Some(apply),
            set_ptt: Some(ptt),
            monotonic_ms: Some(clock),
            transmit_epoch: Some(epoch),
            set_tune: Some(tune),
        };
        // SAFETY: borrowed operations and their exclusive context outlive this call;
        // the engine calls them serially and retains none of these stack pointers.
        let result = unsafe { self.advance(now, activity, &backend) };
        if borrowed.panicked {
            Err(CallError(-1003))
        } else {
            result
        }
    }
    /// Validate and copy a complete selected composition, without physical I/O.
    ///
    /// # Safety
    /// Every setup array/name span is readable through this synchronous call.
    pub unsafe fn open(
        provider: Arc<RemoteProvider>,
        setup: &raw::RptAdvRemoteSetup,
    ) -> Result<Self, CallError> {
        let mut context = std::ptr::null_mut();
        // SAFETY: the admitted descriptor copies all borrowed configuration.
        let code = unsafe { provider.api().create.unwrap()(setup, &mut context) };
        if code != 0 {
            if !context.is_null() {
                // SAFETY: a partial transferred owner belongs to this descriptor.
                unsafe { provider.api().destroy.unwrap()(context) };
            }
            return Err(CallError(code));
        }
        Ok(Self {
            provider,
            context: NonNull::new(context).ok_or(CallError(-1003))?,
        })
    }
    /// Read an immutable configured memory; never rewrite configuration.
    pub fn configured_memory(&self, selector: u32) -> Result<raw::RptAdvRemoteState, CallError> {
        // SAFETY: this C record contains only scalars.
        let mut state = unsafe { std::mem::zeroed() };
        // SAFETY: the unique live owner lends complete writable output.
        check(unsafe {
            self.provider.api().configured_memory.unwrap()(
                self.context.as_ptr(),
                selector,
                &mut state,
            )
        })?;
        Ok(state)
    }
    /// Submit one complete typed operation, without I/O or waiting.
    pub fn request(
        &mut self,
        request: &raw::RptAdvRemoteRequest,
        now: u64,
    ) -> Result<u64, CallError> {
        let mut id = 0;
        // SAFETY: request is a pointer-free complete record; owner access is serial.
        check(unsafe {
            self.provider.api().request.unwrap()(self.context.as_ptr(), request, now, &mut id)
        })?;
        Ok(id)
    }
    /// Service the portable engine using this worker's selected physical backend.
    ///
    /// # Safety
    /// Backend code/context remain live during the call, obey the installed
    /// contract and never reenter this session. No audio callback invokes it.
    pub unsafe fn advance(
        &mut self,
        now: u64,
        activity: &raw::RptAdvRemoteActivity,
        backend: &raw::RptAdvRemoteBackend,
    ) -> Result<(), CallError> {
        // SAFETY: the caller supplies synchronous, uniquely owned backend operations.
        check(unsafe {
            self.provider.api().advance.unwrap()(self.context.as_ptr(), now, activity, backend)
        })
    }
    /// Copy requested/applied/observed state without device I/O.
    pub fn snapshot(&self) -> Result<raw::RptAdvRemoteSnapshot, CallError> {
        // SAFETY: the snapshot is a scalar-only C record.
        let mut snapshot: raw::RptAdvRemoteSnapshot = unsafe { std::mem::zeroed() };
        snapshot.struct_size = size_of::<raw::RptAdvRemoteSnapshot>() as u32;
        // SAFETY: the live owner lends complete writable output.
        check(unsafe {
            self.provider.api().snapshot.unwrap()(self.context.as_ptr(), &mut snapshot)
        })?;
        Ok(snapshot)
    }
    /// Drain a bounded completion slot independently of slower presentation work.
    pub fn take_completion(&mut self) -> Result<Option<raw::RptAdvRemoteCompletion>, CallError> {
        // SAFETY: completion contains only scalar fields.
        let mut completion = unsafe { std::mem::zeroed() };
        // SAFETY: the uniquely retained owner lends complete output.
        let code = unsafe {
            self.provider.api().take_completion.unwrap()(self.context.as_ptr(), &mut completion)
        };
        if code == 1 {
            return Ok(None);
        }
        check(code)?;
        Ok(Some(completion))
    }
    /// Copy current scan state, or report no selected list.
    pub fn scan_snapshot(&self) -> Result<Option<raw::RptAdvRemoteScanSnapshot>, CallError> {
        // SAFETY: this output contains only scalars.
        let mut snapshot: raw::RptAdvRemoteScanSnapshot = unsafe { std::mem::zeroed() };
        snapshot.struct_size = size_of::<raw::RptAdvRemoteScanSnapshot>() as u32;
        // SAFETY: the uniquely retained owner lends complete writable output.
        let code = unsafe {
            self.provider.api().scan_snapshot.unwrap()(self.context.as_ptr(), &mut snapshot)
        };
        if code == 1 {
            return Ok(None);
        }
        check(code)?;
        Ok(Some(snapshot))
    }
    /// Reject further work; the final advance still attempts physical unkey.
    pub fn retire(&mut self) -> Result<(), CallError> {
        // SAFETY: retirement is serialized with every operation on this owner.
        check(unsafe { self.provider.api().retire.unwrap()(self.context.as_ptr()) })
    }
}
impl Drop for RemoteSession {
    fn drop(&mut self) {
        // SAFETY: the sole context is consumed before its retained DSO is released.
        unsafe { self.provider.api().destroy.unwrap()(self.context.as_ptr()) };
    }
}
