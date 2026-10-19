use super::*;

#[test]
fn site_status_distinguishes_invalid_busy_unavailable_and_failed_operations() {
    for (failure, status) in [
        (
            HardwareStationError::Site(SiteError::InvalidMask),
            Status::InvalidArgument,
        ),
        (
            HardwareStationError::Site(SiteError::Busy),
            Status::ChannelBusy,
        ),
        (HardwareStationError::ControlQueueFull, Status::ChannelBusy),
        (
            HardwareStationError::Site(SiteError::Unavailable),
            Status::NotReady,
        ),
        (
            HardwareStationError::Site(SiteError::StaleGeneration),
            Status::NotReady,
        ),
        (HardwareStationError::NotRunning, Status::NotReady),
        (
            HardwareStationError::Site(SiteError::Fault),
            Status::SetupFailed,
        ),
        (HardwareStationError::WorkerPanicked, Status::SetupFailed),
    ] {
        assert_eq!(error(failure).code(), status.code());
    }
}

#[test]
fn site_snapshot_reports_wire_errors_quality_and_missing_acquisition() {
    for (failure, code) in [
        (None, 0),
        (Some(SiteError::Unavailable), 1),
        (Some(SiteError::StaleGeneration), 2),
        (Some(SiteError::InvalidMask), 3),
        (Some(SiteError::Busy), 4),
        (Some(SiteError::Fault), 5),
    ] {
        for (quality, wire_quality) in [
            (SiteReadQuality::Unavailable, 0),
            (SiteReadQuality::Valid, 1),
            (SiteReadQuality::Fault, 2),
        ] {
            let wire = UrpAstSiteSnapshot::from(SiteDigitalSnapshot {
                input_quality: quality,
                last_command_error: failure,
                ..SiteDigitalSnapshot::default()
            });
            assert_eq!(wire.last_command_error, code);
            assert_eq!(wire.input_quality, wire_quality);
            assert_eq!(wire.observed_at_valid, 0);
            assert_eq!(wire.observed_at_ms, 0);
        }
    }
}
