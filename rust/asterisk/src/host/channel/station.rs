//! Pin the registered technology and issue a one-use address during the public ast_request.
use super::*;
use crate::{
    UrpAstStationBindingDescriptor, UrpBoundStationRequest, UrpStationOwnershipDescriptor,
};
use std::{cell::RefCell, ffi::CString, sync::Arc};

const ADDRESS_PREFIX: &[u8] = b"\x1frpt-station/";
static NEXT_ADDRESS: AtomicU64 = AtomicU64::new(1);
thread_local! { static REQUEST:RefCell<Option<(CString,Arc<Plan>)>>=const {RefCell::new(None)}; }

/// An immutable preparation, its provider table and the exact registered driver.
pub(in crate::host) struct Plan {
    host: HostSnapshot,
    operations: *const UrpStationOwnershipDescriptor,
    request: UrpBoundStationRequest,
}
// SAFETY: the operations and preparation are immutable and pinned. Driver/channel mutation
// runs only on the existing Asterisk requester or serialized channel taskprocessor.
unsafe impl Send for Plan {}
// SAFETY: concurrent immutable reads do not transfer or mutate native channel ownership.
unsafe impl Sync for Plan {}

impl Drop for Plan {
    fn drop(&mut self) {
        // SAFETY: the registered Asterisk module retains the driver and its product DSO.
        // Our own adapter DSO stays loaded through the caller's release operation.
        unsafe {
            ((*self.host.descriptor)
                .native_configuration_destroy
                .expect("validated"))(self.request.prepared.cast_mut());
            ffi::__ast_module_unref(
                self.host.module,
                SOURCE_FILE.as_ptr(),
                0,
                SOURCE_FUNCTION.as_ptr(),
            );
        }
        ACTIVE_CHANNELS.fetch_sub(1, Ordering::AcqRel);
    }
}

/// A mounted channel already retains its own module reference and live driver.
/// This identity lets its control owner release obsolete preparation data after commit.
#[derive(Clone, Copy)]
pub(in crate::host) struct Mounted {
    port: [u8; 16],
    driver: usize,
    technology: usize,
    finish: unsafe extern "C" fn(*mut c_void, u32) -> i32,
}
impl Plan {
    pub(super) fn mounted(&self) -> Mounted {
        // SAFETY: this complete provider table is immutable while the registered module is pinned.
        Mounted {
            port: self.request.port_uuid,
            driver: self.host.driver as usize,
            technology: self.host.technology as usize,
            // SAFETY: the pinned module retains this validated callback throughout the channel lifetime.
            finish: unsafe { (*self.operations).channel_finish.expect("validated") },
        }
    }
    pub(super) unsafe fn reserve(
        &self,
        args: &UrpAstChannelReserveArgs,
        out: &mut *mut c_void,
    ) -> i32 {
        // SAFETY: the public requester verified this exact driver and retained its pinned plan.
        unsafe {
            ((*self.operations).channel_reserve.expect("validated"))(
                self.host.driver,
                &self.request,
                args,
                out,
            )
        }
    }
    pub(in crate::host) unsafe fn stage(&self, channel: *mut c_void) -> i32 {
        // SAFETY: this operation runs on the mounted channel's original taskprocessor.
        unsafe { ((*self.operations).channel_stage.expect("validated"))(channel, &self.request) }
    }
}

struct Table(UrpAstStationBindingDescriptor);
// SAFETY: the static table contains only process-lifetime constant data and function addresses.
unsafe impl Sync for Table {}
static TABLE: Table = Table(UrpAstStationBindingDescriptor {
    struct_size: size_of::<UrpAstStationBindingDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"usbradioplus.asterisk-station1".as_ptr(),
    prepare: Some(prepare),
    with_request: Some(with_request),
    stage: Some(stage),
    finish: Some(finish),
    release: Some(release),
});
/// Operations for acquiring this adapter's registered technology and typed station plan.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_asterisk_station_binding_descriptor()
-> *const UrpAstStationBindingDescriptor {
    &TABLE.0
}

