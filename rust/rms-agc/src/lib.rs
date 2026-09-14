// SPDX-License-Identifier: MIT
// Copyright (c) 2026 USBRadioPlus contributors
//! The existing `usbradioplus_agc` LADSPA effect, implemented in Rust.
//!
//! This thin host boundary owns connected pointers and allocation; the private core owns
//! the gain policy without LADSPA types. FFmpeg remains the sole audio-graph host.

mod core;

use core::{CONTROL_COUNT, GainRider, SPECS, SampleRate, Settings};
use std::alloc::{Layout, alloc, dealloc};
use std::ffi::{c_char, c_int, c_ulong, c_void};
use std::ptr;

const PORT_COUNT: usize = 3 + CONTROL_COUNT;
type Handle = *mut c_void;

/// C-compatible LADSPA range hint, in the standard external field order.
#[repr(C)]
pub struct PortRangeHint {
    /// LADSPA hint bit mask.
    pub hint_descriptor: c_int,
    /// Inclusive lower bound.
    pub lower_bound: f32,
    /// Inclusive upper bound.
    pub upper_bound: f32,
}

/// LADSPA 1.1 descriptor layout. No Rust-owned object layout crosses this boundary.
#[repr(C)]
pub struct Descriptor {
    /// Established plugin identity, 524950.
    pub unique_id: c_ulong,
    /// Stable host-facing label.
    pub label: *const c_char,
    /// LADSPA property flags.
    pub properties: c_int,
    /// Human-readable effect name.
    pub name: *const c_char,
    /// Effect author string.
    pub maker: *const c_char,
    /// License string.
    pub copyright: *const c_char,
    /// Count of audio and scalar ports.
    pub port_count: c_ulong,
    /// Port direction and type flags.
    pub port_descriptors: *const c_int,
    /// Array of static port labels.
    pub port_names: *const *const c_char,
    /// Array of static range hints.
    pub port_range_hints: *const PortRangeHint,
    /// Unused external implementation data.
    pub implementation_data: *mut c_void,
    /// Allocate an instance for a supported rate; returns null on failure.
    pub instantiate: unsafe extern "C" fn(*const Descriptor, c_ulong) -> Handle,
    /// Connect a host-owned port without resetting processing.
    pub connect_port: unsafe extern "C" fn(Handle, c_ulong, *mut f32),
    /// Reset streaming state while keeping host ports connected.
    pub activate: unsafe extern "C" fn(Handle),
    /// Process a complete host block without allocation or blocking.
    pub run: unsafe extern "C" fn(Handle, c_ulong),
    /// Unsupported additive processing entry.
    pub run_adding: Option<unsafe extern "C" fn(Handle, c_ulong)>,
    /// Unsupported additive gain control.
    pub set_run_adding_gain: Option<unsafe extern "C" fn(Handle, f32)>,
    /// No deactivation hook is needed.
    pub deactivate: Option<unsafe extern "C" fn(Handle)>,
    /// Destroy an instance after the host has stopped callbacks.
    pub cleanup: unsafe extern "C" fn(Handle),
}

// SAFETY: The sole descriptor is immutable, and all its non-null pointers refer
// to immutable static metadata. Per-instance mutable state is never stored here.
unsafe impl Sync for Descriptor {}

struct PortNames([*const c_char; PORT_COUNT]);

// SAFETY: Every pointer in this immutable array refers to a static C string.
unsafe impl Sync for PortNames {}

/// One host owns all calls and the connected buffers for this opaque instance.
struct Instance {
    ports: [*mut f32; PORT_COUNT],
    rider: GainRider,
}

impl Instance {
    fn new(rate: SampleRate) -> Self {
        Self {
            ports: [ptr::null_mut(); PORT_COUNT],
            rider: GainRider::new(rate),
        }
    }

    /// Read scalar controls only at the established block boundary.
    unsafe fn settings(&self) -> Settings {
        let mut values = [None; CONTROL_COUNT];
        for (value, &port) in values.iter_mut().zip(self.ports[3..].iter()) {
            // SAFETY: LADSPA requires every connected scalar to remain readable
            // during run; a null scalar intentionally uses the documented default.
            *value = unsafe { port.as_ref() }.copied();
        }
        Settings::from_ports(values)
    }

    /// Use raw scalar reads/writes so legal input/output aliasing never creates
    /// overlapping Rust references. The caller validates buffers via the LADSPA contract.
    unsafe fn run(&mut self, count: c_ulong) {
        let [input, detector, output] = [self.ports[0], self.ports[1], self.ports[2]];
        if input.is_null() || detector.is_null() || output.is_null() {
            return;
        }
        // SAFETY: Connected controls remain valid throughout this serialized host call.
        self.rider.configure(unsafe { self.settings() });
        for index in 0..count as usize {
            // SAFETY: The host supplies count readable samples in each input and
            // count writable output samples. Both inputs are read before output
            // is overwritten, including in-place and detector/output aliasing.
            unsafe {
                let program = input.add(index).read();
                let detector = detector.add(index).read();
                output
                    .add(index)
                    .write(self.rider.process(program, detector));
            }
        }
    }
}

