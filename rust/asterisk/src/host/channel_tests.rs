use super::super::support::{FakeChannel, Fixture, with_state};
use super::*;
use crate::{ABI_VERSION, URP_AST_JITTER_ADAPTIVE};
use std::cell::RefCell;
use std::sync::Arc;
use std::sync::mpsc::sync_channel;

#[derive(Default)]
pub(in crate::host) struct DriverCalls {
    pub(in crate::host) result: c_int,
    pub(in crate::host) voice: Vec<i16>,
    pub(in crate::host) text: Vec<u8>,
    pub(in crate::host) transmit: Vec<(u32, u32)>,
    pub(in crate::host) dtmf: Vec<u32>,
    pub(in crate::host) reserve_result: c_int,
    pub(in crate::host) start_result: c_int,
    pub(in crate::host) stop_result: c_int,
    pub(in crate::host) reserves: Vec<(Vec<u8>, u32, usize)>,
    pub(in crate::host) starts: usize,
    pub(in crate::host) stops: usize,
    pub(in crate::host) destroys: usize,
    pub(in crate::host) profiles: Vec<Vec<u8>>,
    pub(in crate::host) query_result: c_int,
    pub(in crate::host) copy_result: c_int,
    worker_channel: usize,
    stop_on_transmit: bool,
    stop_on_service: bool,
    service_result: c_int,
    services: usize,
}

pub(in crate::host) unsafe extern "C" fn capture_profile(
    context: *mut c_void,
    index: u32,
    output: *mut u8,
    capacity: u32,
    length: *mut u32,
) -> c_int {
    // SAFETY: the host retains this fixture and writable length/output buffers.
    unsafe {
        let calls = (*context.cast::<Mutex<DriverCalls>>()).lock().unwrap();
        let Some(profile) = calls.profiles.get(index as usize) else {
            return URP_AST_CHANNEL_NOT_FOUND;
        };
        *length = profile.len() as u32;
        if output.is_null() {
            return calls.query_result;
        }
        assert!(capacity as usize >= profile.len());
        ptr::copy_nonoverlapping(profile.as_ptr(), output, profile.len());
        calls.copy_result
    }
}

pub(in crate::host) unsafe extern "C" fn capture_command(
    context: *mut c_void,
    command: *mut UrpAstChannelCommand,
) -> c_int {
    // SAFETY: synchronous control owns this complete command and live fixture.
    unsafe {
        (*command).value += 7;
    }
    // SAFETY: the fixture mutex remains valid through the synchronous reply.
    unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap()
        .result
}

pub(in crate::host) unsafe extern "C" fn capture_status(
    context: *mut c_void,
    output: *mut UrpAstChannelStatus,
) -> c_int {
    // SAFETY: synchronous control owns this initialized output and live fixture.
    unsafe {
        (*output).running = 1;
    }
    // SAFETY: the fixture mutex remains valid through the synchronous reply.
    unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap()
        .result
}

#[test]
fn live_profile_selection_handles_query_failures_and_frozen_membership() {
    let _fixture = Fixture::new();
    let mut descriptor = lifecycle_descriptor();
    descriptor.driver_channel_name = Some(capture_profile);
    let calls = Mutex::new(DriverCalls::default());
    assert!(first_live_profile().is_none());
    assert!(with_live_channel("usb", |_| ()).is_none());
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    // SAFETY: this complete registered descriptor and owner are retained by the guard.
    let owner = unsafe {
        request(
            c"RadioPlus".as_ptr(),
            requested_capability(c"RadioPlus"),
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        )
    };
    assert!(!owner.is_null());
    for (profiles, query_result, copy_result, expected) in [
        (vec![], 0, 0, None),
        (vec![b"usb".to_vec()], -6, 0, None),
        (vec![vec![]], 0, 0, None),
        (vec![b"usb".to_vec()], 0, -6, None),
        (vec![vec![0xff]], 0, 0, None),
        (vec![b"other".to_vec()], 0, 0, None),
        (vec![b"other".to_vec(), b"UsB".to_vec()], 0, 0, Some("UsB")),
    ] {
        {
            let mut calls = calls.lock().unwrap();
            calls.profiles = profiles;
            calls.query_result = query_result;
            calls.copy_result = copy_result;
        }
        assert_eq!(first_live_profile().as_deref(), expected);
    }
    assert_eq!(
        with_live_channel("UsB", |channel| channel.name().to_owned()),
        Some("usb".into())
    );
    assert!(with_live_channel("other", |_| ()).is_none());
    let mut membership = lock_live_channels();
    assert_eq!(
        membership.iter().map(Channel::name).collect::<Vec<_>>(),
        ["usb"]
    );
    assert_eq!(
        membership
            .first_profile(driver_context().unwrap())
            .as_deref(),
        Some("UsB")
    );
    let channel = membership.iter().next().unwrap();
    assert_eq!(service(channel), URP_AST_CHANNEL_BUSY);
    membership.reopen_control();
    assert_eq!(service(membership.iter().next().unwrap()), URP_AST_OK);
    drop(membership);
    let descriptor = UrpAstDescriptor {
        driver_channel_name: None,
        ..callback_descriptor()
    };
    assert!(
        first_profile(
            DriverContext {
                descriptor: &descriptor,
                driver: ptr::null_mut()
            },
            &[]
        )
        .is_none()
    );
}

#[test]
fn channel_forwarders_preserve_typed_outputs_and_admission_failures() {
    let _fixture = Fixture::new();
    let calls = Mutex::new(DriverCalls::default());
    for case in 0..3 {
        let mut descriptor = callback_descriptor();
        match case {
            0 => descriptor.channel_service = Some(capture_start),
            1 => descriptor.channel_reload_prepare = Some(capture_start),
            _ => descriptor.channel_reload_activate = Some(capture_start),
        }
        let (channel, _owner) = callback_channel(&descriptor, &calls);
        calls.lock().unwrap().start_result = 17;
        let result = match case {
            0 => service(&channel),
            1 => reload_prepare(&channel),
            _ => reload_activate(&channel),
        };
        assert_eq!(result, 17);
    }
    assert_eq!(calls.lock().unwrap().starts, 3);
    let mut descriptor = callback_descriptor();
    descriptor.channel_reload_finish = Some(capture_dtmf);
    descriptor.channel_set_echo = Some(capture_dtmf);
    descriptor.channel_service = Some(capture_service);
    descriptor.channel_command = Some(capture_command);
    descriptor.channel_get_status = Some(capture_status);
    let (channel, _owner) = callback_channel(&descriptor, &calls);
    calls.lock().unwrap().result = 17;
    assert_eq!(reload_finish(&channel, true), 17);
    assert_eq!(reload_finish(&channel, false), 17);
    assert_eq!(set_echo(&channel, true), 17);
    assert_eq!(set_transmit(&channel, true, 1230), 17);
    assert_eq!(calls.lock().unwrap().dtmf, [1, 0, 1]);
    assert_eq!(calls.lock().unwrap().transmit, [(1, 1230)]);
    // SAFETY: command fields are integers and a fixed integer array.
    let mut command: UrpAstChannelCommand = unsafe { zeroed() };
    command.value = 10;
    assert_eq!(run_command(&channel, &mut command), 17);
    assert_eq!(command.value, 17);
    let mut output = UrpAstChannelStatus::default();
    assert_eq!(read_status(&channel, &mut output), 17);
    assert_eq!(output.running, 1);
    with_state(|state| state.taskprocessor_push_result = -1);
    assert_eq!(
        run_command(&channel, &mut command),
        URP_AST_ASTERISK_FAILURE
    );
    assert_eq!(command.value, 17);
    assert_eq!(read_status(&channel, &mut output), URP_AST_ASTERISK_FAILURE);
    assert_eq!(output.running, 1);
    with_state(|state| state.taskprocessor_push_result = 0);
    calls.lock().unwrap().result = URP_AST_OK;
    let poisoned = std::panic::catch_unwind(|| {
        let _gate = control_gate().write().unwrap();
        panic!("test poisoned admission gate");
    });
    let status = service(&channel);
    let transmit = transmit(&channel, true, 1230);
    control_gate().clear_poison();
    assert!(poisoned.is_err());
    assert_eq!(status, URP_AST_ASTERISK_FAILURE);
    assert_eq!(transmit, -1);
    assert_eq!(calls.lock().unwrap().transmit, [(1, 1230)]);
}

