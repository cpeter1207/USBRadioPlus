//! Exact external AST link calls shared by loader and concrete link tests.

use super::*;

struct IteratorState {
    channels: std::vec::IntoIter<usize>,
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_name(
    channel: *const ffi::ast_channel,
) -> *const c_char {
    // SAFETY: the serialized fixture retains this owner for the call.
    unsafe { (*channel.cast::<FakeChannel>()).name.as_ptr() }
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_iterator_all_new() -> *mut ffi::ast_channel_iterator {
    with_state(|state| {
        if state.iterator_allocation_fails {
            return ptr::null_mut();
        }
        state.iterator_allocations += 1;
        let mut channels: Vec<_> = state.channels.keys().copied().collect();
        channels.sort_unstable();
        Box::into_raw(Box::new(IteratorState {
            channels: channels.into_iter(),
        }))
        .cast()
    })
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_iterator_next(
    iterator: *mut ffi::ast_channel_iterator,
) -> *mut ffi::ast_channel {
    // SAFETY: all_new returns this owned iterator, retained until destroy.
    unsafe { (*iterator.cast::<IteratorState>()).channels.next() }
        .map_or(ptr::null_mut(), |address| address as *mut ffi::ast_channel)
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_iterator_destroy(
    iterator: *mut ffi::ast_channel_iterator,
) -> *mut ffi::ast_channel_iterator {
    // SAFETY: consumes exactly the allocation returned by all_new.
    drop(unsafe { Box::from_raw(iterator.cast::<IteratorState>()) });
    with_state(|state| state.iterator_frees += 1);
    ptr::null_mut()
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_appl(
    channel: *const ffi::ast_channel,
) -> *const c_char {
    // SAFETY: the serialized fixture retains this owner for the call.
    unsafe { (*channel.cast::<FakeChannel>()).application.as_ptr() }
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_data(
    channel: *const ffi::ast_channel,
) -> *const c_char {
    // SAFETY: the serialized fixture retains this owner for the call.
    unsafe { (*channel.cast::<FakeChannel>()).data.as_ptr() }
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_rawreadformat(
    channel: *mut ffi::ast_channel,
) -> *mut ffi::ast_format {
    // SAFETY: the format pointer was installed on this live fixture owner.
    unsafe { (*channel.cast::<FakeChannel>()).formats[1] as *mut ffi::ast_format }
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn __ast_datastore_alloc(
    info: *const ffi::ast_datastore_info,
    uid: *const c_char,
    module: *mut ffi::ast_module,
    _: *const c_char,
    _: c_int,
    _: *const c_char,
) -> *mut ffi::ast_datastore {
    with_state(|state| {
        if state.datastore_allocation_fails {
            return ptr::null_mut();
        }
        state.datastore_allocations += 1;
        // SAFETY: the C datastore accepts its empty zero representation.
        let mut datastore: Box<ffi::ast_datastore> = Box::new(unsafe { std::mem::zeroed() });
        assert!(uid.is_null(), "the link host uses unnamed datastores");
        datastore.info = info;
        datastore.mod_ = module;
        Box::into_raw(datastore)
    })
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_datastore_free(datastore: *mut ffi::ast_datastore) -> c_int {
    // SAFETY: the caller transfers the allocation and registered payload.
    let datastore = unsafe { Box::from_raw(datastore) };
    // SAFETY: allocation stored the host's process-lifetime descriptor.
    if let Some(destroy) = unsafe { datastore.info.as_ref() }.and_then(|info| info.destroy) {
        // SAFETY: the payload follows its registered destructor contract.
        unsafe { destroy(datastore.data) };
    }
    with_state(|state| state.datastore_frees += 1);
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_datastore_find(
    channel: *mut ffi::ast_channel,
    info: *const ffi::ast_datastore_info,
    uid: *const c_char,
) -> *mut ffi::ast_datastore {
    assert!(uid.is_null());
    // SAFETY: the fixture retains this channel and its optional datastore.
    unsafe {
        let datastore = (*channel.cast::<FakeChannel>()).datastore as *mut ffi::ast_datastore;
        if !datastore.is_null() && (*datastore).info == info {
            datastore
        } else {
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_datastore_add(
    channel: *mut ffi::ast_channel,
    datastore: *mut ffi::ast_datastore,
) -> c_int {
    // SAFETY: the caller owns both pointers and transfers the datastore attachment.
    unsafe { (*channel.cast::<FakeChannel>()).datastore = datastore as usize };
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_channel_datastore_remove(
    channel: *mut ffi::ast_channel,
    datastore: *mut ffi::ast_datastore,
) -> c_int {
    // SAFETY: the caller owns this channel's one attached datastore.
    unsafe {
        assert_eq!(
            (*channel.cast::<FakeChannel>()).datastore,
            datastore as usize
        );
        (*channel.cast::<FakeChannel>()).datastore = 0;
    }
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_audiohook_init(
    hook: *mut ffi::ast_audiohook,
    kind: ffi::ast_audiohook_type,
    source: *const c_char,
    flags: ffi::ast_audiohook_init_flags,
) -> c_int {
    let result = with_state(|state| {
        state.audiohook_calls.push(("init", hook as usize));
        state.audiohook_init_result
    });
    if result == 0 {
        // SAFETY: the caller supplies writable pinned hook storage.
        unsafe {
            (*hook).type_ = kind;
            (*hook).source = source;
            (*hook).init_flags = flags;
            (*hook).status = ffi::AST_AUDIOHOOK_STATUS_NEW;
        }
    }
    result
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_audiohook_attach(
    channel: *mut ffi::ast_channel,
    hook: *mut ffi::ast_audiohook,
) -> c_int {
    let result = with_state(|state| {
        state.audiohook_calls.push(("attach", hook as usize));
        state.audiohook_attach_result
    });
    if result == 0 {
        // SAFETY: both fixture owner and initialized hook remain live.
        unsafe {
            (*channel.cast::<FakeChannel>()).audiohook = hook as usize;
            (*hook).status = ffi::AST_AUDIOHOOK_STATUS_RUNNING;
        }
    }
    result
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_audiohook_detach(hook: *mut ffi::ast_audiohook) -> c_int {
    with_state(|state| {
        state.audiohook_calls.push(("detach", hook as usize));
        for channel in state.channels.values_mut() {
            if channel.audiohook == hook as usize {
                channel.audiohook = 0;
            }
        }
    });
    // SAFETY: the caller retains the hook through detachment.
    unsafe { (*hook).status = ffi::AST_AUDIOHOOK_STATUS_DONE };
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn ast_audiohook_destroy(hook: *mut ffi::ast_audiohook) -> c_int {
    with_state(|state| state.audiohook_calls.push(("destroy", hook as usize)));
    // SAFETY: the caller retains this initialized hook until destruction completes.
    unsafe { (*hook).status = ffi::AST_AUDIOHOOK_STATUS_DONE };
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn __ast_pthread_mutex_lock(
    _: *const c_char,
    _: c_int,
    _: *const c_char,
    _: *const c_char,
    mutex: *mut ffi::ast_mutex_t,
) -> c_int {
    with_state(|state| state.audiohook_calls.push(("lock", mutex as usize)));
    0
}

#[unsafe(no_mangle)]
pub(super) unsafe extern "C" fn __ast_pthread_mutex_unlock(
    _: *const c_char,
    _: c_int,
    _: *const c_char,
    _: *const c_char,
    mutex: *mut ffi::ast_mutex_t,
) -> c_int {
    with_state(|state| state.audiohook_calls.push(("unlock", mutex as usize)));
    0
}
