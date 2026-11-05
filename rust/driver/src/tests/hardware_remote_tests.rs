use super::*;

struct PhysicalFixture {
    generation: RemoteGeneration,
    worker: ServiceWorker,
    devices: ServiceDevices,
    outputs: OutputState,
    // Keep shared fake-device state protected through every owned destructor.
    _guard: std::sync::MutexGuard<'static, ()>,
}

impl PhysicalFixture {
    fn new(ptt: &str) -> Self {
        Self::configured(ptt, RemoteBackend::Hamlib)
    }

    fn configured(ptt: &str, backend: RemoteBackend) -> Self {
        use super::super::tests as fixture;
        let guard = fixture::test_guard();
        fixture::reset_test_state();
        let (mut plan, selected) = fixture::selected_hardware(backend != RemoteBackend::Hamlib);
        let mut configuration = crate::remote_request::tests::configuration();
        configuration.backend = backend;
        if backend == RemoteBackend::Hamlib {
            configuration.memories[0].state.power_mw = Some(5000);
        } else {
            plan.parallel.as_mut().unwrap().config.output_enable_mask = 0xff;
            plan.parallel.as_mut().unwrap().ptt_mask = 8;
            if backend == RemoteBackend::ParallelChannel {
                configuration.memories[0].state.channel = Some(7);
            }
        }
        configuration.ptt_source = Some(ptt.into());
        let (setup, control) = RemoteSetup::prepare(
            configuration,
            plan.clone(),
            CarrierSource::Dsp,
            usbradioplus_radio::QualificationConfig::default(),
        )
        .unwrap();
        drop(control);
        let (worker, _sender, _events) =
            fixture::service_worker(&plan, &selected, SharedHardwareState::default());
        let mut devices =
            ServiceDevices::open(fixture::gpio_provider(), &selected, plan.parallel.as_ref())
                .unwrap();
        let mut outputs = OutputState::new(&selected, plan.parallel.as_ref(), None);
        let generation = RemoteGeneration::open(
            setup,
            Arc::new(RemoteQueue::new(4)),
            &worker,
            &mut devices,
            &mut outputs,
        )
        .unwrap();
        Self {
            generation,
            worker,
            devices,
            outputs,
            _guard: guard,
        }
    }

    fn backend(&mut self) -> PhysicalBackend<'_> {
        PhysicalBackend {
            setup: &self.generation.setup,
            rig: &mut self.generation.rig,
            wiring: self.generation.wiring,
            worker: &self.worker,
            devices: &mut self.devices,
            outputs: &mut self.outputs,
            current: &mut self.generation.current,
            encode_gate: &mut self.generation.encode_gate,
            queue: &self.generation.queue,
            source: None,
        }
    }

    fn finish(mut self) {
        self.worker.stop.store(false, Ordering::Release);
        self.generation
            .retire(&self.worker, &mut self.devices, &mut self.outputs)
            .unwrap();
    }
}

#[test]
fn physical_parallel_rejects_rf_valid_changes_it_cannot_program() {
    // Each change stays in the configured amateur band. Fixed-channel metadata
    // and the RTX register grid impose additional physical limits at application.
    let mut fixture = PhysicalFixture::configured("usb", RemoteBackend::ParallelChannel);
    let original = fixture.generation.current;
    for field in 0..4 {
        let mut state = original;
        match field {
            0 => state.channel = 8,
            1 => state.receive_hz += 12_500,
            2 => state.transmit_hz += 12_500,
            _ => state.offset_hz += 12_500,
        }
        assert_eq!(fixture.backend().apply_state(&state), Err(CallError(-6)));
    }
    fixture.finish();
    let mut fixture = PhysicalFixture::configured("parallel", RemoteBackend::ParallelRtx);
    let mut state = fixture.generation.current;
    state.receive_hz += 1;
    assert_eq!(fixture.backend().apply_state(&state), Err(CallError(-6)));
    assert_eq!(
        fixture.backend().transmit_epoch(),
        fixture.worker.state.remote_activity().transmit_epoch
    );
    fixture.finish();
}

#[test]
fn physical_callbacks_observe_stop_urgent_and_revoked_sources_without_io() {
    use usbradioplus_core::remote_operation::{RemoteOperation as O, RemoteOperationKind as K};
    for reason in 0..3 {
        let mut fixture = PhysicalFixture::new("hamlib");
        let state = fixture.generation.current;
        let queue = Arc::clone(&fixture.generation.queue);
        let mut operation = O::new(K::Status);
        if reason == 0 {
            fixture.worker.stop.store(true, Ordering::Release);
        }
        if reason == 1 {
            operation = O::new(K::Inhibit);
            operation.flags = 1;
        }
        let ticket = queue.submit(operation, true, false).unwrap();
        let work = (reason == 2).then(|| queue.next_ordinary().unwrap());
        if reason == 2 {
            ticket.source_state(false, false);
        }
        let mut backend = fixture.backend();
        backend.source = work.as_ref().map(|work| work.source_lease());
        assert_eq!(backend.read_state().err(), Some(CallError(-10)));
        assert_eq!(backend.apply_state(&state), Err(CallError(-10)));
        fixture.finish();
    }
}