unsafe extern "C" fn capture_reserve(
    context: *mut c_void,
    arguments: *const UrpAstChannelReserveArgs,
    output: *mut *mut c_void,
) -> c_int {
    // SAFETY: request retains valid reserve arguments/output and the fixture mutex.
    unsafe {
        let arguments = &*arguments;
        let mut calls = (*context.cast::<Mutex<DriverCalls>>()).lock().unwrap();
        calls.reserves.push((
            std::slice::from_raw_parts(
                arguments.channel_name,
                arguments.channel_name_length as usize,
            )
            .to_vec(),
            arguments.transport,
            arguments.channel_context as usize,
        ));
        if calls.reserve_result == URP_AST_OK {
            *output = context;
        }
        calls.reserve_result
    }
}

unsafe extern "C" fn capture_start(context: *mut c_void) -> c_int {
    // SAFETY: the fixture mutex outlives every synchronous control reply.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    calls.starts += 1;
    calls.start_result
}

unsafe extern "C" fn capture_stop(context: *mut c_void) -> c_int {
    // SAFETY: the fixture mutex outlives every synchronous control reply.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    calls.stops += 1;
    calls.stop_result
}

unsafe extern "C" fn capture_service(context: *mut c_void) -> c_int {
    // SAFETY: the fixture context and optional worker channel outlive dispatch.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    calls.services += 1;
    if calls.stop_on_service {
        // SAFETY: worker tests publish their retained Box<Channel> before dispatch.
        unsafe { &*(calls.worker_channel as *const Channel) }
            .delivery_stop
            .store(true, Ordering::Release);
    }
    calls.service_result
}

unsafe extern "C" fn capture_destroy(context: *mut c_void) {
    // SAFETY: the fixture mutex outlives this final synchronous control reply.
    unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap()
        .destroys += 1;
}

pub(in crate::host) fn lifecycle_descriptor() -> UrpAstDescriptor {
    // SAFETY: the process descriptor contains plain ABI fields and callback pointers.
    let mut descriptor = unsafe { ptr::read(crate::product_descriptor()) };
    descriptor.channel_reserve = Some(capture_reserve);
    descriptor.channel_start = Some(capture_start);
    descriptor.channel_stop = Some(capture_stop);
    descriptor.channel_service = Some(capture_service);
    descriptor.channel_destroy = Some(capture_destroy);
    descriptor.channel_get_jitter_config = Some(capture_jitter);
    descriptor
}

pub(in crate::host) struct RegistrationGuard<'a> {
    pub(in crate::host) _descriptor: &'a UrpAstDescriptor,
    pub(in crate::host) _calls: &'a Mutex<DriverCalls>,
}

impl Drop for RegistrationGuard<'_> {
    fn drop(&mut self) {
        let owners = with_state(|state| state.channels.keys().copied().collect::<Vec<_>>());
        for owner in owners {
            // SAFETY: cleanup runs before the borrowed descriptor and context expire.
            unsafe { ffi::ast_hangup(owner as *mut ffi::ast_channel) };
        }
        assert_eq!(usbradioplus_asterisk_channel_host_unregister(), URP_AST_OK);
    }
}

pub(in crate::host) fn register_fixture(
    descriptor: &UrpAstDescriptor,
    calls: &Mutex<DriverCalls>,
) -> c_int {
    // SAFETY: the registration guard retains both inputs; module is an opaque token.
    unsafe {
        usbradioplus_asterisk_channel_host_register(
            descriptor,
            ptr::from_ref(calls).cast_mut().cast(),
            ptr::dangling_mut::<u8>().cast(),
        )
    }
}

pub(in crate::host) fn requested_capability(technology: &CStr) -> *mut ffi::ast_format_cap {
    let contract = technology_contract(technology.to_str().unwrap()).unwrap();
    let snapshot = snapshot(contract).unwrap();
    // SAFETY: registration retains the technology and its capability.
    unsafe { (*snapshot.technology).capabilities }
}

#[test]
fn registration_rolls_back_every_completed_external_stage() {
    for case in 0..9 {
        let _fixture = Fixture::new();
        let descriptor = lifecycle_descriptor();
        let calls = Mutex::new(DriverCalls::default());
        let _registration = RegistrationGuard {
            _descriptor: &descriptor,
            _calls: &calls,
        };
        with_state(|state| match case {
            0 => state.advanced_format_missing = true,
            1 => state.advanced_format_wrong_rate = true,
            2 | 3 => state.capability_fail_at = case - 1,
            4 | 5 => state.append_fail_at = case - 3,
            6 | 7 => state.register_fail_at = case - 5,
            _ => {}
        });
        let result = register_fixture(&descriptor, &calls);
        if case < 8 {
            assert_eq!(result, URP_AST_ASTERISK_FAILURE, "case {case}");
            assert!(driver_context().is_none());
        } else {
            assert_eq!(result, URP_AST_OK);
            assert_eq!(register_fixture(&descriptor, &calls), URP_AST_CHANNEL_BUSY);
            assert_eq!(
                driver_context().unwrap().driver,
                ptr::from_ref(&calls).cast_mut().cast()
            );
            assert_eq!(usbradioplus_asterisk_channel_host_unregister(), URP_AST_OK);
        }
        with_state(|state| {
            assert!(state.registered.is_empty(), "case {case}");
            assert!(state.capabilities.is_empty(), "case {case}");
            let expected: &[&str] = match case {
                7 => &["RadioPlus"],
                8 => &["RadioPlusAdvanced", "RadioPlus"],
                _ => &[],
            };
            assert_eq!(state.unregistered, expected);
        });
    }
}

