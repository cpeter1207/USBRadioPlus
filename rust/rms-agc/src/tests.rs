// SPDX-License-Identifier: MIT
//! Unit tests cover safe policy and the complete allocation/host boundary.

use super::*;
use std::alloc::{GlobalAlloc, System};
use std::cell::Cell;
use std::ffi::CStr;

thread_local! {
    static AUDIO_ACTIVE: Cell<bool> = const { Cell::new(false) };
    static AUDIO_ALLOCS: Cell<usize> = const { Cell::new(0) };
}

struct CheckedAllocator;

// SAFETY: This delegates each allocator contract unchanged to System; thread-local
// counters observe allocations without allocating or accessing another thread's state.
unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        AUDIO_ACTIVE.with(|active| {
            if active.get() {
                AUDIO_ALLOCS.with(|count| count.set(count.get() + 1));
            }
        });
        // SAFETY: Forward the allocator's original valid layout.
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: Forward the matching pointer and layout to the backing allocator.
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

unsafe fn fail_allocation(_: Layout) -> *mut u8 {
    ptr::null_mut()
}

struct Fixture {
    handle: Handle,
    controls: Box<[f32; CONTROL_COUNT]>,
    input: [f32; 512],
    detector: [f32; 512],
    output: [f32; 512],
}

impl Fixture {
    fn new(rate: c_ulong) -> Self {
        // SAFETY: Descriptor is valid and the supplied supported rate needs no buffers.
        let handle = unsafe { instantiate(&DESCRIPTOR, rate) };
        assert!(!handle.is_null());
        let mut controls = Box::new(SPECS.map(|spec| spec.2 as f32));
        for (index, value) in controls.iter_mut().enumerate() {
            // SAFETY: Box keeps controls stable until Drop; calls are serialized.
            unsafe {
                connect_port(handle, (index + 3) as c_ulong, value);
            }
        }
        Self {
            handle,
            controls,
            input: [0.0; 512],
            detector: [0.0; 512],
            output: [0.0; 512],
        }
    }

    fn process(&mut self, count: usize) {
        assert!(count <= 512);
        // SAFETY: These arrays contain at least count samples and remain alive for run.
        unsafe {
            connect_port(self.handle, 0, self.input.as_mut_ptr());
            connect_port(self.handle, 1, self.detector.as_mut_ptr());
            connect_port(self.handle, 2, self.output.as_mut_ptr());
            AUDIO_ACTIVE.with(|active| active.set(true));
            run(self.handle, count as c_ulong);
            AUDIO_ACTIVE.with(|active| active.set(false));
        }
        AUDIO_ALLOCS.with(|count| assert_eq!(count.get(), 0));
    }

    fn drive(&mut self, mut count: usize, detector: f32, input: f32) -> f64 {
        let mut result = 0.0;
        self.input.fill(input);
        self.detector.fill(detector);
        while count != 0 {
            let block = count.min(512);
            self.process(block);
            result = f64::from(self.output[block - 1]) / f64::from(input);
            count -= block;
        }
        result
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // SAFETY: All processing has stopped and this is the sole owner of the handle.
        unsafe {
            cleanup(self.handle);
        }
    }
}