#[test]
fn physical_getters_require_observed_power_context_and_confirm_both_ptt_routes() {
    for ptt in ["usb", "hamlib"] {
        let mut fixture = PhysicalFixture::new(ptt);
        let mut snapshot = fixture.generation.snapshot().unwrap();
        let state = fixture.generation.current;
        let mut backend = fixture.backend();
        assert_eq!(backend.get_setting(11, &snapshot), Err(CallError(-6)));
        assert_eq!(backend.apply_state(&state), Err(CallError(-1102)));
        snapshot.observed = backend.read_state().unwrap();
        snapshot.observed_fields = 2 | 4 | 64 | 256;
        assert!(backend.get_setting(1, &snapshot).is_ok());
        assert!(backend.get_setting(11, &snapshot).is_ok());
        snapshot.observed.flags |= 32;
        snapshot.observed.xit_hz = 100;
        assert!(backend.get_setting(11, &snapshot).is_ok());
        backend.set_tune(true).unwrap();
        backend.set_tune(false).unwrap();
        backend.set_ptt(true).unwrap();
        assert_eq!(backend.transmit_idle(), Ok(false));
        backend.set_ptt(false).unwrap();
        assert_eq!(backend.transmit_idle(), Ok(true));
        use super::super::tests as device;
        device::FAILURE.store(device::FAIL_CM119_STATISTICS, Ordering::Release);
        assert_eq!(backend.set_ptt(true), Err(CallError(-1101)));
        backend.set_ptt(false).unwrap();
        backend.set_ptt(true).unwrap();
        device::FAILURE.store(device::FAIL_CM119_STATISTICS, Ordering::Release);
        assert_eq!(backend.set_ptt(false), Err(CallError(-1101)));
        backend.set_ptt(false).unwrap();
        backend.set_setting(16, 0).unwrap();
        fixture.finish();
    }
}

#[test]
fn physical_startup_remains_pending_while_a_previous_carrier_is_keyed() {
    let mut fixture = PhysicalFixture::new("hamlib");
    let state = fixture.generation.current;
    let mut backend = fixture.backend();
    assert_eq!(backend.apply_state(&state), Err(CallError(-1102)));
    backend.set_ptt(true).unwrap();
    assert_eq!(
        fixture
            .generation
            .service(&fixture.worker, &mut fixture.devices, &mut fixture.outputs),
        Ok(())
    );
    assert_ne!(
        fixture.generation.snapshot().unwrap().flags & raw::RPTADV_REMOTE_PENDING,
        0
    );
    assert!(!fixture.generation.queue.ready());
    fixture.finish();
}

#[test]
fn physical_ptt_requires_matching_feedback() {
    for (actual, keyed) in [(0, false), (1, false), (0, true), (1, true)] {
        assert_eq!(
            confirm_ptt(actual, keyed),
            if (actual != 0) == keyed {
                Ok(())
            } else {
                Err(CallError(-1101))
            }
        );
    }
}

#[test]
fn recorded_hardware_failure_wins_without_hiding_a_new_policy_failure() {
    assert_eq!(first_cause(CallError(-10), -1102), CallError(-1102));
    assert_eq!(first_cause(CallError(-5), 0), CallError(-5));
}

#[test]
fn rig_controls_require_both_programming_and_enable_operations() {
    assert!(!writable(
        &[crate::test_support::setting(20, true, false)],
        20
    ));
    for mask in 0..32 {
        let settings: Vec<_> = [11, 12, 16, 13, 17]
            .into_iter()
            .enumerate()
            .map(|(bit, selector)| {
                crate::test_support::setting(selector, true, mask & (1 << bit) != 0)
            })
            .collect();
        let mut caps = SelectedCapabilities {
            encode_sources: 2,
            decode_control_sources: 2,
            ..SelectedCapabilities::default()
        };
        add_rig_controls(&mut caps, &settings);
        assert_eq!(caps.power_mw, mask & 1 != 0);
        assert_eq!(caps.encode_sources, 2 | (u32::from(mask & 6 == 6) << 3));
        assert_eq!(
            caps.decode_control_sources,
            2 | (u32::from(mask & 24 == 24) << 3)
        );
    }
}

#[test]
fn dsp_fault_mapping_preserves_distinct_terminal_causes() {
    use usbradioplus_station::{RemoteDspError as E, StationUpdateError};
    for (error, code) in [
        (E::Busy, -9),
        (E::Cancelled, -10),
        (E::Stopped, -1102),
        (E::Timeout, -1103),
        (E::Update(StationUpdateError::Incompatible), -1104),
    ] {
        assert_eq!(dsp_failure(error), CallError(code));
    }
}

#[test]
fn coverage_preserves_declared_power_bounds_without_inventing_missing_limits() {
    for (minimum, maximum, flags) in [(-1, -1, 0), (0, -1, 1), (-1, 5000, 2), (0, 5000, 3)] {
        let range = cover(raw::rptadv_hamlib_range {
            struct_size: size_of::<raw::rptadv_hamlib_range>(),
            minimum_hz: 144_000_000,
            maximum_hz: 148_000_000,
            modes: 2,
            minimum_power_mw: minimum,
            maximum_power_mw: maximum,
        });
        assert_eq!((range.minimum, range.maximum), (144_000_000, 148_000_000));
        assert_eq!((range.mode_mask, range.flags), (2, flags));
        assert_eq!(range.minimum_power_mw, minimum.max(0) as u32);
        assert_eq!(range.maximum_power_mw, maximum.max(0) as u32);
    }
}

#[test]
fn unknown_physical_ptt_blocks_new_rf_work() {
    assert!(physical_busy(None));
    assert!(physical_busy(Some(true)));
    assert!(!physical_busy(Some(false)));
}
#[test]
fn non_fm_half_duplex_receive_waits_for_confirmed_physical_unkey() {
    for mode in [1, 3] {
        for duplex in [false, true] {
            for physical in [None, Some(false), Some(true)] {
                assert_eq!(
                    receive_allowed(true, mode, duplex, physical),
                    mode == 1 || duplex || physical == Some(false)
                );
                assert!(!receive_allowed(false, mode, duplex, physical));
            }
        }
    }
}