#[test]
fn requester_rejects_invalid_inputs_without_reserving_resources() {
    let _fixture = Fixture::new();
    let descriptor = lifecycle_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    // SAFETY: Fixture serializes all access; restore the process-format token
    // before assertions or other operations can observe the temporary absence.
    let missing_format = unsafe {
        let saved = ffi::ast_format_slin;
        ffi::ast_format_slin = ptr::null_mut();
        let result = register_fixture(&descriptor, &calls);
        ffi::ast_format_slin = saved;
        result
    };
    assert_eq!(missing_format, URP_AST_ASTERISK_FAILURE);
    // SAFETY: null inputs are intentionally validated before dereferencing.
    unsafe {
        assert_eq!(
            usbradioplus_asterisk_channel_host_register(
                ptr::null(),
                ptr::null_mut(),
                ptr::null_mut()
            ),
            crate::URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            usbradioplus_asterisk_channel_host_register(
                &descriptor,
                ptr::null_mut(),
                ptr::null_mut()
            ),
            crate::URP_AST_INVALID_ARGUMENT
        );
        assert_eq!(
            usbradioplus_asterisk_channel_host_register(
                &descriptor,
                ptr::from_ref(&calls).cast_mut().cast(),
                ptr::null_mut()
            ),
            crate::URP_AST_INVALID_ARGUMENT
        );
        let invalid: UrpAstDescriptor = zeroed();
        assert_eq!(
            register_fixture(&invalid, &calls),
            crate::URP_AST_INCOMPATIBLE_ABI
        );
        assert!(
            request(
                c"RadioPlus".as_ptr(),
                ptr::dangling_mut(),
                ptr::null(),
                ptr::null(),
                c"usb".as_ptr(),
                ptr::null_mut()
            )
            .is_null()
        );
    }
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    let capability = requested_capability(c"RadioPlus");
    for (technology, formats, name) in [
        (ptr::null(), capability, c"usb".as_ptr()),
        (c"RadioPlus".as_ptr(), ptr::null_mut(), c"usb".as_ptr()),
        (c"RadioPlus".as_ptr(), capability, ptr::null()),
        (c"Unknown".as_ptr(), capability, c"usb".as_ptr()),
        (c"RadioPlus".as_ptr(), capability, c"".as_ptr()),
        (c"\xFF".as_ptr(), capability, c"usb".as_ptr()),
    ] {
        assert!(
            // SAFETY: each present string and capability is live; nulls test validation.
            unsafe {
                request(
                    technology,
                    formats,
                    ptr::null(),
                    ptr::null(),
                    name,
                    ptr::null_mut(),
                )
            }
            .is_null()
        );
    }
    with_state(|state| state.incompatible_formats = true);
    assert!(
        // SAFETY: the fixture forces compatibility failure for otherwise valid inputs.
        unsafe {
            request(
                c"RadioPlus".as_ptr(),
                capability,
                ptr::null(),
                ptr::null(),
                c"usb".as_ptr(),
                ptr::null_mut(),
            )
        }
        .is_null()
    );
    assert!(calls.lock().unwrap().reserves.is_empty());
    with_state(|state| assert!(state.taskprocessors.is_empty()));
}

#[test]
fn request_failures_release_reservation_and_asterisk_resources() {
    for case in 0..8 {
        let _fixture = Fixture::new();
        let descriptor = lifecycle_descriptor();
        let calls = Mutex::new(DriverCalls::default());
        let _registration = RegistrationGuard {
            _descriptor: &descriptor,
            _calls: &calls,
        };
        assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
        with_state(|state| match case {
            0 => state.taskprocessor_get_fails = true,
            1 => state.dsp_allocation_fails = true,
            5 | 7 => state.channel_allocation_fails = true,
            _ => {}
        });
        {
            let mut calls = calls.lock().unwrap();
            calls.reserve_result = match case {
                2 => URP_AST_CHANNEL_BUSY,
                3 => URP_AST_CHANNEL_NOT_FOUND,
                4 => -6,
                _ => URP_AST_OK,
            };
            if case == 6 {
                calls.result = -6;
            }
        }
        let mut cause = 0;
        let technology = if case == 7 {
            c"RadioPlusAdvanced"
        } else {
            c"RadioPlus"
        };
        // SAFETY: registration owns this capability, descriptor, and driver context.
        let owner = unsafe {
            request(
                technology.as_ptr(),
                requested_capability(technology),
                ptr::null(),
                ptr::null(),
                c"usb".as_ptr(),
                if case == 3 {
                    ptr::null_mut()
                } else {
                    &mut cause
                },
            )
        };
        assert!(owner.is_null(), "case {case}");
        assert_eq!(
            cause,
            if case == 2 {
                ffi::AST_CAUSE_BUSY as c_int
            } else {
                0
            }
        );
        assert_eq!(calls.lock().unwrap().destroys, usize::from(case >= 5));
        assert_eq!(calls.lock().unwrap().stops, usize::from(case >= 5));
        assert_eq!(ACTIVE_CHANNELS.load(Ordering::Acquire), 0);
        with_state(|state| {
            assert!(state.channels.is_empty(), "case {case}");
            assert!(state.taskprocessors.is_empty(), "case {case}");
            assert!(state.dsps.is_empty(), "case {case}");
            assert_eq!(state.module_references, 0);
            assert_eq!(state.hangups, usize::from(case == 6));
        });
    }
}

#[test]
fn text_callback_rejects_real_strings_larger_than_the_driver_abi() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (_channel, mut owner) = callback_channel(&descriptor, &calls);
    let started = std::time::Instant::now();
    let before = std::fs::read_to_string("/proc/self/smaps_rollup").unwrap();
    let text = super::super::support::OversizedCString::new();
    // SAFETY: text owns a readable, terminated string and owner remains live.
    assert_eq!(unsafe { send_text(owner.as_ptr(), text.as_ptr()) }, -1);
    assert!(calls.lock().unwrap().text.is_empty());
    let after = std::fs::read_to_string("/proc/self/smaps_rollup").unwrap();
    let pss = |value: String| {
        value
            .lines()
            .find(|line| line.starts_with("Pss:"))
            .unwrap()
            .to_owned()
    };
    eprintln!(
        "4 GiB CStr boundary: {:?}; before {}; after {}",
        started.elapsed(),
        pss(before),
        pss(after)
    );
}

#[test]
fn registered_technologies_request_start_and_hang_up_with_balanced_resources() {
    for technology in [c"RadioPlus", c"RadioPlusAdvanced"] {
        let _fixture = Fixture::new();
        let descriptor = lifecycle_descriptor();
        let calls = Mutex::new(DriverCalls::default());
        let _registration = RegistrationGuard {
            _descriptor: &descriptor,
            _calls: &calls,
        };
        assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
        let capability = requested_capability(technology);
        // SAFETY: the registered technology owns its requester and capability.
        let owner = unsafe {
            request(
                technology.as_ptr(),
                capability,
                ptr::null(),
                ptr::null(),
                c"usb".as_ptr(),
                ptr::null_mut(),
            )
        };
        assert!(!owner.is_null());
        // SAFETY: the newly requested owner retains the live pinned Channel.
        let channel = unsafe { &*ffi::ast_channel_tech_pvt(owner).cast::<Channel>() };
        let advanced = technology == c"RadioPlusAdvanced";
        assert_eq!(
            channel.sample_rate_hz,
            if advanced { 48_000 } else { 8_000 }
        );
        assert_eq!(channel.dsp.is_null(), advanced);
        assert_eq!(calls.lock().unwrap().reserves[0].0, b"usb");
        assert_eq!(
            calls.lock().unwrap().reserves[0].1,
            if advanced {
                URP_AST_TRANSPORT_RPT_ADVANCED
            } else {
                URP_AST_TRANSPORT_APP_RPT
            }
        );
        assert_eq!(
            usbradioplus_asterisk_channel_host_unregister(),
            URP_AST_CHANNEL_BUSY
        );
        assert_eq!(register_fixture(&descriptor, &calls), URP_AST_CHANNEL_BUSY);
        mark_jitter_pending(channel);
        calls.lock().unwrap().result = -6;
        // SAFETY: owner remains registered and live until explicit hangup below.
        unsafe {
            assert_eq!(call(ptr::null_mut(), ptr::null(), 0), -1);
            assert_eq!(call(owner, ptr::null(), 0), -1);
            calls.lock().unwrap().result = URP_AST_OK;
            calls.lock().unwrap().start_result = -6;
            assert_eq!(call(owner, ptr::null(), 0), -1);
            calls.lock().unwrap().start_result = URP_AST_OK;
            channel.direct.store(advanced, Ordering::Release);
            assert_eq!(call(owner, ptr::null(), 0), 0);
        }
        with_state(|state| {
            let owner = &state.channels[&(owner as usize)];
            assert_eq!(
                owner.name.to_bytes(),
                if advanced {
                    b"RadioPlusAdvanced/usb".as_slice()
                } else {
                    b"RadioPlus/usb".as_slice()
                }
            );
            assert_eq!(owner.state, ffi::AST_STATE_UP as c_int);
            assert_eq!(owner.fd, (0, -1));
            assert_eq!(owner.formats[0], capability as usize);
            assert_eq!(owner.formats[1], owner.formats[2]);
            assert_eq!(state.module_references, 1);
        });
        calls.lock().unwrap().stop_result = if advanced { -6 } else { URP_AST_OK };
        // SAFETY: hangup consumes the pinned Channel; the fake owner stays allocated.
        unsafe {
            assert_eq!(hangup(owner), if advanced { -1 } else { 0 });
            assert_eq!(hangup(owner), 0);
        }
        assert_eq!(calls.lock().unwrap().destroys, 1);
        assert_eq!(calls.lock().unwrap().stops, 1);
        assert_eq!(ACTIVE_CHANNELS.load(Ordering::Acquire), 0);
        with_state(|state| {
            assert!(state.taskprocessors.is_empty());
            assert!(state.dsps.is_empty());
            assert_eq!(state.module_references, 0);
        });
    }
}

