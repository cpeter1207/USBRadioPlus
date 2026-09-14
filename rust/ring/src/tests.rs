use super::*;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicUsize, Ordering};

static TRACKED_DESTROYS: AtomicUsize = AtomicUsize::new(0);
static PARTIAL_DESTROYS: AtomicUsize = AtomicUsize::new(0);
const RESULT_ADAPTER_ERROR: c_int = -3;

struct FakeRing {
    values: VecDeque<f32>,
    capacity: usize,
    fail: bool,
    last_reserve: u64,
    last_target: u64,
    track_destroy: bool,
    partial_destroy: bool,
}

unsafe extern "C" fn create(config: *const Config, output: *mut *mut c_void) -> c_int {
    // SAFETY: Tests invoke this through the validated wrapper ABI.
    let config = unsafe { &*config };
    let capacity = usize::try_from(config.capacity_samples).unwrap();
    // SAFETY: The wrapper supplies writable output storage.
    unsafe {
        *output = Box::into_raw(Box::new(FakeRing {
            values: VecDeque::with_capacity(capacity),
            capacity,
            fail: false,
            last_reserve: 0,
            last_target: 0,
            track_destroy: config.input_rate_hz == 8_000,
            partial_destroy: false,
        }))
        .cast();
    };
    RESULT_OK
}

unsafe extern "C" fn failing_create(_config: *const Config, output: *mut *mut c_void) -> c_int {
    // SAFETY: The wrapper supplies writable output storage.
    unsafe { *output = std::ptr::null_mut() };
    RESULT_ADAPTER_ERROR
}

unsafe extern "C" fn partial_failing_create(
    config: *const Config,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: Reuse the valid test constructor with the same live arguments.
    let _ = unsafe { create(config, output) };
    // SAFETY: `create` returned a live test handle through writable storage.
    unsafe { (*(*output).cast::<FakeRing>()).partial_destroy = true };
    RESULT_ADAPTER_ERROR
}

unsafe extern "C" fn destroy(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: Test handles originate from Box::into_raw in create.
        let ring = unsafe { Box::from_raw(handle.cast::<FakeRing>()) };
        if ring.track_destroy {
            TRACKED_DESTROYS.fetch_add(1, Ordering::Relaxed);
        }
        if ring.partial_destroy {
            PARTIAL_DESTROYS.fetch_add(1, Ordering::Relaxed);
        }
        drop(ring);
    }
}

unsafe extern "C" fn push_sample(handle: *mut c_void, sample: f32, accepted: *mut bool) -> c_int {
    // SAFETY: Wrapper supplies a live fake handle and writable result.
    let ring = unsafe { &mut *handle.cast::<FakeRing>() };
    if ring.fail {
        return RESULT_ADAPTER_ERROR;
    }
    let fits = ring.values.len() < ring.capacity;
    if fits {
        ring.values.push_back(sample);
    }
    // SAFETY: Wrapper supplies writable result storage.
    unsafe { *accepted = fits };
    RESULT_OK
}

unsafe extern "C" fn push(
    handle: *mut c_void,
    input: *const f32,
    samples: u64,
    accepted: *mut u64,
) -> c_int {
    // SAFETY: Wrapper supplies a live fake handle and exact input span.
    let ring = unsafe { &mut *handle.cast::<FakeRing>() };
    if ring.fail {
        return RESULT_ADAPTER_ERROR;
    }
    // SAFETY: Wrapper supplies a readable span of samples elements.
    let input = unsafe { std::slice::from_raw_parts(input, usize::try_from(samples).unwrap()) };
    let count = (ring.capacity - ring.values.len()).min(input.len());
    ring.values.extend(&input[..count]);
    // SAFETY: Wrapper supplies writable result storage.
    unsafe { *accepted = count as u64 };
    RESULT_OK
}

