//! Deterministic Asterisk timer/framehook lifecycle boundary for host tests.

use super::*;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(crate) enum Failure {
    #[default]
    None,
    Open,
    Fd,
    Rate,
    Register,
    Attach,
    Ack,
}

struct Timer {
    rate: u32,
    fd: c_int,
}

#[derive(Default)]
struct Owner {
    fds: Vec<c_int>,
    selected: c_int,
    hooks: HashMap<c_int, (usize, bool)>,
    next_id: c_int,
}

#[derive(Default)]
struct TimingState {
    owners: HashMap<usize, Owner>,
    timers: HashMap<usize, Box<Timer>>,
    next_fd: c_int,
    failure: Failure,
    opens: usize,
    closes: usize,
    acknowledgements: usize,
    continuous: bool,
}

fn timing<T>(action: impl FnOnce(&mut TimingState) -> T) -> T {
    static TIMING: OnceLock<Mutex<TimingState>> = OnceLock::new();
    action(
        &mut TIMING
            .get_or_init(|| Mutex::new(TimingState::default()))
            .lock()
            .unwrap(),
    )
}

pub(crate) fn fail(failure: Failure) {
    timing(|state| state.failure = failure);
}
pub(crate) fn counts() -> (usize, usize, usize) {
    timing(|state| (state.opens, state.closes, state.acknowledgements))
}
pub(crate) fn continuous(value: bool) {
    timing(|state| state.continuous = value);
}
pub(crate) fn rates() -> Vec<u32> {
    timing(|state| state.timers.values().map(|timer| timer.rate).collect())
}
pub(crate) fn select_timer(channel: *mut ffi::ast_channel) {
    timing(|state| {
        let owner = state.owners.get_mut(&(channel as usize)).unwrap();
        owner.selected = owner.fds.iter().position(|fd| *fd >= 0).unwrap() as c_int;
    });
}

pub(crate) fn drain(channel: *mut ffi::ast_channel) {
    let detached = timing(|state| {
        let Some(owner) = state.owners.get_mut(&(channel as usize)) else {
            return Vec::new();
        };
        let ids: Vec<_> = owner
            .hooks
            .iter()
            .filter_map(|(id, (_, pending))| pending.then_some(*id))
            .collect();
        ids.into_iter()
            .map(|id| owner.hooks.remove(&id).unwrap().0)
            .collect::<Vec<_>>()
    });
    for address in detached {
        // SAFETY: each stored interface is owned until its one terminal event and destructor.
        unsafe {
            let interface = Box::from_raw(address as *mut ffi::ast_framehook_interface);
            interface.event_cb.unwrap()(
                channel,
                ptr::null_mut(),
                ffi::AST_FRAMEHOOK_EVENT_DETACHED,
                interface.data,
            );
            if let Some(destroy) = interface.destroy_cb {
                destroy(interface.data);
            }
        }
    }
}

pub(crate) fn destroy(channel: *mut ffi::ast_channel) {
    timing(|state| {
        if let Some(owner) = state.owners.get_mut(&(channel as usize)) {
            for (_, pending) in owner.hooks.values_mut() {
                *pending = true;
            }
        }
    });
    drain(channel);
}

pub(crate) fn reset() {
    let owners = timing(|state| state.owners.keys().copied().collect::<Vec<_>>());
    for owner in owners {
        destroy(owner as *mut ffi::ast_channel);
    }
    timing(|state| {
        assert!(state.timers.is_empty());
        *state = TimingState::default();
    });
}

pub(crate) fn event(
    channel: *mut ffi::ast_channel,
    frame: *mut ffi::ast_frame,
    event: ffi::ast_framehook_event,
    timer: bool,
) -> *mut ffi::ast_frame {
    drain(channel);
    let interfaces = timing(|state| {
        let Some(owner) = state.owners.get_mut(&(channel as usize)) else {
            return Vec::new();
        };
        owner.selected = if timer {
            owner
                .fds
                .iter()
                .position(|fd| *fd >= 0)
                .map_or(-1, |slot| slot as c_int)
        } else {
            -1
        };
        let mut hooks = owner
            .hooks
            .iter()
            .map(|(id, (address, _))| (*id, *address))
            .collect::<Vec<_>>();
        hooks.sort_unstable_by_key(|(id, _)| *id);
        hooks
            .into_iter()
            .map(|(_, address)| address)
            .collect::<Vec<_>>()
    });
    let mut frame = frame;
    let mut skip = vec![false; interfaces.len()];
    loop {
        let original = frame;
        for (index, address) in interfaces.iter().enumerate() {
            if skip[index] {
                continue;
            }
            // SAFETY: the fixture retains the interface throughout Asterisk's restartable traversal.
            let next = unsafe {
                let interface = &*(*address as *const ffi::ast_framehook_interface);
                interface.event_cb.unwrap()(channel, frame, event, interface.data)
            };
            if next != frame {
                skip[index] = true;
                frame = next;
                break;
            }
        }
        if frame == original {
            break;
        }
    }
    frame
}

