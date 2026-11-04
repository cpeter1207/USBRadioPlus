//! Independent diagnostics retain counters, never a mutable graph/provider borrow.

use super::*;
use std::sync::{
    Arc, Barrier,
    atomic::{AtomicBool, AtomicUsize, Ordering},
};

#[repr(C)]
struct ObserverTail {
    text: Option<
        unsafe extern "C" fn(
            *mut c_void,
            Option<UrpNativeProcessingDiagnostic>,
            *mut c_void,
        ) -> c_int,
    >,
    create: Option<unsafe extern "C" fn(*mut c_void, *mut *mut c_void) -> c_int>,
    read: Option<unsafe extern "C" fn(*const c_void, *mut UrpAstLinkObservation) -> c_int>,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

fn observer_api() -> &'static ObserverTail {
    let offset = std::mem::offset_of!(UrpAstDescriptor, native_status_text)
        + size_of::<Option<unsafe extern "C" fn()>>();
    assert!(
        DESCRIPTOR.struct_size as usize >= offset + size_of::<ObserverTail>(),
        "independent observer and native processing text descriptor tail is missing"
    );
    // SAFETY: the advertised immutable table now covers this exact appended C prefix.
    unsafe {
        &*ptr::from_ref(product_descriptor())
            .cast::<u8>()
            .add(offset)
            .cast::<ObserverTail>()
    }
}

fn observe(api: &ObserverTail, owner: *const c_void) -> (u64, u64, u64) {
    let mut output = UrpAstLinkObservation {
        struct_size: size_of::<UrpAstLinkObservation>() as u32,
        abi_version: ABI_VERSION,
        ..UrpAstLinkObservation::default()
    };
    // SAFETY: each caller retains its independent observer through the synchronous read.
    assert_eq!(unsafe { api.read.unwrap()(owner, &mut output) }, URP_AST_OK);
    (
        output.processed_blocks,
        output.bypassed_blocks,
        output.failed_blocks,
    )
}

struct AlternatingGraph {
    calls: usize,
    entered: Arc<Barrier>,
    release: Arc<Barrier>,
    destroyed: Arc<AtomicUsize>,
}

impl usbradioplus_asl3::ExactRateGraph for AlternatingGraph {
    fn process_exact(&mut self, input: &[f32], output: &mut [f32]) -> bool {
        self.calls += 1;
        if self.calls == 1 {
            self.entered.wait();
            self.release.wait();
        }
        output.copy_from_slice(input);
        self.calls % 2 == 1
    }
}

impl Drop for AlternatingGraph {
    fn drop(&mut self) {
        self.destroyed.fetch_add(1, Ordering::Relaxed);
    }
}