unsafe extern "C" fn capture_voice(
    context: *mut c_void,
    samples: *const i16,
    length: u32,
) -> c_int {
    // SAFETY: fixture retains its mutex and the callback receives readable PCM.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    // SAFETY: the host validated this many readable PCM samples.
    calls.voice = unsafe { std::slice::from_raw_parts(samples, length as usize) }.to_vec();
    calls.result
}

unsafe extern "C" fn capture_text(context: *mut c_void, text: *const u8, length: u32) -> c_int {
    // SAFETY: fixture retains its mutex and dispatch owns readable text bytes.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    // SAFETY: dispatch retains its owned text allocation until this reply.
    calls.text = unsafe { std::slice::from_raw_parts(text, length as usize) }.to_vec();
    calls.result
}

pub(in crate::host) unsafe extern "C" fn capture_transmit(
    context: *mut c_void,
    keyed: u32,
    tone: u32,
) -> c_int {
    // SAFETY: fixture retains this mutex until synchronous dispatch completes.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    calls.transmit.push((keyed, tone));
    if calls.stop_on_transmit {
        // SAFETY: worker tests publish their retained Box<Channel> before dispatch.
        unsafe { &*(calls.worker_channel as *const Channel) }
            .delivery_stop
            .store(true, Ordering::Release);
    }
    calls.result
}

#[test]
fn delivery_thread_start_failure_stops_the_started_station_and_allows_retry() {
    use super::super::support::{urp_test_fail_thread_create, urp_test_thread_create_calls};
    let _fixture = Fixture::new();
    let descriptor = lifecycle_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let _registration = RegistrationGuard {
        _descriptor: &descriptor,
        _calls: &calls,
    };
    assert_eq!(register_fixture(&descriptor, &calls), URP_AST_OK);
    // SAFETY: the registration guard retains the descriptor, context, and owner.
    unsafe {
        let owner = request(
            c"RadioPlus".as_ptr(),
            requested_capability(c"RadioPlus"),
            ptr::null(),
            ptr::null(),
            c"usb".as_ptr(),
            ptr::null_mut(),
        );
        assert!(!owner.is_null());
        urp_test_fail_thread_create(2);
        let result = call(owner, ptr::null(), 0);
        let creates = urp_test_thread_create_calls();
        urp_test_fail_thread_create(0);
        assert_eq!(result, -1);
        assert_eq!(creates, 3, "Start succeeds, delivery fails, Stop succeeds");
        assert_eq!(calls.lock().unwrap().starts, 1);
        assert_eq!(calls.lock().unwrap().stops, 1);
        assert_eq!(call(owner, ptr::null(), 0), 0);
        assert_eq!(calls.lock().unwrap().starts, 2);
    }
}

#[test]
fn delivery_worker_bounds_transmit_and_service_failure_paths() {
    let _fixture = Fixture::new();
    for case in 0..7 {
        let mut descriptor = callback_descriptor();
        descriptor.channel_service = Some(capture_service);
        let calls = Mutex::new(DriverCalls::default());
        let (channel, _owner) = callback_channel(&descriptor, &calls);
        {
            let mut calls = calls.lock().unwrap();
            calls.worker_channel = ptr::from_ref(&*channel) as usize;
            calls.stop_on_transmit = case == 2;
            calls.stop_on_service = case >= 3;
            calls.result = match case {
                1 => -6,
                2 => URP_AST_CHANNEL_BUSY,
                _ => URP_AST_OK,
            };
            calls.service_result = match case {
                4 => URP_AST_CHANNEL_BUSY,
                5 => -6,
                _ => URP_AST_OK,
            };
        }
        channel.delivery_stop.store(case == 0, Ordering::Release);
        if matches!(case, 1 | 2 | 6) {
            channel
                .pending_transmit
                .store(PendingTransmit::new(true, 1230).raw(), Ordering::Release);
        }
        // SAFETY: the fixture retains all worker state until this synchronous run returns.
        unsafe { delivery::run_worker(&*channel) };
        assert_eq!(
            channel.service_failed.load(Ordering::Acquire),
            matches!(case, 1 | 5),
            "case {case}"
        );
        let calls = calls.lock().unwrap();
        assert_eq!(calls.services, usize::from(case >= 3), "case {case}");
        assert_eq!(
            calls.transmit.len(),
            usize::from(matches!(case, 1 | 2 | 6)),
            "case {case}"
        );
    }
}

pub(in crate::host) unsafe extern "C" fn capture_dtmf(context: *mut c_void, enabled: u32) -> c_int {
    // SAFETY: fixture retains this mutex until synchronous dispatch completes.
    let mut calls = unsafe { &*context.cast::<Mutex<DriverCalls>>() }
        .lock()
        .unwrap();
    calls.dtmf.push(enabled);
    calls.result
}

unsafe extern "C" fn capture_jitter(
    context: *mut c_void,
    output: *mut UrpAstJitterConfig,
) -> c_int {
    // SAFETY: dispatch provides initialized writable output and a live fixture.
    unsafe {
        (*output).enabled = 1;
        (*output).maximum_size_ms = 80;
        (*output).implementation = URP_AST_JITTER_FIXED;
        (*context.cast::<Mutex<DriverCalls>>())
            .lock()
            .unwrap()
            .result
    }
}

pub(in crate::host) fn callback_descriptor() -> UrpAstDescriptor {
    // SAFETY: descriptor fields are integers, raw pointers, and optional callbacks.
    let mut descriptor: UrpAstDescriptor = unsafe { zeroed() };
    descriptor.channel_write_voice = Some(capture_voice);
    descriptor.channel_write_text = Some(capture_text);
    descriptor.channel_set_transmit = Some(capture_transmit);
    descriptor.channel_set_dtmf = Some(capture_dtmf);
    descriptor.channel_get_jitter_config = Some(capture_jitter);
    descriptor
}

pub(in crate::host) fn callback_channel(
    descriptor: &UrpAstDescriptor,
    calls: &Mutex<DriverCalls>,
) -> (Box<Channel>, Box<FakeChannel>) {
    let mut channel = Box::new(Channel {
        _name: "usb".into(),
        descriptor,
        rust_channel: AtomicPtr::new(ptr::from_ref(calls).cast_mut().cast()),
        control: ptr::null_mut(),
        dsp: ptr::null_mut(),
        format: ptr::null_mut(),
        sample_rate_hz: APP_RPT_RATE_HZ,
        frame_samples: 160,
        owner: AtomicPtr::new(ptr::null_mut()),
        worker: Mutex::new(None),
        delivery_stop: AtomicBool::new(false),
        service_failed: AtomicBool::new(false),
        jitter_pending: AtomicBool::new(false),
        pending_transmit: AtomicU64::new(0),
        direct: AtomicBool::new(false),
    });
    let mut owner = FakeChannel::new(ptr::from_mut(&mut *channel).cast());
    channel.owner.store(owner.as_ptr(), Ordering::Release);
    (channel, owner)
}

