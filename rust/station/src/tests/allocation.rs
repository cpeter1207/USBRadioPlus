//! Per-thread heap guard for warmed callback paths; other tests remain independent.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static OPERATIONS: Cell<usize> = const { Cell::new(0) };
}

struct GuardedAllocator;

fn note() {
    if ARMED.try_with(Cell::get).unwrap_or(false) {
        let _ = OPERATIONS.try_with(|count| count.set(count.get() + 1));
    }
}

// SAFETY: every request is forwarded unchanged to the platform allocator.
unsafe impl GlobalAlloc for GuardedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: the allocator caller supplies the standard allocation contract.
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        note();
        // SAFETY: pointer/layout originate from the forwarded allocator.
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note();
        // SAFETY: the allocator caller supplies a valid allocation layout.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        note();
        // SAFETY: the caller supplies an owned allocation and valid resized layout.
        unsafe { System.realloc(pointer, layout, size) }
    }
}

#[global_allocator]
static ALLOCATOR: GuardedAllocator = GuardedAllocator;

pub(crate) fn without_heap(call: impl FnOnce()) {
    OPERATIONS.set(0);
    ARMED.set(true);
    call();
    ARMED.set(false);
    assert_eq!(
        OPERATIONS.get(),
        0,
        "warmed feedback callback touched the Rust heap"
    );
}
