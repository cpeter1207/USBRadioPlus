use super::*;
use std::sync::Mutex;
use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

static TEST_LOCK: Mutex<()> = Mutex::new(());
static RESULT: AtomicI32 = AtomicI32::new(RESULT_OK);
static MODE: AtomicU32 = AtomicU32::new(0);
static DESTROYS: AtomicU32 = AtomicU32::new(0);

struct FakeConverter;

unsafe extern "C" fn create(
    input_rate: u32,
    output_rate: u32,
    maximum_input: u32,
    maximum_output: u32,
    output: *mut *mut OpaqueConverter,
) -> c_int {
    if MODE.load(Ordering::Relaxed) == 4 {
        // SAFETY: the tested wrapper supplies writable handle storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeConverter)).cast::<OpaqueConverter>();
        }
        return -2;
    }
    let result = RESULT.load(Ordering::Relaxed);
    if result != RESULT_OK {
        return result;
    }
    assert_eq!(
        (input_rate, output_rate, maximum_input, maximum_output),
        (48000, 8000, 3, 2)
    );
    if MODE.load(Ordering::Relaxed) != 1 {
        // SAFETY: the tested wrapper supplies writable handle storage.
        unsafe {
            *output = Box::into_raw(Box::new(FakeConverter)).cast::<OpaqueConverter>();
        }
    }
    result
}

unsafe extern "C" fn reset(_converter: *mut OpaqueConverter) -> c_int {
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn queued_input(_: *mut OpaqueConverter, frames: *mut u32) -> c_int {
    // SAFETY: the wrapper supplies writable count storage.
    unsafe { *frames = 1 };
    RESULT.load(Ordering::Relaxed)
}

unsafe extern "C" fn process(
    _converter: *mut OpaqueConverter,
    input: *const f32,
    input_frames: u32,
    output: *mut f32,
    output_capacity: u32,
    ratio: f64,
    input_used: *mut u32,
    output_generated: *mut u32,
) -> c_int {
    let result = RESULT.load(Ordering::Relaxed);
    if result != RESULT_OK {
        return result;
    }
    let count = input_frames.min(output_capacity);
    // SAFETY: the tested wrapper supplies buffers of the declared lengths.
    unsafe {
        for index in 0..count as usize {
            *output.add(index) = *input.add(index) * ratio as f32;
        }
        *input_used = if MODE.load(Ordering::Relaxed) == 2 {
            input_frames.saturating_add(1)
        } else {
            count
        };
        *output_generated = if MODE.load(Ordering::Relaxed) == 3 {
            output_capacity.saturating_add(1)
        } else {
            count
        };
    }
    result
}

unsafe extern "C" fn destroy(converter: *mut OpaqueConverter) {
    // SAFETY: this is the unique allocation returned by `create`.
    drop(unsafe { Box::from_raw(converter.cast::<FakeConverter>()) });
    DESTROYS.fetch_add(1, Ordering::Relaxed);
}

fn descriptor() -> Descriptor {
    Descriptor {
        struct_size: size_of::<Descriptor>() as u32,
        abi_version: ABI_VERSION,
        capability_name: CAPABILITY.as_ptr(),
        create: Some(create),
        reset: Some(reset),
        process: Some(process),
        destroy: Some(destroy),
        queued_input: Some(queued_input),
        converter_output_delay: Some(queued_input),
    }
}

fn adapter(raw: &Descriptor) -> SampleRateAdapter {
    // SAFETY: the local descriptor and functions live for the test.
    unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(raw).cast()).unwrap() }
}

fn reset_fixture() {
    RESULT.store(RESULT_OK, Ordering::Relaxed);
    MODE.store(0, Ordering::Relaxed);
    DESTROYS.store(0, Ordering::Relaxed);
}

#[test]
fn obsolete_unbounded_adapter_is_rejected_before_conversion() {
    let mut raw = descriptor();
    raw.abi_version = 1;
    assert!(matches!(
        // SAFETY: the complete descriptor is readable; its old ABI must be rejected.
        unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) },
        Err(SampleRateError::IncompatibleAdapter)
    ));
}

#[test]
fn persistent_converter_uses_prepared_rates_and_counts_accepted_input() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_fixture();
    let raw = descriptor();
    let mut converter = adapter(&raw).create(48000, 8000, 3, 2).unwrap();
    let input = [0.25, -0.5, 1.0];
    let mut output = [0.0; 2];
    assert_eq!(
        converter.process(&input, &mut output, 1.0 / 6.0).unwrap(),
        ProcessResult {
            input_used: 2,
            output_generated: 2,
        }
    );
    assert_eq!(output, [0.25 / 6.0, -0.5 / 6.0]);
    drop(converter);
    assert_eq!(DESTROYS.load(Ordering::Relaxed), 1);
}

