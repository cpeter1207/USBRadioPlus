//! Unique dynamic portable-engine context. No tune or scan policy is duplicated here.

use crate::{CallError, RemoteProvider, hamlib::check, raw};
use std::{ffi::c_void, mem::size_of, ptr::NonNull, sync::Arc};

/// One physical owner's remote state machine. The opaque handle is deliberately
/// not Send; create, service, retire and destroy it on that same worker.
pub struct RemoteSession {
    provider: Arc<RemoteProvider>,
    context: NonNull<c_void>,
}
impl RemoteSession {
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
