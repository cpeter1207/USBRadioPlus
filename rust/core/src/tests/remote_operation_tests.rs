use super::*;

#[test]
fn remote_operation_admits_only_complete_known_requests() {
    let mut request = RemoteOperation::new(RemoteOperationKind::SetSetting);
    request.selector = 100;
    request.magnitude = 250_000;
    request.decimal_places = 6;
    assert_eq!(request.kind(), Some(RemoteOperationKind::SetSetting));
    for field in 0..3 {
        let mut invalid = request;
        match field {
            0 => invalid.struct_size -= 1,
            1 => invalid.abi_version += 1,
            _ => invalid.operation = u32::MAX,
        }
        assert_eq!(invalid.kind(), None);
    }
    assert_eq!(std::mem::size_of::<RemoteOperation>(), 40);
}

#[test]
fn remote_operation_reserves_only_inhibit_and_stop_for_urgent_admission() {
    for selector in 1..=27 {
        let kind = RemoteOperationKind::from_selector(selector).unwrap();
        let mut operation = RemoteOperation::new(kind);
        assert_eq!(operation.urgent(), kind == RemoteOperationKind::StopScan);
        operation.flags = 1;
        assert_eq!(
            operation.urgent(),
            matches!(
                kind,
                RemoteOperationKind::StopScan | RemoteOperationKind::Inhibit
            )
        );
    }
    assert!(RemoteOperationKind::from_selector(0).is_none());
    assert!(RemoteOperationKind::from_selector(28).is_none());
}