#[test]
fn voice_callbacks_validate_frames_and_propagate_driver_results() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (channel, mut owner) = callback_channel(&descriptor, &calls);
    let mut empty = FakeChannel::new(ptr::null_mut());
    let mut samples = [23i16; 160];
    // SAFETY: a frame contains only integer fields, pointers, and unions.
    let mut frame: ffi::ast_frame = unsafe { zeroed() };
    frame.frametype = ffi::AST_FRAME_VOICE;
    frame.datalen = 320;
    frame.samples = 160;
    frame.data.ptr = samples.as_mut_ptr().cast();
    // SAFETY: each owner, frame, and PCM buffer remains live for each callback.
    unsafe {
        for missing in [ptr::null_mut(), empty.as_ptr()] {
            assert!(read(missing).is_null());
            assert_eq!(write(missing, &mut frame), -1);
        }
        assert_eq!(read(owner.as_ptr()), &raw mut ffi::ast_null_frame);
        channel.service_failed.store(true, Ordering::Release);
        assert!(read(owner.as_ptr()).is_null());
        assert_eq!(write(owner.as_ptr(), ptr::null_mut()), -1);
        for case in 0..6 {
            let mut invalid = frame;
            match case {
                0 => invalid.frametype = ffi::AST_FRAME_TEXT,
                1 => invalid.data.ptr = ptr::null_mut(),
                2 => invalid.datalen = 319,
                3 => invalid.datalen = -2,
                4 => invalid.datalen = 318,
                _ => invalid.samples = 159,
            }
            assert_eq!(write(owner.as_ptr(), &mut invalid), -1);
        }
        assert!(calls.lock().unwrap().voice.is_empty());
        for result in [URP_AST_OK, -6] {
            calls.lock().unwrap().result = result;
            assert_eq!(
                write(owner.as_ptr(), &mut frame),
                if result == 0 { 0 } else { -1 }
            );
            assert_eq!(calls.lock().unwrap().voice, [23; 160]);
        }
    }
}

#[test]
fn text_and_tone_options_preserve_payloads_and_failures() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (_channel, mut owner) = callback_channel(&descriptor, &calls);
    let mut empty = FakeChannel::new(ptr::null_mut());
    let mut mode = 0u8;
    // SAFETY: callback arguments are live fixture owners and valid byte buffers.
    unsafe {
        for missing in [ptr::null_mut(), empty.as_ptr()] {
            assert_eq!(send_text(missing, c"x".as_ptr()), -1);
            assert_eq!(
                setoption(
                    missing,
                    ffi::AST_OPTION_TONE_VERIFY as c_int,
                    ptr::from_mut(&mut mode).cast(),
                    1
                ),
                -1
            );
            assert_eq!(*libc::__errno_location(), libc::EINVAL);
        }
        assert_eq!(send_text(owner.as_ptr(), ptr::null()), -1);
        for (data, length) in [(ptr::null_mut(), 1), (ptr::from_mut(&mut mode).cast(), 0)] {
            assert_eq!(
                setoption(
                    owner.as_ptr(),
                    ffi::AST_OPTION_TONE_VERIFY as c_int,
                    data,
                    length
                ),
                -1
            );
            assert_eq!(*libc::__errno_location(), libc::EINVAL);
        }
        for result in [URP_AST_OK, -6] {
            calls.lock().unwrap().result = result;
            let expected = if result == 0 { 0 } else { -1 };
            assert_eq!(send_text(owner.as_ptr(), c"hello radio".as_ptr()), expected);
            assert_eq!(calls.lock().unwrap().text, b"hello radio");
            for value in [0u8, 3] {
                mode = value;
                assert_eq!(
                    setoption(
                        owner.as_ptr(),
                        ffi::AST_OPTION_TONE_VERIFY as c_int,
                        ptr::from_mut(&mut mode).cast(),
                        1
                    ),
                    expected
                );
            }
        }
    }
    assert_eq!(calls.lock().unwrap().dtmf, [1, 0, 1, 0]);
}

#[test]
fn indications_forward_music_and_radio_control_with_deferred_replay() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (channel, mut owner) = callback_channel(&descriptor, &calls);
    let mut empty = FakeChannel::new(ptr::null_mut());
    // SAFETY: live fixture owners and bounded tone/class buffers match each API.
    unsafe {
        assert_eq!(
            indicate(
                empty.as_ptr(),
                ffi::AST_CONTROL_BUSY as c_int,
                ptr::null(),
                0
            ),
            -1
        );
        for condition in [
            ffi::AST_CONTROL_BUSY,
            ffi::AST_CONTROL_CONGESTION,
            ffi::AST_CONTROL_RINGING,
            ffi::AST_CONTROL_VIDUPDATE,
        ] {
            assert_eq!(
                indicate(owner.as_ptr(), condition as c_int, ptr::null(), 0),
                0
            );
        }
        with_state(|state| state.moh_result = 7);
        assert_eq!(
            indicate(
                owner.as_ptr(),
                ffi::AST_CONTROL_HOLD as c_int,
                c"music".as_ptr().cast(),
                6
            ),
            7
        );
        for condition in [
            ffi::AST_CONTROL_UNHOLD,
            ffi::AST_CONTROL_PROCEEDING,
            ffi::AST_CONTROL_PROGRESS,
        ] {
            assert_eq!(
                indicate(owner.as_ptr(), condition as c_int, ptr::null(), 0),
                0
            );
        }
        for tone in [b"bad".as_slice(), &[b'1'; 32]] {
            assert_eq!(
                indicate(
                    owner.as_ptr(),
                    ffi::AST_CONTROL_RADIO_KEY as c_int,
                    tone.as_ptr().cast(),
                    tone.len()
                ),
                -1
            );
        }
        for (tone, length) in [
            (ptr::null(), 0),
            (c"ignored".as_ptr(), 0),
            (c"123.0".as_ptr(), 5),
        ] {
            assert_eq!(
                indicate(
                    owner.as_ptr(),
                    ffi::AST_CONTROL_RADIO_KEY as c_int,
                    tone.cast(),
                    length
                ),
                0
            );
        }
        assert_eq!(
            indicate(
                owner.as_ptr(),
                ffi::AST_CONTROL_RADIO_UNKEY as c_int,
                ptr::null(),
                0
            ),
            0
        );
        for result in [-6, URP_AST_CHANNEL_BUSY] {
            calls.lock().unwrap().result = result;
            assert_eq!(
                indicate(
                    owner.as_ptr(),
                    ffi::AST_CONTROL_RADIO_UNKEY as c_int,
                    ptr::null(),
                    0
                ),
                -1
            );
        }
        calls.lock().unwrap().result = URP_AST_OK;
        assert_eq!(indicate(owner.as_ptr(), 999, ptr::null(), 0), -1);
        let gate = control_gate().write().unwrap();
        assert_eq!(
            indicate(
                owner.as_ptr(),
                ffi::AST_CONTROL_RADIO_KEY as c_int,
                c"100.0".as_ptr().cast(),
                5
            ),
            0
        );
        assert_eq!(
            indicate(
                owner.as_ptr(),
                ffi::AST_CONTROL_RADIO_UNKEY as c_int,
                ptr::null(),
                0
            ),
            0
        );
        drop(gate);
    }
    assert_eq!(
        calls.lock().unwrap().transmit,
        [(1, 0), (1, 0), (1, 1230), (0, 0), (0, 0), (0, 0)]
    );
    calls.lock().unwrap().result = -6;
    assert_eq!(replay_transmit(&channel), -6);
    assert_ne!(channel.pending_transmit.load(Ordering::Acquire), 0);
    calls.lock().unwrap().result = URP_AST_OK;
    assert_eq!(replay_transmit(&channel), URP_AST_OK);
    assert_eq!(replay_transmit(&channel), URP_AST_OK);
    assert_eq!(calls.lock().unwrap().transmit.len(), 8);
    with_state(|state| {
        assert_eq!(state.moh_started, [("music".into(), "default".into())]);
        assert_eq!(state.moh_stopped, 3);
    });
}

