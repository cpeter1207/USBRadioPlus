use super::*;
use crate::control::tests::factory;
use crate::media::tests::radio_provider;
use crate::runtime::tests::{remote_dsp_render, runtime};

fn target() -> RemoteOperatingState {
    RemoteOperatingState {
        mode: 1,
        decode: usbradioplus_core::RemoteSignal {
            source: usbradioplus_core::RemoteSignalSource::Dsp,
            enabled: true,
            tone_tenths_hz: 1000,
            ..Default::default()
        },
        ..Default::default()
    }
}
fn exchange() -> Arc<Exchange> {
    Arc::new(Exchange {
        target: target(),
        external_decode: SubaudibleSource::Disabled,
        carrier: usbradioplus_core::CarrierSource::Dsp,
        preparation: ArrayQueue::new(1),
        ready: ArrayQueue::new(1),
        result: ArrayQueue::new(1),
        completed: AtomicBool::new(false),
        cancelled: AtomicBool::new(false),
    })
}
fn enqueue(dsp: &RemoteDspControl, exchange: &Arc<Exchange>) {
    assert!(dsp.port.pending.push(Arc::clone(exchange)).is_ok());
}

#[test]
fn remote_dsp_keeps_the_first_completion_and_observes_worker_stop() {
    let exchange = exchange();
    exchange.complete(Err(RemoteDspError::Cancelled));
    exchange.complete(Ok(()));
    assert!(matches!(
        exchange.result.pop(),
        Some(Err(RemoteDspError::Cancelled))
    ));
    assert!(exchange.result.pop().is_none());
    let dsp = RemoteDspControl::default();
    assert!(!dsp.awaiting_callbacks());
    let port = dsp.port();
    let worker = std::thread::spawn(move || {
        port.apply(
            target(),
            SubaudibleSource::Disabled,
            usbradioplus_core::CarrierSource::Dsp,
            Duration::from_secs(2),
            || false,
        )
    });
    let deadline = Instant::now() + Duration::from_secs(2);
    while !dsp.is_busy() {
        assert!(Instant::now() < deadline);
        std::thread::yield_now();
    }
    drop(dsp);
    assert!(matches!(
        worker.join().unwrap(),
        Err(RemoteDspError::Stopped)
    ));
}

#[test]
fn remote_dsp_does_not_capture_a_foreign_control_owner() {
    let (mut radio, _control) = runtime(40);
    let (_foreign, mut foreign_control) = runtime(41);
    let mut dsp = RemoteDspControl::new();
    let exchange = exchange();
    enqueue(&dsp, &exchange);
    dsp.pump(
        &mut radio,
        &mut foreign_control,
        &factory(),
        radio_provider(),
    );
    assert!(matches!(
        exchange.result.pop(),
        Some(Err(RemoteDspError::Update(
            StationUpdateError::Incompatible
        )))
    ));
    assert!(!dsp.is_busy());
}

#[test]
fn remote_dsp_recaptures_after_an_ordinary_update_wins_the_slots() {
    let (mut radio, mut control) = runtime(40);
    radio.start().unwrap();
    let original = control.plan().clone();
    let mut dsp = RemoteDspControl::new();
    let exchange = exchange();
    enqueue(&dsp, &exchange);
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(dsp.active.as_ref().unwrap().prepared);
    // No ready result yet: slow graph construction must leave control nonblocking.
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(!dsp.awaiting_callbacks());
    let preparation = exchange.preparation.pop().unwrap();
    let candidate = preparation
        .plan()
        .clone()
        .with_remote_signaling(&target(), SubaudibleSource::Disabled)
        .unwrap();
    let update = preparation.prepare(candidate).unwrap();
    assert!(exchange.ready.push(Ok(update)).is_ok());
    let ordinary = radio
        .prepare_update(&control, original.clone(), &factory(), radio_provider())
        .unwrap();
    radio.begin_update(&control, ordinary).unwrap();
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(!dsp.active.as_ref().unwrap().prepared);
    // While that ordinary update owns the slots, a fresh snapshot waits too.
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(!dsp.active.as_ref().unwrap().prepared);
    remote_dsp_render(&mut radio, true);
    radio.finish_update(&mut control, true).unwrap();
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(dsp.active.as_ref().unwrap().prepared);
    exchange.cancelled.store(true, Ordering::Release);
    dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
    assert!(!dsp.is_busy());
}

#[test]
fn remote_dsp_reports_preparation_and_callback_failures_and_retains_rollback() {
    for (generation, invalid) in [(40, true), (94, false)] {
        let (mut radio, mut control) = runtime(generation);
        radio.start().unwrap();
        let original = control.plan().clone();
        let mut dsp = RemoteDspControl::new();
        let port = dsp.port();
        let mut requested = target();
        if invalid {
            requested.decode.tone_tenths_hz = u32::MAX;
        }
        let worker = std::thread::spawn(move || {
            port.apply(
                requested,
                SubaudibleSource::Disabled,
                usbradioplus_core::CarrierSource::Dsp,
                Duration::from_secs(2),
                || false,
            )
        });
        let deadline = Instant::now() + Duration::from_secs(2);
        while !worker.is_finished() {
            assert!(Instant::now() < deadline);
            dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
            remote_dsp_render(&mut radio, true);
            std::thread::yield_now();
        }
        assert!(matches!(
            worker.join().unwrap(),
            Err(RemoteDspError::Update(_))
        ));
        while dsp.is_busy() {
            assert!(Instant::now() < deadline);
            remote_dsp_render(&mut radio, true);
            dsp.pump(&mut radio, &mut control, &factory(), radio_provider());
        }
        assert_eq!(control.plan(), &original);
    }
}