pub(crate) fn masquerade(
    old: *mut ffi::ast_channel,
    new: *mut ffi::ast_channel,
    after_copy: impl FnOnce(),
) {
    let interfaces = timing(|state| {
        let fds = state.owners.get(&(old as usize)).unwrap().fds.clone();
        state.owners.entry(new as usize).or_default().fds = fds;
        let recipients = state.owners[&(new as usize)]
            .hooks
            .iter()
            .map(|(id, (address, _))| (*id, *address))
            .collect::<Vec<_>>();
        let incoming = state
            .owners
            .get_mut(&(old as usize))
            .unwrap()
            .hooks
            .drain()
            .map(|(_, (address, _))| address)
            .collect::<Vec<_>>();
        (recipients, incoming)
    });
    after_copy();
    for (id, address) in interfaces.0 {
        // SAFETY: Asterisk invokes recipient breakdown after copying FD slots.
        unsafe {
            let interface = &*(address as *const ffi::ast_framehook_interface);
            if let Some(callback) = interface.chan_breakdown_cb {
                callback(interface.data, id, old, new);
            }
        }
    }
    for address in interfaces.1 {
        // SAFETY: mirror ATTACHED(new), fixup, DETACHED(old) without destroying preserved data.
        unsafe {
            let mut interface = Box::from_raw(address as *mut ffi::ast_framehook_interface);
            let id = ast_framehook_attach(new, interface.as_mut());
            if id >= 0 {
                interface.chan_fixup_cb.unwrap()(interface.data, id, old, new);
            }
            interface.event_cb.unwrap()(
                old,
                ptr::null_mut(),
                ffi::AST_FRAMEHOOK_EVENT_DETACHED,
                interface.data,
            );
            if id < 0 {
                interface.destroy_cb.unwrap()(interface.data);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_open() -> *mut ffi::ast_timer {
    timing(|state| {
        if state.failure == Failure::Open {
            return ptr::null_mut();
        }
        state.next_fd += 1;
        let mut timer = Box::new(Timer {
            rate: 0,
            fd: 100 + state.next_fd,
        });
        let address = ptr::from_mut(timer.as_mut()) as usize;
        state.timers.insert(address, timer);
        state.opens += 1;
        address as *mut ffi::ast_timer
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_close(timer: *mut ffi::ast_timer) {
    timing(|state| {
        assert!(state.timers.remove(&(timer as usize)).is_some());
        state.closes += 1;
    });
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_fd(timer: *const ffi::ast_timer) -> c_int {
    timing(|state| {
        if state.failure == Failure::Fd {
            -1
        } else {
            state.timers[&(timer as usize)].fd
        }
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_set_rate(
    timer: *const ffi::ast_timer,
    rate: u32,
) -> c_int {
    timing(|state| {
        if rate != 0 && state.failure == Failure::Rate {
            return -1;
        }
        state.timers.get_mut(&(timer as usize)).unwrap().rate = rate;
        0
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_get_event(
    _: *const ffi::ast_timer,
) -> ffi::ast_timer_event {
    timing(|state| {
        if state.continuous {
            ffi::AST_TIMING_EVENT_CONTINUOUS
        } else {
            ffi::AST_TIMING_EVENT_EXPIRED
        }
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_timer_ack(_: *const ffi::ast_timer, quantity: u32) -> c_int {
    assert_eq!(quantity, 1);
    timing(|state| {
        state.acknowledgements += 1;
        if state.failure == Failure::Ack { -1 } else { 0 }
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_channel_fd_add(
    channel: *mut ffi::ast_channel,
    fd: c_int,
) -> c_int {
    timing(|state| {
        if state.failure == Failure::Register {
            return -1;
        }
        let owner = state.owners.entry(channel as usize).or_default();
        if let Some(slot) = owner.fds.iter().position(|value| *value < 0) {
            owner.fds[slot] = fd;
            slot as c_int
        } else {
            owner.fds.push(fd);
            owner.fds.len() as c_int - 1
        }
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_channel_fd(
    channel: *const ffi::ast_channel,
    slot: c_int,
) -> c_int {
    timing(|state| {
        state
            .owners
            .get(&(channel as usize))
            .and_then(|owner| owner.fds.get(slot as usize))
            .copied()
            .unwrap_or(-1)
    })
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_channel_fdno(channel: *const ffi::ast_channel) -> c_int {
    timing(|state| state.owners[&(channel as usize)].selected)
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_channel_fdno_set(channel: *mut ffi::ast_channel, value: c_int) {
    timing(|state| state.owners.get_mut(&(channel as usize)).unwrap().selected = value);
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_channel_set_fd(
    channel: *mut ffi::ast_channel,
    slot: c_int,
    fd: c_int,
) {
    timing(|state| state.owners.get_mut(&(channel as usize)).unwrap().fds[slot as usize] = fd);
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_framehook_attach(
    channel: *mut ffi::ast_channel,
    interface: *mut ffi::ast_framehook_interface,
) -> c_int {
    // SAFETY: caller supplies a complete interface; Asterisk copies it before returning.
    let interface = unsafe { *interface };
    let id = timing(|state| {
        if state.failure == Failure::Attach {
            return -1;
        }
        let owner = state.owners.entry(channel as usize).or_default();
        owner.next_id += 1;
        owner.hooks.insert(
            owner.next_id,
            (Box::into_raw(Box::new(interface)) as usize, false),
        );
        owner.next_id
    });
    if id >= 0 {
        // SAFETY: ATTACHED is synchronous and the data remains retained.
        unsafe {
            interface.event_cb.unwrap()(
                channel,
                ptr::null_mut(),
                ffi::AST_FRAMEHOOK_EVENT_ATTACHED,
                interface.data,
            );
        }
    }
    id
}
#[unsafe(no_mangle)]
pub(crate) unsafe extern "C" fn ast_framehook_detach(
    channel: *mut ffi::ast_channel,
    id: c_int,
) -> c_int {
    timing(|state| {
        let Some((_, pending)) = state
            .owners
            .get_mut(&(channel as usize))
            .and_then(|owner| owner.hooks.get_mut(&id))
        else {
            return -1;
        };
        *pending = true;
        0
    })
}