#[test]
fn observers_read_existing_counters_during_audio_and_after_graph_destruction() {
    let api = observer_api();
    let entered = Arc::new(Barrier::new(2));
    let release = Arc::new(Barrier::new(2));
    let destroyed = Arc::new(AtomicUsize::new(0));
    let link = Box::into_raw(Box::new(LinkHandle {
        processing: PreparedLink::new(
            48_000,
            1,
            Box::new(AlternatingGraph {
                calls: 0,
                entered: entered.clone(),
                release: release.clone(),
                destroyed: destroyed.clone(),
            }),
        )
        .unwrap(),
        description: String::new(),
        sample_rate_hz: 48_000,
        maximum_frame_count: 1,
    }))
    .cast::<c_void>();
    let mut first = ptr::null_mut();
    let mut retained = ptr::null_mut();
    // SAFETY: both observers are created while the graph has its sole quiescent owner.
    unsafe {
        assert_eq!(api.create.unwrap()(link, &mut first), URP_AST_OK);
        assert_eq!(api.create.unwrap()(link, &mut retained), URP_AST_OK);
    }
    assert!(!first.is_null() && !retained.is_null());
    assert_ne!(first, retained);
    assert_eq!(observe(api, retained), (0, 0, 0));
    let done = AtomicBool::new(false);
    let link_address = link as usize;
    std::thread::scope(|scope| {
        let done = &done;
        scope.spawn(move || {
            let link = link_address as *mut c_void;
            // SAFETY: this thread exclusively owns PCM processing and destroys its quiesced graph.
            unsafe {
                for _ in 0..4_000 {
                    assert_eq!(
                        product_descriptor().link_process_f32.unwrap()(
                            link,
                            URP_AST_LINK_DIRECTION_READ,
                            48_000,
                            &mut 0.25,
                            1
                        ),
                        URP_AST_OK
                    );
                    assert_eq!(
                        product_descriptor().link_process.unwrap()(
                            link,
                            URP_AST_LINK_DIRECTION_WRITE,
                            48_000,
                            &mut 1,
                            1
                        ),
                        URP_AST_OK
                    );
                }
                product_descriptor().link_destroy.unwrap()(link);
            }
            done.store(true, Ordering::Release);
        });
        entered.wait();
        // The mutable graph callback is parked here; observer reads must not borrow it.
        assert_eq!(observe(api, retained), (0, 0, 0));
        // SAFETY: this independent lease has no reads in flight; the graph retains the counters.
        unsafe { api.destroy.unwrap()(first) };
        release.wait();
        let mut previous = (0, 0, 0);
        while !done.load(Ordering::Acquire) {
            let current = observe(api, retained);
            assert!(current.0 >= previous.0 && current.1 >= previous.1 && current.2 >= previous.2);
            previous = current;
            std::thread::yield_now();
        }
    });
    assert_eq!(destroyed.load(Ordering::Relaxed), 1);
    assert_eq!(observe(api, retained), (2_000, 4_000, 2_000));
    // SAFETY: the retained observer remains independent after graph/provider teardown.
    unsafe { api.destroy.unwrap()(retained) };
}

#[test]
fn observation_tail_rejects_invalid_handles_and_records_without_writing_them() {
    let api = observer_api();
    let mut owner = ptr::dangling_mut();
    unsafe extern "C" fn ignore_text(_: *mut c_void, _: *const u8, _: u32) {}
    // SAFETY: invalid null inputs are admitted only for rejection and writable output is retained.
    unsafe {
        assert_eq!(
            api.create.unwrap()(ptr::null_mut(), &mut owner),
            URP_AST_INVALID_ARGUMENT
        );
        assert!(owner.is_null());
        assert_eq!(
            api.create.unwrap()(ptr::null_mut(), ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.read.unwrap()(ptr::null(), ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        api.destroy.unwrap()(ptr::null_mut());
        assert_eq!(
            api.text.unwrap()(ptr::null_mut(), None, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            api.text.unwrap()(ptr::null_mut(), Some(ignore_text), ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
    }
    let link = Box::into_raw(Box::new(LinkHandle {
        processing: PreparedLink::new(
            48_000,
            1,
            Box::new(AlternatingGraph {
                calls: 0,
                entered: Arc::new(Barrier::new(1)),
                release: Arc::new(Barrier::new(1)),
                destroyed: Arc::new(AtomicUsize::new(0)),
            }),
        )
        .unwrap(),
        description: String::new(),
        sample_rate_hz: 48_000,
        maximum_frame_count: 1,
    }))
    .cast();
    // SAFETY: this graph stays quiescent, and each record remains fully allocated for prefix checks.
    unsafe {
        assert_eq!(api.create.unwrap()(link, &mut owner), URP_AST_OK);
        assert_eq!(
            api.read.unwrap()(owner, ptr::null_mut()),
            URP_AST_INVALID_ARGUMENT
        );
        for (size, abi) in [
            (0, 1),
            (size_of::<UrpAstLinkObservation>() as u32 - 1, 1),
            (size_of::<UrpAstLinkObservation>() as u32, 99),
        ] {
            let mut output = UrpAstLinkObservation {
                struct_size: size,
                abi_version: abi,
                processed_blocks: 77,
                ..UrpAstLinkObservation::default()
            };
            assert_eq!(
                api.read.unwrap()(owner, &mut output),
                URP_AST_INCOMPATIBLE_ABI
            );
            assert_eq!(output.processed_blocks, 77);
        }
        product_descriptor().link_destroy.unwrap()(link);
        api.destroy.unwrap()(owner);
    }
}