#[test]
fn owner_callbacks_publish_state_and_retry_failed_jitter() {
    let _fixture = Fixture::new();
    let descriptor = callback_descriptor();
    let calls = Mutex::new(DriverCalls::default());
    let (mut channel, mut owner) = callback_channel(&descriptor, &calls);
    let mut replacement = FakeChannel::new(ptr::null_mut());
    // SAFETY: all opaque owners have stable fixture storage and live tech_pvt.
    unsafe {
        assert_eq!(fixup(owner.as_ptr(), replacement.as_ptr()), -1);
        replacement.tech_pvt = ptr::from_mut(&mut *channel) as usize;
        assert_eq!(fixup(owner.as_ptr(), replacement.as_ptr()), 0);
        assert_eq!(channel.owner.load(Ordering::Acquire), replacement.as_ptr());
        assert_eq!(answer(replacement.as_ptr()), 0);
        assert_eq!(replacement.state, ffi::AST_STATE_UP as c_int);
        assert_eq!(digit_begin(owner.as_ptr(), b'1' as c_char), 0);
        assert_eq!(digit_end(owner.as_ptr(), b'1' as c_char, 50), 0);
        channel.owner.store(ptr::null_mut(), Ordering::Release);
        assert!(lock_owner(&channel).is_none());
        assert!(configure_jitter(&channel).is_err());
        channel.owner.store(replacement.as_ptr(), Ordering::Release);
        with_state(|state| state.trylock_result = 1);
        assert!(lock_owner(&channel).is_none());
        with_state(|state| state.trylock_result = 0);
    }
    mark_jitter_pending(&channel);
    calls.lock().unwrap().result = -6;
    configure_pending_jitter(&channel);
    assert!(channel.jitter_pending.load(Ordering::Acquire));
    calls.lock().unwrap().result = URP_AST_OK;
    configure_pending_jitter(&channel);
    configure_pending_jitter(&channel);
    assert!(!channel.jitter_pending.load(Ordering::Acquire));
    with_state(|state| {
        assert_eq!(state.unlocks, 2);
        assert_eq!(state.jitter.len(), 1);
        assert_eq!(state.jitter[0].0, replacement.as_ptr() as usize);
        assert_eq!(state.jitter[0].1.max_size, 80);
        assert_eq!(state.jitter[0].1.flags, ffi::AST_JB_ENABLED);
        assert!(state.messages.iter().any(|(kind, _, message)| *kind == 2
            && message.contains("received digit 1 of duration 50 ms")));
    });
    let channel_address = ptr::from_ref(&*channel) as usize;
    with_state(|state| {
        state.trylock_hook = Some(Box::new(move || {
            // SAFETY: this synchronous one-shot hook runs while the Box remains live.
            unsafe { &*(channel_address as *const Channel) }
                .owner
                .store(ptr::null_mut(), Ordering::Release);
        }))
    });
    // SAFETY: the owner is live; the hook simulates publication changing during trylock.
    assert!(unsafe { lock_owner(&channel) }.is_none());
    with_state(|state| assert_eq!(state.unlocks, 3));
}

#[repr(C)]
struct DirectV2 {
    struct_size: u32,
    abi_version: u32,
    receive_context: *mut c_void,
    receive: Option<unsafe extern "C" fn(*mut c_void, u32, *mut f32, u32) -> c_int>,
    transmit_context: *mut c_void,
    transmit: Option<unsafe extern "C" fn(*mut c_void, *mut f32, u32, *mut u32) -> c_int>,
    accepted_abi_version: u32,
}

unsafe extern "C" fn retain_direct(
    channel: *mut c_void,
    _: *const crate::UrpAstDirectCallbacks,
) -> c_int {
    // SAFETY: the fixture supplies a live retention result until reply arrives.
    unsafe { *channel.cast::<c_int>() }
}

#[test]
fn direct_attachment_acknowledges_only_valid_retained_descriptor() {
    let _fixture = Fixture::new();
    let callbacks = crate::tests::direct_callbacks();
    // SAFETY: descriptor fields are integers, raw pointers, and optional callbacks.
    let mut descriptor: UrpAstDescriptor = unsafe { zeroed() };
    descriptor.channel_set_direct_callbacks = Some(retain_direct);
    let mut retention = URP_AST_OK;
    let mut channel = Channel {
        _name: "usb".into(),
        descriptor: &descriptor,
        rust_channel: AtomicPtr::new(ptr::from_mut(&mut retention).cast()),
        control: ptr::null_mut(),
        dsp: ptr::null_mut(),
        format: ptr::null_mut(),
        sample_rate_hz: ADVANCED_RATE_HZ,
        frame_samples: 960,
        owner: AtomicPtr::new(ptr::null_mut()),
        worker: Mutex::new(None),
        delivery_stop: AtomicBool::new(false),
        service_failed: AtomicBool::new(false),
        jitter_pending: AtomicBool::new(false),
        pending_transmit: AtomicU64::new(0),
        direct: AtomicBool::new(false),
    };
    let mut owner = FakeChannel::new(ptr::from_mut(&mut channel).cast());
    let mut value = 0u8;
    // SAFETY: the fake owner and its writable byte remain live through the call.
    let unsupported =
        unsafe { setoption(owner.as_ptr(), 12345, ptr::from_mut(&mut value).cast(), 1) };
    assert_eq!(unsupported, -1, "unsupported option must fail");
    // SAFETY: Linux provides a valid pointer to this thread's errno.
    assert_eq!(unsafe { *libc::__errno_location() }, libc::ENOSYS);
    for case in 0..9 {
        retention = if case == 7 { -1 } else { URP_AST_OK };
        channel
            .rust_channel
            .store(ptr::from_mut(&mut retention).cast(), Ordering::Release);
        let mut direct = DirectV2 {
            struct_size: size_of::<DirectV2>() as u32,
            abi_version: 2,
            receive_context: callbacks.receive_context,
            receive: callbacks.receive,
            transmit_context: callbacks.transmit_context,
            transmit: callbacks.transmit,
            accepted_abi_version: 0,
        };
        let mut length = size_of::<DirectV2>() as c_int;
        match case {
            0 => direct.struct_size -= 1,
            1 => direct.abi_version = 1,
            2 => direct.receive = None,
            3 => direct.transmit = None,
            4 => direct.receive_context = ptr::null_mut(),
            5 => direct.transmit_context = ptr::null_mut(),
            6 => length -= 1,
            _ => {}
        }
        // The host option must also accept byte-aligned Asterisk payloads.
        let mut storage = vec![0u8; size_of::<DirectV2>() + 1];
        // SAFETY: the allocation includes the byte offset and complete descriptor.
        let data = unsafe { storage.as_mut_ptr().add(1).cast::<DirectV2>() };
        // SAFETY: the byte buffer reserves a complete, possibly unaligned descriptor.
        unsafe { data.write_unaligned(direct) };
        // SAFETY: the fake owner, descriptor, and retention context remain live.
        let result = unsafe {
            setoption(
                owner.as_ptr(),
                crate::URP_AST_OPTION_DIRECT_CALLBACKS,
                data.cast(),
                length,
            )
        };
        // SAFETY: setoption returned synchronously; the complete buffer remains live.
        let accepted = unsafe { data.read_unaligned().accepted_abi_version };
        assert_eq!(accepted, if case == 8 { 2 } else { 0 }, "case {case}");
        assert_eq!(result == 0, case == 8, "case {case}");
    }
}