#[test]
fn descriptor_and_adapter_errors_fail_safely() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_fixture();
    // SAFETY: null deliberately tests descriptor validation.
    let result = unsafe { SampleRateAdapter::from_raw(std::ptr::null()) };
    assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    let mut raw = descriptor();
    raw.struct_size = 0;
    // SAFETY: the descriptor is readable but deliberately incompatible.
    let result = unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    raw = descriptor();
    raw.abi_version = 1;
    // SAFETY: the descriptor is readable but deliberately incompatible.
    let result = unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    raw = descriptor();
    raw.capability_name = std::ptr::null();
    // SAFETY: the descriptor is readable but deliberately incompatible.
    let result = unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    raw = descriptor();
    raw.capability_name = c"wrong".as_ptr();
    // SAFETY: the descriptor is readable but deliberately incompatible.
    let result = unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) };
    assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    for remove in [
        |value: &mut Descriptor| value.create = None,
        |value: &mut Descriptor| value.reset = None,
        |value: &mut Descriptor| value.process = None,
        |value: &mut Descriptor| value.destroy = None,
        |value: &mut Descriptor| value.queued_input = None,
        |value: &mut Descriptor| value.converter_output_delay = None,
    ] {
        raw = descriptor();
        remove(&mut raw);
        // SAFETY: the descriptor is readable but deliberately incomplete.
        let result = unsafe { SampleRateAdapter::from_raw(std::ptr::from_ref(&raw).cast()) };
        assert!(matches!(result, Err(SampleRateError::IncompatibleAdapter)));
    }

    let raw = descriptor();
    RESULT.store(-1, Ordering::Relaxed);
    assert!(matches!(
        adapter(&raw).create(48000, 8000, 3, 2),
        Err(SampleRateError::InvalidArgument)
    ));
    RESULT.store(RESULT_OK, Ordering::Relaxed);
    MODE.store(1, Ordering::Relaxed);
    assert!(matches!(
        adapter(&raw).create(48000, 8000, 3, 2),
        Err(SampleRateError::AdapterFailure)
    ));
    MODE.store(4, Ordering::Relaxed);
    let before = DESTROYS.load(Ordering::Relaxed);
    assert!(matches!(
        adapter(&raw).create(48000, 8000, 3, 2),
        Err(SampleRateError::Conversion)
    ));
    assert_eq!(DESTROYS.load(Ordering::Relaxed), before + 1);
    MODE.store(0, Ordering::Relaxed);
    for (code, expected) in [
        (-1, SampleRateError::InvalidArgument),
        (-2, SampleRateError::Conversion),
        (-3, SampleRateError::Unsupported),
        (-99, SampleRateError::AdapterFailure),
    ] {
        assert_eq!(map_result(code), Err(expected));
        assert!(!expected.to_string().is_empty());
    }
    assert!(!SampleRateError::IncompatibleAdapter.to_string().is_empty());
}

#[test]
fn process_validates_ratios_and_returned_counts() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_fixture();
    let raw = descriptor();
    let mut converter = adapter(&raw).create(48000, 8000, 3, 2).unwrap();
    let input = [0.0; 2];
    let mut output = [0.0; 2];
    for ratio in [0.0, 1.0, f64::NAN, f64::INFINITY, 0.998 / 6.0, 1.002 / 6.0] {
        assert_eq!(
            converter.process(&input, &mut output, ratio),
            Err(SampleRateError::InvalidArgument)
        );
    }
    assert!(
        converter
            .process(&input, &mut output, (1.0 / 6.0) * 0.999)
            .is_ok()
    );
    assert!(
        converter
            .process(&input, &mut output, (1.0 / 6.0) * 1.001)
            .is_ok()
    );
    MODE.store(2, Ordering::Relaxed);
    assert_eq!(
        converter.process(&input, &mut output, 1.0 / 6.0),
        Err(SampleRateError::AdapterFailure)
    );
    MODE.store(3, Ordering::Relaxed);
    assert_eq!(
        converter.process(&input, &mut output, 1.0 / 6.0),
        Err(SampleRateError::AdapterFailure)
    );
    MODE.store(0, Ordering::Relaxed);
    RESULT.store(-2, Ordering::Relaxed);
    assert_eq!(
        converter.process(&input, &mut output, 1.0 / 6.0),
        Err(SampleRateError::Conversion)
    );
    assert_eq!(
        checked_frame_count(u32::MAX as usize + 1),
        Err(SampleRateError::InvalidArgument)
    );
    RESULT.store(RESULT_OK, Ordering::Relaxed);
    assert_eq!(
        converter.process(&[0.0; 4], &mut output, 1.0 / 6.0),
        Err(SampleRateError::InvalidArgument)
    );
    assert_eq!(
        converter.process(&input, &mut [0.0; 3], 1.0 / 6.0),
        Err(SampleRateError::InvalidArgument)
    );
    for (input_rate, output_rate, maximum_input, maximum_output) in [
        (0, 8000, 3, 2),
        (48000, 0, 3, 2),
        (48000, 8000, 0, 2),
        (48000, 8000, 3, 0),
    ] {
        assert!(matches!(
            adapter(&raw).create(input_rate, output_rate, maximum_input, maximum_output),
            Err(SampleRateError::InvalidArgument)
        ));
    }
}