#[test]
fn descriptor_and_allocation_contract() {
    let descriptor = ladspa_descriptor(0);
    let rebuilt_hints = port_hints();
    assert_eq!(descriptor, &DESCRIPTOR as *const _);
    assert!(ladspa_descriptor(1).is_null());
    assert_eq!(DESCRIPTOR.unique_id, 524950);
    assert_eq!(DESCRIPTOR.properties, 4);
    assert_eq!(DESCRIPTOR.port_count, 13);
    assert!(DESCRIPTOR.implementation_data.is_null());
    assert!(DESCRIPTOR.run_adding.is_none());
    assert!(DESCRIPTOR.set_run_adding_gain.is_none());
    assert!(DESCRIPTOR.deactivate.is_none());
    // SAFETY: All descriptor metadata is static and null terminated.
    unsafe {
        assert_eq!(
            CStr::from_ptr(DESCRIPTOR.label).to_bytes(),
            b"usbradioplus_agc"
        );
        assert_eq!(
            CStr::from_ptr(DESCRIPTOR.name).to_bytes(),
            b"USBRadioPlus gated RMS gain rider"
        );
        assert_eq!(
            CStr::from_ptr(DESCRIPTOR.maker).to_bytes(),
            b"USBRadioPlus contributors"
        );
        assert_eq!(CStr::from_ptr(DESCRIPTOR.copyright).to_bytes(), b"MIT");
        for index in 0..13 {
            assert_eq!(
                *DESCRIPTOR.port_descriptors.add(index),
                PORT_DESCRIPTORS[index]
            );
            assert!(
                !CStr::from_ptr(*DESCRIPTOR.port_names.add(index))
                    .to_bytes()
                    .is_empty()
            );
            let hint = &*DESCRIPTOR.port_range_hints.add(index);
            assert_eq!(hint.hint_descriptor, rebuilt_hints[index].hint_descriptor);
            assert_eq!(hint.lower_bound, rebuilt_hints[index].lower_bound);
            assert_eq!(hint.upper_bound, rebuilt_hints[index].upper_bound);
            if index < 3 {
                assert_eq!(hint.hint_descriptor, 0);
            } else {
                assert_eq!(hint.hint_descriptor, 3);
                assert_eq!(hint.lower_bound, SPECS[index - 3].0 as f32);
                assert_eq!(hint.upper_bound, SPECS[index - 3].1 as f32);
            }
        }
        for rate in [0, 999, 384001, c_ulong::MAX] {
            assert!(instantiate(descriptor, rate).is_null());
        }
        assert!(instantiate_with(8000, fail_allocation).is_null());
        cleanup(ptr::null_mut());
    }
    for rate in [1000, 384000] {
        drop(Fixture::new(rate));
    }
}

#[test]
fn disconnected_ports_defaults_and_reset() {
    // SAFETY: Each step uses a live handle and connects storage valid for its run call.
    unsafe {
        let handle = instantiate(&DESCRIPTOR, 8000);
        let mut input = [0.25, -0.25];
        let mut output = [99.0; 2];
        run(handle, 1);
        connect_port(handle, 0, input.as_mut_ptr());
        run(handle, 1);
        connect_port(handle, 1, input.as_mut_ptr());
        run(handle, 1);
        connect_port(handle, 2, output.as_mut_ptr());
        connect_port(handle, 13, input.as_mut_ptr());
        run(handle, 0);
        run(handle, 2);
        assert_eq!(input, output);
        activate(handle);
        run(handle, 1);
        assert_eq!(input[0], output[0]);
        cleanup(handle);
    }
}

#[test]
fn gain_law_caps_freezing_and_immediate_reduction() {
    for rate in [8000, 16000, 48000] {
        let mut f = Fixture::new(rate);
        let r = rate as usize;
        assert!((f.drive(r / 4, 0.01, 0.1) - 1.0).abs() < 1e-6);
        let db = 20.0 * f.drive(r / 2, 0.01, 0.1).log10();
        assert!((0.4..0.6).contains(&db));
        assert!((20.0 * f.drive(4 * r, 0.01, 0.1).log10() - 6.0).abs() < 0.001);
        let frozen = f.drive(r, 0.0001, 0.1);
        assert_eq!(f.drive(2 * r, 0.0001, 0.1), frozen);
        assert!((2.8..3.2).contains(&(20.0 * f.drive(r / 2, 0.5, 0.1).log10())));
        assert!((20.0 * f.drive(3 * r, 0.5, 0.1).log10() + 6.0).abs() < 0.001);
        f.controls[5] = 0.0;
        assert_eq!(f.drive(1, 0.5, 2.0), 1.0);
        assert_eq!(f.output[0], 2.0);
    }
}