unsafe extern "C" fn render_sample(
    handle: *mut c_void,
    sample: *mut f32,
    target: u64,
    real: *mut bool,
) -> c_int {
    // SAFETY: Wrapper supplies a live fake handle and writable results.
    let ring = unsafe { &mut *handle.cast::<FakeRing>() };
    if ring.fail {
        return RESULT_ADAPTER_ERROR;
    }
    let value = ring.values.pop_front();
    ring.last_target = target;
    // SAFETY: Wrapper supplies writable result storage.
    unsafe {
        *sample = value.unwrap_or(-0.25);
        *real = value.is_some();
    }
    RESULT_OK
}

unsafe extern "C" fn render(
    handle: *mut c_void,
    output: *mut f32,
    samples: u64,
    reserve: u64,
    target: u64,
    real_samples: *mut u64,
) -> c_int {
    // SAFETY: Wrapper supplies a live fake handle and writable output span.
    let ring = unsafe { &mut *handle.cast::<FakeRing>() };
    if ring.fail {
        return RESULT_ADAPTER_ERROR;
    }
    // SAFETY: Wrapper supplies an exact writable span.
    let output =
        unsafe { std::slice::from_raw_parts_mut(output, usize::try_from(samples).unwrap()) };
    let mut real = 0;
    for sample in output {
        if let Some(value) = ring.values.pop_front() {
            *sample = value;
            real += 1;
        } else {
            *sample = -0.25;
        }
    }
    ring.last_reserve = reserve;
    ring.last_target = target;
    // SAFETY: Wrapper supplies writable result storage.
    unsafe { *real_samples = real };
    RESULT_OK
}

unsafe extern "C" fn observe(handle: *const c_void, output: *mut RawObservation) -> c_int {
    // SAFETY: Wrapper supplies a live fake handle and writable snapshot.
    let ring = unsafe { &*handle.cast::<FakeRing>() };
    // SAFETY: Wrapper supplies writable snapshot storage.
    unsafe {
        *output = RawObservation {
            struct_size: u32::try_from(size_of::<RawObservation>()).unwrap(),
            abi_version: ABI_VERSION,
            capacity_samples: ring.capacity as u64,
            available_samples: ring.values.len() as u64,
            reserve_samples: ring.last_reserve,
            filtered_occupancy_samples: ring.values.len() as u64,
            target_samples: ring.last_target,
            ratio_correction_ppm: 12,
            reserved: 0,
            discarded_samples: 1,
            missing_samples: 2,
            consecutive_shortfall_samples: 3,
            shortfall_average_milli: 4,
            adapter_error_count: 5,
        }
    };
    RESULT_OK
}

unsafe extern "C" fn incompatible_observe(
    handle: *const c_void,
    output: *mut RawObservation,
) -> c_int {
    // SAFETY: Reuse the valid observer with the same live arguments.
    let result = unsafe { observe(handle, output) };
    // SAFETY: The wrapper supplies writable snapshot storage.
    unsafe { (*output).abi_version = ABI_VERSION + 1 };
    result
}

unsafe extern "C" fn excessive_push(
    _handle: *mut c_void,
    _input: *const f32,
    samples: u64,
    accepted: *mut u64,
) -> c_int {
    // SAFETY: The wrapper supplies writable result storage.
    unsafe { *accepted = samples + 1 };
    RESULT_OK
}

unsafe extern "C" fn excessive_render(
    _handle: *mut c_void,
    _output: *mut f32,
    samples: u64,
    _reserve: u64,
    _target: u64,
    real_samples: *mut u64,
) -> c_int {
    // SAFETY: The wrapper supplies writable result storage.
    unsafe { *real_samples = samples + 1 };
    RESULT_OK
}

fn valid_descriptor() -> Descriptor {
    Descriptor {
        struct_size: u32::try_from(size_of::<Descriptor>()).unwrap(),
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY.as_ptr(),
        create: Some(create),
        destroy: Some(destroy),
        push_sample: Some(push_sample),
        push: Some(push),
        render_sample: Some(render_sample),
        render: Some(render),
        observe: Some(observe),
    }
}

fn provider(descriptor: &'static Descriptor) -> Result<RingProvider, RingError> {
    // SAFETY: Static test descriptors satisfy the documented lifetime.
    unsafe { RingProvider::from_raw_descriptor(std::ptr::from_ref(descriptor).cast()) }
}

