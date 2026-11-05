use super::*;
use std::mem::{offset_of, size_of};

#[test]
fn remote_dynamic_client_admits_only_complete_current_contracts() {
    assert!(RemoteProvider::load_from("/missing-rptadv-fixture.so").is_err());
    let provider = RemoteProvider::load().unwrap();
    assert_eq!(provider.api().abi_version, 1);
    let good = *provider.api();
    for offset in [
        offset_of!(raw::RptAdvRemoteApi, create),
        offset_of!(raw::RptAdvRemoteApi, destroy),
        offset_of!(raw::RptAdvRemoteApi, configured_memory),
        offset_of!(raw::RptAdvRemoteApi, request),
        offset_of!(raw::RptAdvRemoteApi, advance),
        offset_of!(raw::RptAdvRemoteApi, snapshot),
        offset_of!(raw::RptAdvRemoteApi, take_completion),
        offset_of!(raw::RptAdvRemoteApi, retire),
        offset_of!(raw::RptAdvRemoteApi, scan_snapshot),
    ] {
        let mut bad = good;
        // SAFETY: each listed field is a nullable C function pointer in this owned record.
        unsafe {
            (&raw mut bad)
                .cast::<u8>()
                .add(offset)
                .cast::<usize>()
                .write(0);
        }
        // SAFETY: the owned record remains readable for descriptor admission.
        assert!(unsafe { admit_remote(&bad) }.is_err());
    }
    for (size, abi) in [
        (0, 1),
        (size_of::<raw::RptAdvRemoteApi>() as u32 - 1, 1),
        (good.struct_size, 2),
    ] {
        let mut bad = good;
        bad.struct_size = size;
        bad.abi_version = abi;
        // SAFETY: the complete record is readable even when its advertised prefix is rejected.
        assert!(unsafe { admit_remote(&bad) }.is_err());
    }
}

#[test]
fn hamlib_dynamic_client_preserves_its_separate_boundary() {
    assert!(HamlibProvider::load_from("/missing-hamlib-fixture.so").is_err());
    let provider = HamlibProvider::load().unwrap();
    let api = provider.api();
    assert_eq!(api.abi_version, 1);
    // SAFETY: complete admitted immutable descriptor remains loaded by provider.
    assert!(unsafe { api.model_count.unwrap()() } > 0);
    let mut bad = *api;
    bad.struct_size -= 1;
    // SAFETY: the complete owned record remains readable during admission.
    assert!(unsafe { admit_hamlib(&bad) }.is_err());
    bad = *api;
    bad.get = None;
    // SAFETY: the complete owned record remains readable during admission.
    assert!(unsafe { admit_hamlib(&bad) }.is_err());
}