#[test]
fn direct_call_starts_station_without_delivery_worker() {
    for direct in [true, false] {
        let events = RefCell::new(Vec::new());
        assert!(
            start_media(
                direct,
                || {
                    events.borrow_mut().push("start");
                    URP_AST_OK
                },
                || {
                    events.borrow_mut().push("delivery");
                    Ok(())
                },
                || {
                    events.borrow_mut().push("stop");
                },
            )
            .is_ok()
        );
        let expected: &[&str] = if direct {
            &["start"]
        } else {
            &["start", "delivery"]
        };
        assert_eq!(*events.borrow(), expected);
    }
}

#[test]
fn failed_delivery_start_stops_the_station() {
    let events = RefCell::new(Vec::new());
    assert!(
        start_media(
            false,
            || {
                events.borrow_mut().push("start");
                URP_AST_OK
            },
            || {
                events.borrow_mut().push("delivery");
                Err(())
            },
            || {
                events.borrow_mut().push("stop");
            },
        )
        .is_err()
    );
    assert_eq!(*events.borrow(), ["start", "delivery", "stop"]);
}

#[test]
fn direct_option_rejects_wrong_technology_and_payload_length() {
    let callbacks = crate::tests::direct_callbacks();
    let data = ptr::from_ref(&callbacks).cast_mut().cast();
    let length = size_of::<super::super::super::UrpAstDirectCallbacks>() as c_int;
    // SAFETY: complete copied descriptor remains live throughout validation.
    unsafe {
        assert!(direct_option(APP_RPT_RATE_HZ, data, length).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length - 1).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length + 1).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, ptr::null_mut(), length).is_err());
        assert!(direct_option(ADVANCED_RATE_HZ, data, length).is_ok());
    }
}
#[test]
fn technologies_keep_existing_rates_and_transports() {
    assert_eq!(
        technology_contract("radioplus"),
        Some(TechnologyContract {
            transport: URP_AST_TRANSPORT_APP_RPT,
            sample_rate_hz: 8_000,
        })
    );
    assert_eq!(
        technology_contract("RADIOPLUSADVANCED"),
        Some(TechnologyContract {
            transport: URP_AST_TRANSPORT_RPT_ADVANCED,
            sample_rate_hz: 48_000,
        })
    );
    assert_eq!(technology_contract("unknown"), None);
}

#[test]
fn channel_names_must_be_nonempty_and_fit_the_adapter_abi() {
    assert_eq!(channel_name_length(b""), None);
    assert_eq!(channel_name_length(b"alpha"), Some(5));
}

#[test]
fn reservation_failures_keep_the_existing_operator_diagnostics() {
    assert_eq!(
        reservation_failure_diagnostic("RadioPlus", b"alpha", URP_AST_CHANNEL_NOT_FOUND),
        (
            ffi::__LOG_WARNING as c_int,
            "RadioPlus/alpha: Rust channel was not configured".into()
        )
    );
    assert_eq!(
        reservation_failure_diagnostic("RadioPlusAdvanced", b"bravo", -6),
        (
            ffi::__LOG_ERROR as c_int,
            "RadioPlusAdvanced/bravo: Rust channel reservation failed (-6)".into()
        )
    );
}

#[test]
fn reservation_failures_reach_asterisk_with_channel_and_status() {
    let _fixture = Fixture::new();
    log_reservation_failure("RadioPlus", b"alpha", URP_AST_CHANNEL_NOT_FOUND);
    log_reservation_failure("RadioPlusAdvanced", b"bravo", -6);
    with_state(|state| {
        assert_eq!(
            state.messages,
            [
                (
                    0,
                    ffi::__LOG_WARNING as c_int,
                    "RadioPlus/alpha: Rust channel was not configured\n".into(),
                ),
                (
                    0,
                    ffi::__LOG_ERROR as c_int,
                    "RadioPlusAdvanced/bravo: Rust channel reservation failed (-6)\n".into(),
                ),
            ]
        );
    });
}

#[test]
fn native_format_must_have_the_exact_required_rate() {
    let format = std::ptr::dangling_mut::<u8>();

    assert!(format_has_rate(format, 48_000, |_| 48_000));
    assert!(!format_has_rate(format, 48_000, |_| 44_100));
    assert!(!format_has_rate(ptr::null_mut::<u8>(), 48_000, |_| 48_000));
}

#[test]
fn failed_jitter_configuration_remains_pending_for_retry() {
    let pending = AtomicBool::new(true);
    assert_eq!(apply_pending_jitter(&pending, || Err(())), Err(()));
    assert!(pending.load(Ordering::Acquire));

    assert_eq!(apply_pending_jitter(&pending, || Ok(())), Ok(()));
    assert!(!pending.load(Ordering::Acquire));
}

#[test]
fn forced_ctcss_matches_decimal_boundary_contract() {
    assert_eq!(forced_ctcss_tenths_hz(b""), Ok(0));
    assert_eq!(forced_ctcss_tenths_hz(b"100.04"), Ok(1_000));
    assert_eq!(forced_ctcss_tenths_hz(b"100.05"), Ok(1_001));
    assert_eq!(forced_ctcss_tenths_hz(b"-1"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"nan"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"not-a-tone"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"429496729.6"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"\xff"), Err(()));
}