#[test]
fn descriptor_validation_rejects_incompatible_prefixes() {
    assert_eq!(
        // SAFETY: Null is explicitly accepted and rejected.
        unsafe { RingProvider::from_raw_descriptor(std::ptr::null()) }.err(),
        Some(RingError::IncompatibleAdapter)
    );
    for descriptor in [
        Descriptor {
            struct_size: u32::try_from(REQUIRED_DESCRIPTOR_SIZE - 1).unwrap(),
            ..valid_descriptor()
        },
        Descriptor {
            abi_version: ABI_VERSION + 1,
            ..valid_descriptor()
        },
        Descriptor {
            capability_name: std::ptr::null(),
            ..valid_descriptor()
        },
        Descriptor {
            capability_name: c"wrong".as_ptr(),
            ..valid_descriptor()
        },
        Descriptor {
            create: None,
            ..valid_descriptor()
        },
        Descriptor {
            destroy: None,
            ..valid_descriptor()
        },
        Descriptor {
            push_sample: None,
            ..valid_descriptor()
        },
        Descriptor {
            push: None,
            ..valid_descriptor()
        },
        Descriptor {
            render_sample: None,
            ..valid_descriptor()
        },
        Descriptor {
            render: None,
            ..valid_descriptor()
        },
        Descriptor {
            observe: None,
            ..valid_descriptor()
        },
    ] {
        assert_eq!(
            provider(Box::leak(Box::new(descriptor))).err(),
            Some(RingError::IncompatibleAdapter)
        );
    }
}

#[test]
fn setup_validates_capacity_rate_ratio_and_adapter_failure() {
    let valid_provider = provider(Box::leak(Box::new(valid_descriptor()))).unwrap();
    assert_eq!(
        valid_provider
            .prepare(511, 48_000, 48_000, ConversionQuality::Best)
            .err(),
        Some(RingError::InvalidArgument)
    );
    assert_eq!(
        valid_provider
            .prepare(512, 0, 48_000, ConversionQuality::Best)
            .err(),
        Some(RingError::InvalidArgument)
    );
    assert_eq!(
        valid_provider
            .prepare(512, 48_000, 0, ConversionQuality::Best)
            .err(),
        Some(RingError::InvalidArgument)
    );
    assert_eq!(
        valid_provider
            .prepare(512, 1, 48_000, ConversionQuality::Best)
            .err(),
        Some(RingError::InvalidArgument)
    );
    assert_eq!(
        valid_provider
            .prepare(512, 48_000, 1, ConversionQuality::Fastest)
            .err(),
        Some(RingError::InvalidArgument)
    );
    let descriptor = Box::leak(Box::new(Descriptor {
        create: Some(failing_create),
        ..valid_descriptor()
    }));
    assert_eq!(
        provider(descriptor)
            .unwrap()
            .prepare(512, 48_000, 48_000, ConversionQuality::Best)
            .err(),
        Some(RingError::AdapterFailure)
    );
    let descriptor = Box::leak(Box::new(Descriptor {
        create: Some(partial_failing_create),
        ..valid_descriptor()
    }));
    let before = PARTIAL_DESTROYS.load(Ordering::Relaxed);
    assert_eq!(
        provider(descriptor)
            .unwrap()
            .prepare(512, 48_000, 48_000, ConversionQuality::Best)
            .err(),
        Some(RingError::AdapterFailure)
    );
    assert_eq!(PARTIAL_DESTROYS.load(Ordering::Relaxed), before + 1);
}

