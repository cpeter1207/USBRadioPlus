//! Reject incompatible products before any operation can consume their handles.

use super::*;
use crate::host::support::product_support;
use std::ptr;

#[test]
fn product_validation_rejects_null_short_and_incompatible_headers() {
    // SAFETY: a null descriptor must be rejected without a read.
    assert!(!unsafe { product_descriptor_is_valid(ptr::null()) });
    let short = size_of::<u32>() as u32;
    // SAFETY: only the advertised size word exists; validation must stop at that word.
    assert!(!unsafe { product_descriptor_is_valid(ptr::from_ref(&short).cast()) });
    let valid = product_support::descriptor();
    // SAFETY: the fixture retains the complete shared-library descriptor for process lifetime.
    assert!(unsafe { product_descriptor_is_valid(valid) });
    for case in 0..4 {
        let mut descriptor = *valid;
        match case {
            0 => descriptor.struct_size -= 1,
            1 => descriptor.abi_version += 1,
            2 => descriptor.capability_name = ptr::null(),
            _ => descriptor.capability_name = c"wrong.product".as_ptr(),
        }
        assert!(
            // SAFETY: this complete copy and its static capability remain live through validation.
            !unsafe { product_descriptor_is_valid(&descriptor) },
            "case {case}"
        );
    }
}

#[test]
fn product_validation_requires_every_operation_the_host_can_invoke() {
    let valid = product_support::descriptor();
    macro_rules! reject_missing {
        ($field:ident) => {
            let mut incomplete = *valid;
            incomplete.$field = None;
            assert!(
                // SAFETY: the complete descriptor copy and static strings outlive validation.
                !unsafe { product_descriptor_is_valid(&incomplete) },
                stringify!($field)
            );
        };
    }
    reject_missing!(driver_create);
    reject_missing!(driver_reload);
    reject_missing!(driver_reload_finish);
    reject_missing!(driver_channel_name);
    reject_missing!(driver_active_channel);
    reject_missing!(driver_set_active_channel);
    reject_missing!(driver_destroy);
    reject_missing!(link_prepare);
    reject_missing!(link_prepare_reload);
    reject_missing!(link_reload_unchanged);
    reject_missing!(link_process);
    reject_missing!(link_observe);
    reject_missing!(link_destroy);
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