#[test]
fn detector_independence_hysteresis_reacquisition_and_deadband() {
    let mut f = Fixture::new(48000);
    f.controls[8] = 0.0;
    f.controls[4] = 30.0;
    assert_eq!(f.drive(48000, 0.001, 4.0), 1.0);
    let before = f.drive(24000, 0.006, 0.1);
    assert!(f.drive(24000, 0.0028, 0.1) > before);
    let frozen = f.drive(48000, 0.001, 0.1);
    assert_eq!(f.drive(48000, 0.001, 0.1), frozen);
    assert!(f.drive(4800, 0.5, 0.1) < frozen);
    // SAFETY: Fixture owns its handle and has no concurrent run.
    unsafe {
        activate(f.handle);
    }
    assert!((20.0 * f.drive(144000, 0.063_095_73, 0.1).log10()).abs() < 0.1);
}

#[test]
fn pauses_and_reductions_reset_continuous_hold() {
    let mut f = Fixture::new(8000);
    for _ in 0..4 {
        assert!((f.drive(2000, 0.01, 0.1) - 1.0).abs() < 1e-6);
        assert!((f.drive(4000, 0.0001, 0.1) - 1.0).abs() < 1e-6);
    }
    let before = f.drive(8000, 0.5, 0.1);
    assert!(f.drive(2000, 0.01, 0.1) <= before);
}

#[test]
fn malformed_controls_audio_and_overflow_recover() {
    let mut f = Fixture::new(1000);
    for value in [f32::NAN, -f32::MAX, f32::MAX] {
        f.controls.fill(value);
        assert!(f.drive(1000, 0.1, 0.1).is_finite());
    }
    f.input[..2].copy_from_slice(&[f32::NAN, f32::INFINITY]);
    f.detector[..2].copy_from_slice(&[f32::NAN, f32::NEG_INFINITY]);
    f.process(2);
    assert_eq!(&f.output[..2], &[0.0, 0.0]);
    let mut f = Fixture::new(8000);
    f.controls[8] = 0.0;
    assert!(f.drive(16000, 0.01, 0.1) > 1.0);
    assert_eq!(f.drive(1, 0.01, f32::MAX), 0.0);
    assert!(f.drive(8000, f32::MAX, 0.1).is_finite());
    f.controls[4] = 0.0;
    f.controls[5] = 0.0;
    assert!((f.drive(8000, 0.01, 0.1) - 1.0).abs() < 1e-6);
}

#[test]
fn blocks_and_in_place_processing_are_identical() {
    for rate in [1000, 1001, 8000, 16000, 44100, 48000, 384000] {
        let mut bulk = Fixture::new(rate);
        let mut single = Fixture::new(rate);
        bulk.controls[8] = 0.0;
        single.controls[8] = 0.0;
        for block in 0..100 {
            for index in 0..512 {
                bulk.input[index] = (0.2 * ((block * 512 + index) as f64 * 0.07).sin()) as f32;
                bulk.detector[index] = bulk.input[index] * if block < 25 { 0.1 } else { 1.0 };
            }
            bulk.process(512);
            for index in 0..512 {
                let mut input = bulk.input[index];
                let mut detector = bulk.detector[index];
                // SAFETY: Valid single-sample buffers; both reads precede the aliased write.
                unsafe {
                    connect_port(single.handle, 0, &mut input);
                    connect_port(single.handle, 1, &mut detector);
                    connect_port(single.handle, 2, &mut input);
                    run(single.handle, 1);
                }
                assert_eq!(bulk.output[index], input);
            }
        }
    }
}

#[test]
fn detector_output_aliasing_preserves_both_inputs() {
    let mut separate = Fixture::new(48000);
    let alias = Fixture::new(48000);
    for index in 0..48000 {
        let mut program = if index % 2 == 0 { 0.01 } else { -0.01 };
        let mut detector = program * 0.25;
        separate.input[0] = program;
        separate.detector[0] = detector;
        separate.process(1);
        // SAFETY: Live single-sample buffers and serialized calls. Detector and
        // output deliberately alias, which is permitted by the existing LADSPA ABI.
        unsafe {
            connect_port(alias.handle, 0, &mut program);
            connect_port(alias.handle, 1, &mut detector);
            connect_port(alias.handle, 2, &mut detector);
            run(alias.handle, 1);
        }
        assert_eq!(separate.output[0], detector);
    }
}