/// Locate the additional operations inside the DSO which supplied the registered product table.
unsafe fn operations(
    product: *const UrpAstDescriptor,
) -> Option<*const UrpStationOwnershipDescriptor> {
    // SAFETY: dladdr writes one record for this still-loaded immutable product descriptor.
    let mut info = unsafe { std::mem::zeroed::<libc::Dl_info>() };
    // SAFETY: the product table is live and info is writable for this lookup.
    if unsafe { libc::dladdr(product.cast(), &mut info) } == 0 || info.dli_fname.is_null() {
        return None;
    }
    // SAFETY: this checks the already loaded object identified by dladdr; the registered module
    // keeps it resident after the transient loader reference is dropped.
    let library = unsafe { libc::dlopen(info.dli_fname, libc::RTLD_NOW | libc::RTLD_NOLOAD) };
    if library.is_null() {
        return None;
    }
    // SAFETY: the two exact getters use the signatures defined by the public product SDK.
    let (base, extra) = unsafe {
        (
            libc::dlsym(library, c"usbradioplus_product_descriptor_v1".as_ptr()),
            libc::dlsym(
                library,
                c"usbradioplus_station_ownership_descriptor".as_ptr(),
            ),
        )
    };
    let result = (|| {
        if base.is_null() || extra.is_null() {
            return None;
        }
        // SAFETY: exact symbol names and the product pointer check bind the getter pair to this instance.
        let (base, extra) = unsafe {
            (
                std::mem::transmute::<*mut c_void, unsafe extern "C" fn() -> *const UrpAstDescriptor>(
                    base,
                ),
                std::mem::transmute::<
                    *mut c_void,
                    unsafe extern "C" fn() -> *const UrpStationOwnershipDescriptor,
                >(extra),
            )
        };
        // SAFETY: the retained DSO exports this product getter with its public signature.
        if unsafe { base() } != product {
            return None;
        }
        // SAFETY: the verified product DSO owns the immutable station getter and returned table.
        let pointer = unsafe { extra() };
        if pointer.is_null()
            || !pointer.is_aligned()
            // SAFETY: this aligned, non-null ABI pointer supplies at least the leading size word.
            || (unsafe { pointer.cast::<u32>().read() } as usize)
                < size_of::<UrpStationOwnershipDescriptor>()
        {
            return None;
        }
        // SAFETY: full table size was checked before reading callback pointers.
        let table = unsafe { &*pointer };
        (table.abi_version == 1
            && !table.capability_name.is_null()
            // SAFETY: the complete table supplies a terminated name retained by the module.
            && unsafe { CStr::from_ptr(table.capability_name) }
                == c"usbradioplus.station-ownership1"
            && table.copy_prepared.is_some()
            && table.channel_reserve.is_some()
            && table.channel_stage.is_some()
            && table.channel_finish.is_some())
        .then_some(pointer)
    })();
    // SAFETY: this is the unique temporary NOLOAD reference; the registered module retains its DSO.
    unsafe { libc::dlclose(library) };
    result
}

unsafe extern "C" fn prepare(raw: *const UrpBoundStationRequest, out: *mut *mut c_void) -> i32 {
    // SAFETY: output is writable storage and the request provides an initial size word.
    let Some(out) = (unsafe { out.as_mut() }) else {
        return -1;
    };
    *out = ptr::null_mut();
    if raw.is_null()
        || !raw.is_aligned()
        // SAFETY: this aligned, non-null ABI pointer supplies at least the leading size word.
        || (unsafe { raw.cast::<u32>().read() } as usize) < size_of::<UrpBoundStationRequest>()
    {
        return -1;
    }
    // SAFETY: full readable request confirmed.
    let mut request = unsafe { *raw };
    if request.abi_version != 1 || request.candidate_identity == 0 || request.prepared.is_null() {
        return -1;
    }
    let _membership = live_channels()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let Some(host) =
        snapshot(technology_contract(ADVANCED_TECHNOLOGY).expect("built-in technology"))
    else {
        return -2;
    };
    if host.descriptor != request.product {
        return -2;
    }
    // SAFETY: membership prevents unregister until the module reference and driver count are acquired.
    let Some(ops) = (unsafe { operations(host.descriptor) }) else {
        return -2;
    };
    // SAFETY: the registered driver retains its validated product descriptor.
    if unsafe { (*host.descriptor).native_configuration_destroy }.is_none() {
        return -2;
    }
    let mut owned = ptr::null_mut();
    // SAFETY: matched preparation and writable output remain valid through the synchronous copy.
    let code = unsafe {
        ((*ops).copy_prepared.expect("validated"))(host.descriptor, request.prepared, &mut owned)
    };
    if code != 0 {
        return code;
    }
    if owned.is_null() {
        return -1;
    }
    request.prepared = owned;
    // SAFETY: module belongs to this still-registered driver; the plan releases the module exactly once.
    unsafe {
        ffi::__ast_module_ref(
            host.module,
            SOURCE_FILE.as_ptr(),
            0,
            SOURCE_FUNCTION.as_ptr(),
        )
    };
    ACTIVE_CHANNELS.fetch_add(1, Ordering::AcqRel);
    *out = Box::into_raw(Box::new(Arc::new(Plan {
        host,
        operations: ops,
        request,
    })))
    .cast();
    0
}
unsafe extern "C" fn release(raw: *mut c_void) {
    if !raw.is_null() {
        // SAFETY: caller transfers the unique wrapper returned from prepare.
        drop(unsafe { Box::from_raw(raw.cast::<Arc<Plan>>()) });
    }
}