#[test]
fn split_endpoints_forward_blocks_and_observation() {
    TRACKED_DESTROYS.store(0, Ordering::Relaxed);
    let ring = provider(Box::leak(Box::new(valid_descriptor())))
        .unwrap()
        .prepare(512, 8_000, 48_000, ConversionQuality::Best)
        .unwrap();
    let (mut producer, mut consumer) = ring.split();
    assert_eq!(producer.push(&[0.1, 0.2, 0.3, 0.4]).unwrap(), 4);
    let mut output = [0.0; 5];
    assert_eq!(consumer.render(&mut output, 20, 60).unwrap(), 4);
    assert_eq!(
        output.map(f32::to_bits),
        [0.1, 0.2, 0.3, 0.4, -0.25].map(f32::to_bits)
    );
    let observed = consumer.observe().unwrap();
    assert_eq!(observed.capacity_samples, 512);
    assert_eq!(observed.available_samples, 0);
    assert_eq!(observed.reserve_samples, 20);
    assert_eq!(observed.target_samples, 60);
    assert_eq!(observed.ratio_correction_ppm, 12);
    assert_eq!(observed.discarded_samples, 1);
    assert_eq!(observed.missing_samples, 2);
    assert_eq!(observed.consecutive_shortfall_samples, 3);
    assert_eq!(observed.shortfall_average_milli, 4);
    assert_eq!(observed.adapter_error_count, 5);
    assert_eq!(consumer.observe().unwrap(), observed);
    drop(producer);
    assert_eq!(TRACKED_DESTROYS.load(Ordering::Relaxed), 0);
    drop(consumer);
    assert_eq!(TRACKED_DESTROYS.load(Ordering::Relaxed), 1);
}

#[test]
fn endpoint_failures_map_without_panicking() {
    let ring = provider(Box::leak(Box::new(valid_descriptor())))
        .unwrap()
        .prepare(512, 48_000, 48_000, ConversionQuality::Medium)
        .unwrap();
    let (mut producer, mut consumer) = ring.split();
    // SAFETY: The test owns both endpoints and no operation is concurrent.
    unsafe { (*producer.inner.handle.as_ptr().cast::<FakeRing>()).fail = true };
    assert_eq!(producer.push(&[0.0]), Err(RingError::AdapterFailure));
    assert_eq!(
        consumer.render(&mut [0.0], 0, 0),
        Err(RingError::AdapterFailure)
    );
    assert_eq!(
        RingError::IncompatibleAdapter.to_string(),
        "incompatible PCM-ring adapter"
    );
    assert_eq!(
        RingError::InvalidArgument.to_string(),
        "invalid PCM-ring argument"
    );
    assert_eq!(
        RingError::NoMemory.to_string(),
        "unable to allocate PCM ring"
    );
    assert_eq!(
        RingError::AdapterFailure.to_string(),
        "PCM-ring sample-rate adapter failed"
    );
    assert_eq!(
        map_result(RESULT_INVALID_ARGUMENT),
        Err(RingError::InvalidArgument)
    );
    assert_eq!(map_result(RESULT_NO_MEMORY), Err(RingError::NoMemory));
    assert_eq!(map_result(99), Err(RingError::AdapterFailure));
}

#[test]
fn invalid_adapter_results_are_rejected() {
    let descriptor = Box::leak(Box::new(Descriptor {
        observe: Some(incompatible_observe),
        ..valid_descriptor()
    }));
    let (_producer, consumer) = provider(descriptor)
        .unwrap()
        .prepare(512, 48_000, 48_000, ConversionQuality::Best)
        .unwrap()
        .split();
    assert_eq!(consumer.observe(), Err(RingError::IncompatibleAdapter));

    let descriptor = Box::leak(Box::new(Descriptor {
        push: Some(excessive_push),
        ..valid_descriptor()
    }));
    let (mut producer, _consumer) = provider(descriptor)
        .unwrap()
        .prepare(512, 48_000, 48_000, ConversionQuality::Best)
        .unwrap()
        .split();
    assert_eq!(producer.push(&[0.0]), Err(RingError::AdapterFailure));

    let descriptor = Box::leak(Box::new(Descriptor {
        render: Some(excessive_render),
        ..valid_descriptor()
    }));
    let (_producer, mut consumer) = provider(descriptor)
        .unwrap()
        .prepare(512, 48_000, 48_000, ConversionQuality::Best)
        .unwrap()
        .split();
    assert_eq!(
        consumer.render(&mut [0.0], 0, 0),
        Err(RingError::AdapterFailure)
    );
}