/// Fallible allocation is injected only at setup, so failed allocation can be
/// exercised without changing any real-time operation or publishing a test ABI.
unsafe fn instantiate_with(rate: c_ulong, allocate: unsafe fn(Layout) -> *mut u8) -> Handle {
    let Some(rate) = SampleRate::new(rate) else {
        return ptr::null_mut();
    };
    // SAFETY: Layout exactly matches the value initialized below; allocation is
    // paired with cleanup and occurs only during the control-plane setup call.
    let instance = unsafe { allocate(Layout::new::<Instance>()) }.cast::<Instance>();
    if instance.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: A successful allocation provides aligned, writable Instance storage.
    unsafe {
        instance.write(Instance::new(rate));
    }
    instance.cast()
}

unsafe extern "C" fn instantiate(_: *const Descriptor, rate: c_ulong) -> Handle {
    // SAFETY: The system allocator and matching cleanup own the opaque instance.
    unsafe { instantiate_with(rate, alloc) }
}

unsafe extern "C" fn connect_port(handle: Handle, port: c_ulong, data: *mut f32) {
    if port < PORT_COUNT as c_ulong {
        // SAFETY: LADSPA supplies a live, exclusively owned handle from instantiate.
        unsafe {
            (*handle.cast::<Instance>()).ports[port as usize] = data;
        }
    }
}

unsafe extern "C" fn activate(handle: Handle) {
    // SAFETY: LADSPA serializes activation with run and supplies its live handle.
    unsafe {
        (*handle.cast::<Instance>()).rider.reset();
    }
}

unsafe extern "C" fn run(handle: Handle, count: c_ulong) {
    // SAFETY: LADSPA serializes calls for an instance and maintains connected buffers.
    unsafe {
        (*handle.cast::<Instance>()).run(count);
    }
}

unsafe extern "C" fn cleanup(handle: Handle) {
    if !handle.is_null() {
        // SAFETY: The host has stopped calls. This pointer was allocated by alloc
        // with this exact layout and is destroyed/deallocated exactly once.
        unsafe {
            ptr::drop_in_place(handle.cast::<Instance>());
            dealloc(handle.cast(), Layout::new::<Instance>());
        }
    }
}

const PORT_DESCRIPTORS: [c_int; PORT_COUNT] = [9, 9, 10, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5];
static PORT_NAMES: PortNames = PortNames([
    c"Program input".as_ptr(),
    c"Detector input".as_ptr(),
    c"Output".as_ptr(),
    c"Target RMS (dBFS)".as_ptr(),
    c"Averaging (ms)".as_ptr(),
    c"Gain increase (dB/s)".as_ptr(),
    c"Gain decrease (dB/s)".as_ptr(),
    c"Maximum boost (dB)".as_ptr(),
    c"Maximum attenuation (dB)".as_ptr(),
    c"Activity threshold (dBFS)".as_ptr(),
    c"Activity hysteresis (dB)".as_ptr(),
    c"Gain-increase hold (ms)".as_ptr(),
    c"Deadband (dB)".as_ptr(),
]);
const fn port_hints() -> [PortRangeHint; PORT_COUNT] {
    let mut hints = [const {
        PortRangeHint {
            hint_descriptor: 0,
            lower_bound: 0.0,
            upper_bound: 0.0,
        }
    }; PORT_COUNT];
    let mut index = 0;
    while index < CONTROL_COUNT {
        hints[index + 3] = PortRangeHint {
            hint_descriptor: 3,
            lower_bound: SPECS[index].0 as f32,
            upper_bound: SPECS[index].1 as f32,
        };
        index += 1;
    }
    hints
}
static PORT_HINTS: [PortRangeHint; PORT_COUNT] = port_hints();
static DESCRIPTOR: Descriptor = Descriptor {
    unique_id: 524950,
    label: c"usbradioplus_agc".as_ptr(),
    properties: 4,
    name: c"USBRadioPlus gated RMS gain rider".as_ptr(),
    maker: c"USBRadioPlus contributors".as_ptr(),
    copyright: c"MIT".as_ptr(),
    port_count: PORT_COUNT as c_ulong,
    port_descriptors: PORT_DESCRIPTORS.as_ptr(),
    port_names: PORT_NAMES.0.as_ptr(),
    port_range_hints: PORT_HINTS.as_ptr(),
    implementation_data: ptr::null_mut(),
    instantiate,
    connect_port,
    activate,
    run,
    run_adding: None,
    set_run_adding_gain: None,
    deactivate: None,
    cleanup,
};

/// Return the sole established LADSPA descriptor for index zero, or null otherwise.
///
/// The host owns valid buffers and serializes callbacks for each allocated handle.
#[unsafe(no_mangle)]
pub extern "C" fn ladspa_descriptor(index: c_ulong) -> *const Descriptor {
    if index == 0 { &DESCRIPTOR } else { ptr::null() }
}

#[cfg(test)]
mod tests;