unsafe extern "C" fn with_request(
    raw: *const c_void,
    context: *mut c_void,
    call: crate::UrpStationRequestChannel,
) -> i32 {
    // SAFETY: caller retains the dormant owner for the whole request.
    let (Some(plan), Some(call)) = (unsafe { raw.cast::<Arc<Plan>>().as_ref() }, call) else {
        return -1;
    };
    let Ok(sequence) = NEXT_ADDRESS.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| {
        old.checked_add(1)
    }) else {
        return -1;
    };
    let address =
        CString::new(format!("\x1frpt-station/{sequence:016x}")).expect("fixed ASCII address");
    let installed = REQUEST.with_borrow_mut(|slot| {
        if slot.is_some() {
            false
        } else {
            *slot = Some((address.clone(), plan.clone()));
            true
        }
    });
    if !installed {
        return -5;
    }
    // SAFETY: callback synchronously calls public ast_request while the ephemeral address is retained.
    let code = unsafe { call(context, address.as_ptr()) };
    let unclaimed = REQUEST.with_borrow_mut(Option::take).is_some();
    if code == 0 && unclaimed { -1 } else { code }
}

pub(super) fn claim(
    address: &[u8],
    host: &HostSnapshot,
    contract: TechnologyContract,
) -> Result<Option<Arc<Plan>>, ()> {
    if !address.starts_with(ADDRESS_PREFIX) {
        return Ok(None);
    }
    REQUEST.with_borrow_mut(|slot| {
        let Some((expected, plan)) = slot.as_ref() else {
            return Err(());
        };
        if expected.as_bytes() != address
            || contract.transport != URP_AST_TRANSPORT_RPT_ADVANCED
            || plan.host.driver != host.driver
            || plan.host.technology != host.technology
            || plan.host.descriptor != host.descriptor
        {
            return Err(());
        }
        Ok(slot.take().map(|(_, plan)| plan))
    })
}

fn mounted(owner: *mut c_void, operation: impl FnOnce(&Channel, &Mounted) -> i32) -> i32 {
    if owner.is_null() {
        return -1;
    }
    let membership = live_channels()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    for address in membership.iter() {
        // SAFETY: the membership lock excludes destruction of every listed channel.
        let channel = unsafe { &*(*address as *const Channel) };
        if channel.owner.load(Ordering::Acquire).cast::<c_void>() == owner {
            return channel
                .typed
                .as_ref()
                .map_or(-1, |current| operation(channel, current));
        }
    }
    -1
}
unsafe extern "C" fn stage(owner: *mut c_void, raw: *const c_void) -> i32 {
    // SAFETY: caller retains its dormant plan for this serialized stage.
    let Some(next) = (unsafe { raw.cast::<Arc<Plan>>().as_ref() }) else {
        return -1;
    };
    mounted(owner, |channel, current| {
        if next.request.port_uuid != current.port
            || next.host.driver as usize != current.driver
            || next.host.technology as usize != current.technology
        {
            return -1;
        }
        channel.control_status(ControlOperation::TypedStage(next.clone()))
    })
}
unsafe extern "C" fn finish(owner: *mut c_void, commit: u32) -> i32 {
    if commit > 1 {
        return -1;
    }
    mounted(owner, |channel, mounted| {
        channel.control_status(ControlOperation::TypedFinish(mounted.finish, commit == 1))
    })
}