#[test]
fn forced_ctcss_accepts_only_one_optional_trailing_nul() {
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0"), Ok(1_000));
    assert_eq!(forced_ctcss_tenths_hz(b"100\0.0"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0x"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"100.0\0\0"), Err(()));
    assert_eq!(forced_ctcss_tenths_hz(b"\0"), Err(()));
}

#[test]
fn voice_frames_must_match_the_fixed_technology_contract() {
    assert!(voice_frame_is_valid(
        ffi::AST_FRAME_VOICE,
        320,
        160,
        false,
        160,
    ));
    assert!(!voice_frame_is_valid(
        ffi::AST_FRAME_TEXT,
        320,
        160,
        false,
        160,
    ));
    assert!(!voice_frame_is_valid(
        ffi::AST_FRAME_VOICE,
        319,
        160,
        false,
        160,
    ));
    assert!(!voice_frame_is_valid(
        ffi::AST_FRAME_VOICE,
        320,
        159,
        false,
        160,
    ));
    assert!(!voice_frame_is_valid(
        ffi::AST_FRAME_VOICE,
        320,
        160,
        true,
        160,
    ));
}

#[test]
fn pending_transmit_keeps_the_latest_complete_intent() {
    let keyed = PendingTransmit::new(true, 1_230);
    assert_eq!(keyed.decode(), Some((true, 1_230)));
    let unkeyed = PendingTransmit::new(false, 0);
    assert_eq!(unkeyed.decode(), Some((false, 0)));
    assert_eq!(PendingTransmit::NONE.decode(), None);
}

#[test]
fn only_reload_admission_contention_defers_transmit() {
    let pending = AtomicU64::new(PendingTransmit::NONE.raw());
    let intent = PendingTransmit::new(true, 1_230);

    assert_eq!(
        finish_transmit_attempt(&pending, intent, TransmitAttempt::Deferred),
        0
    );
    assert_eq!(pending.load(Ordering::Acquire), intent.raw());

    pending.store(PendingTransmit::NONE.raw(), Ordering::Release);
    assert_eq!(
        finish_transmit_attempt(
            &pending,
            intent,
            TransmitAttempt::Completed(URP_AST_CHANNEL_BUSY)
        ),
        -1
    );
    assert_eq!(pending.load(Ordering::Acquire), PendingTransmit::NONE.raw());
}

#[test]
fn request_keeps_membership_frozen_through_initial_jitter_setup() {
    let events = RefCell::new(Vec::new());

    let succeeded = finish_request_jitter(
        || {
            events.borrow_mut().push("configure");
            Err(())
        },
        || events.borrow_mut().push("unlock"),
        || events.borrow_mut().push("release-membership"),
        || events.borrow_mut().push("hangup"),
    );

    assert!(!succeeded);
    assert_eq!(
        *events.borrow(),
        ["configure", "unlock", "release-membership", "hangup"]
    );
}

#[test]
fn hangup_releases_owner_before_waiting_for_membership() {
    let events = RefCell::new(Vec::new());

    let membership = handoff_hangup_lock(
        || events.borrow_mut().push("unpublish"),
        || events.borrow_mut().push("unlock-owner"),
        || {
            events.borrow_mut().push("lock-membership");
            17
        },
        || events.borrow_mut().push("relock-owner"),
    );

    assert_eq!(membership, 17);
    assert_eq!(
        *events.borrow(),
        [
            "unpublish",
            "unlock-owner",
            "lock-membership",
            "relock-owner"
        ]
    );
}

#[test]
fn unregister_rechecks_activity_after_request_construction_finishes() {
    let membership = Arc::new(Mutex::new(()));
    let active = Arc::new(AtomicUsize::new(0));
    let construction = membership.lock().expect("membership lock poisoned");
    let (started_sender, started_receiver) = sync_channel(0);
    let (result_sender, result_receiver) = sync_channel(0);
    let worker_membership = Arc::clone(&membership);
    let worker_active = Arc::clone(&active);

    let worker = std::thread::spawn(move || {
        started_sender.send(()).expect("start signal failed");
        let result = match lock_idle_membership(&worker_membership, &worker_active) {
            Ok(membership) => {
                drop(membership);
                Ok(())
            }
            Err(status) => Err(status),
        };
        result_sender.send(result).expect("result signal failed");
    });
    started_receiver.recv().expect("start signal failed");
    active.store(1, Ordering::Release);
    drop(construction);

    assert_eq!(
        result_receiver.recv().expect("result signal failed"),
        Err(URP_AST_CHANNEL_BUSY)
    );
    worker.join().expect("worker panicked");
}

#[test]
fn registration_gate_blocks_requests_until_rollback_finishes() {
    let membership = Arc::new(Mutex::new(()));
    let active = AtomicUsize::new(0);
    let registration =
        lock_idle_membership(&membership, &active).expect("registration gate failed");
    let (started_sender, started_receiver) = sync_channel(0);
    let (acquired_sender, acquired_receiver) = sync_channel(0);
    let request_membership = Arc::clone(&membership);

    let request = std::thread::spawn(move || {
        started_sender.send(()).expect("start signal failed");
        let _request = request_membership.lock().expect("membership lock poisoned");
        acquired_sender.send(()).expect("acquired signal failed");
    });
    started_receiver.recv().expect("start signal failed");
    assert_eq!(
        acquired_receiver.try_recv(),
        Err(std::sync::mpsc::TryRecvError::Empty)
    );

    drop(registration);
    acquired_receiver.recv().expect("acquired signal failed");
    request.join().expect("request panicked");
}

#[test]
fn channel_host_requires_every_operation_it_invokes() {
    assert!(descriptor_is_valid(crate::product_descriptor()));
    macro_rules! reject_missing {
        ($field:ident) => {{
            // SAFETY: the descriptor contains only plain ABI fields and
            // function pointers, so this copy owns no dropped resource.
            let mut incomplete = unsafe { std::ptr::read(crate::product_descriptor()) };
            incomplete.$field = None;
            assert!(!descriptor_is_valid(&incomplete));
        }};
    }
    for corrupt in [0_u8, 1] {
        // SAFETY: see the macro comment above.
        let mut incomplete = unsafe { std::ptr::read(crate::product_descriptor()) };
        if corrupt == 0 {
            incomplete.struct_size = 0;
        } else {
            incomplete.abi_version = 0;
        }
        assert!(!descriptor_is_valid(&incomplete));
    }
    reject_missing!(driver_reload);
    reject_missing!(driver_reload_finish);
    reject_missing!(driver_channel_name);
    reject_missing!(driver_active_channel);
    reject_missing!(driver_set_active_channel);
    reject_missing!(channel_reserve);
    reject_missing!(channel_start);
    reject_missing!(channel_stop);
    reject_missing!(channel_reload_prepare);
    reject_missing!(channel_reload_activate);
    reject_missing!(channel_reload_finish);
    reject_missing!(channel_write_voice);
    reject_missing!(channel_write_text);
    reject_missing!(channel_set_transmit);
    reject_missing!(channel_set_dtmf);
    reject_missing!(channel_set_echo);
    reject_missing!(channel_set_direct_callbacks);
    reject_missing!(channel_get_jitter_config);
    reject_missing!(channel_command);
    reject_missing!(channel_get_status);
    reject_missing!(channel_service);
    reject_missing!(channel_destroy);
}

#[test]
fn jitter_mapping_preserves_every_asterisk_setting() {
    let resolved = UrpAstJitterConfig {
        struct_size: size_of::<UrpAstJitterConfig>() as u32,
        abi_version: ABI_VERSION,
        enabled: 1,
        maximum_size_ms: 500,
        resync_threshold_ms: 1_000,
        implementation: URP_AST_JITTER_FIXED,
        logging_enabled: 1,
        force_enabled: 1,
        target_extra_ms: 40,
        video_sync_enabled: 1,
    };
    let mapped = jitter_configuration(resolved);
    assert_eq!(
        mapped.flags,
        ffi::AST_JB_ENABLED | ffi::AST_JB_FORCED | ffi::AST_JB_LOG | ffi::AST_JB_SYNC_VIDEO
    );
    assert_eq!(mapped.max_size, 500);
    assert_eq!(mapped.resync_threshold, 1_000);
    assert_eq!(mapped.target_extra, 40);
    assert_eq!(
        mapped
            .impl_
            .iter()
            .map(|byte| *byte as u8)
            .take_while(|byte| *byte != 0)
            .collect::<Vec<_>>(),
        b"fixed"
    );

    let mapped = jitter_configuration(UrpAstJitterConfig {
        implementation: URP_AST_JITTER_ADAPTIVE,
        enabled: 2,
        force_enabled: 2,
        logging_enabled: 2,
        video_sync_enabled: 2,
        ..resolved
    });
    assert_eq!(
        mapped.flags,
        ffi::AST_JB_ENABLED | ffi::AST_JB_FORCED | ffi::AST_JB_LOG | ffi::AST_JB_SYNC_VIDEO
    );
    assert_eq!(
        mapped
            .impl_
            .iter()
            .map(|byte| *byte as u8)
            .take_while(|byte| *byte != 0)
            .collect::<Vec<_>>(),
        b"adaptive"
    );
    let disabled = jitter_configuration(UrpAstJitterConfig {
        enabled: 0,
        force_enabled: 0,
        logging_enabled: 0,
        video_sync_enabled: 0,
        ..resolved
    });
    assert_eq!(disabled.flags, 0);
}
