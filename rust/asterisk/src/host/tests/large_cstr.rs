//! A real ABI-overflow C string backed by one small, sealed shared allocation.

use std::ffi::c_char;
use std::io::Write;
use std::os::fd::{AsRawFd, FromRawFd};
use std::ptr;

pub(crate) struct OversizedCString {
    base: *mut libc::c_void,
    size: usize,
}

impl OversizedCString {
    pub(crate) fn new() -> Self {
        const BLOCK: usize = 1024 * 1024;
        const LENGTH: usize = u32::MAX as usize + 1;
        // SAFETY: sysconf has no pointer argument; this matrix uses native Linux.
        let page = usize::try_from(unsafe { libc::sysconf(libc::_SC_PAGESIZE) }).unwrap();
        assert_eq!(LENGTH % page, 0);
        // SAFETY: the name is terminated and the returned descriptor is checked.
        let fd = unsafe { libc::memfd_create(c"urp-large-cstr".as_ptr(), libc::MFD_ALLOW_SEALING) };
        assert!(fd >= 0);
        // SAFETY: fd is newly owned and File closes it on every exit path.
        let mut backing = unsafe { std::fs::File::from_raw_fd(fd) };
        backing.write_all(&vec![b'x'; BLOCK]).unwrap();
        assert_eq!(
            // SAFETY: the owned writable descriptor has no writable mappings.
            unsafe {
                libc::fcntl(
                    fd,
                    libc::F_ADD_SEALS,
                    libc::F_SEAL_WRITE
                        | libc::F_SEAL_GROW
                        | libc::F_SEAL_SHRINK
                        | libc::F_SEAL_SEAL,
                )
            },
            0
        );
        let size = LENGTH + page;
        // SAFETY: reserve a fresh region; it is not dereferenced until populated.
        let base = unsafe {
            libc::mmap(
                ptr::null_mut(),
                size,
                libc::PROT_NONE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            )
        };
        assert_ne!(base, libc::MAP_FAILED);
        let result = Self { base, size };
        for offset in (0..LENGTH).step_by(BLOCK) {
            // SAFETY: each MAP_FIXED range lies wholly inside our reservation.
            let address = unsafe { base.cast::<u8>().add(offset) }.cast();
            assert_eq!(
                // SAFETY: map the sealed nonzero backing into the reserved block.
                unsafe {
                    libc::mmap(
                        address,
                        BLOCK,
                        libc::PROT_READ,
                        libc::MAP_SHARED | libc::MAP_FIXED,
                        backing.as_raw_fd(),
                        0,
                    )
                },
                address
            );
        }
        // SAFETY: the final reserved page follows exactly LENGTH nonzero bytes.
        let terminator = unsafe { base.cast::<u8>().add(LENGTH) }.cast();
        assert_eq!(
            // SAFETY: an anonymous mapping provides the readable zero terminator.
            unsafe {
                libc::mmap(
                    terminator,
                    page,
                    libc::PROT_READ,
                    libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                    -1,
                    0,
                )
            },
            terminator
        );
        result
    }

    pub(crate) fn as_ptr(&self) -> *const c_char {
        self.base.cast()
    }
}

impl Drop for OversizedCString {
    fn drop(&mut self) {
        // SAFETY: this object exclusively owns the complete reserved region.
        assert_eq!(unsafe { libc::munmap(self.base, self.size) }, 0);
    }
}
