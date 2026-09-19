use super::*;
use std::sync::{Arc, Mutex};

impl EchoConfiguration {
    const fn new(enabled: bool, maximum_frames: u16) -> Option<Self> {
        if maximum_frames <= MAX_ECHO_FRAMES {
            Some(Self {
                enabled,
                maximum_frames,
            })
        } else {
            None
        }
    }
}

#[derive(Clone, Copy)]
enum ConverterBehavior {
    Downsample,
    DropFirst,
    Fail,
    Stall,
    InvalidInputCount,
    InvalidOutputCount,
    Flood,
}

struct FakeConverter {
    behavior: ConverterBehavior,
    phase: usize,
    dropped: bool,
}

impl FakeConverter {
    fn boxed(behavior: ConverterBehavior) -> Box<dyn AppRptConverter> {
        Box::new(Self {
            behavior,
            phase: 0,
            dropped: false,
        })
    }
}

impl AppRptConverter for FakeConverter {
    fn process(
        &mut self,
        input: &[f32],
        output: &mut [f32],
    ) -> Result<ConversionProgress, ConversionError> {
        match self.behavior {
            ConverterBehavior::Fail => return Err(ConversionError),
            ConverterBehavior::Stall => return Ok(ConversionProgress::default()),
            ConverterBehavior::InvalidInputCount => {
                return Ok(ConversionProgress {
                    input_used: input.len() + 1,
                    output_generated: 0,
                });
            }
            ConverterBehavior::InvalidOutputCount => {
                return Ok(ConversionProgress {
                    input_used: input.len(),
                    output_generated: output.len() + 1,
                });
            }
            ConverterBehavior::Flood => {
                output.fill(0.25);
                return Ok(ConversionProgress {
                    input_used: input.len(),
                    output_generated: output.len(),
                });
            }
            ConverterBehavior::Downsample | ConverterBehavior::DropFirst => {}
        }

        let mut generated = 0;
        for sample in input {
            self.phase += 1;
            if self.phase == 6 {
                self.phase = 0;
                if matches!(self.behavior, ConverterBehavior::DropFirst) && !self.dropped {
                    self.dropped = true;
                } else {
                    output[generated] = *sample;
                    generated += 1;
                }
            }
        }
        Ok(ConversionProgress {
            input_used: input.len(),
            output_generated: generated,
        })
    }
}

#[derive(Clone, Copy)]
enum ProducerBehavior {
    All,
    Prefix(usize),
    Fail,
    OverReport,
}

#[derive(Default)]
struct ProgramLog {
    pushes: Vec<Vec<f32>>,
    qualifications: Vec<ReceiveQualification>,
}

struct FakeProducer {
    behavior: ProducerBehavior,
    log: Arc<Mutex<ProgramLog>>,
}

impl ProgramProducer for FakeProducer {
    fn push(
        &mut self,
        input: &[f32],
        receive: ReceiveQualification,
    ) -> Result<usize, ProgramProducerError> {
        let mut log = self.log.lock().unwrap();
        log.pushes.push(input.to_vec());
        log.qualifications.push(receive);
        drop(log);
        match self.behavior {
            ProducerBehavior::All => Ok(input.len()),
            ProducerBehavior::Prefix(count) => Ok(count.min(input.len())),
            ProducerBehavior::Fail => Err(ProgramProducerError),
            ProducerBehavior::OverReport => Ok(input.len() + 1),
        }
    }
}

fn producer(behavior: ProducerBehavior) -> (Box<dyn ProgramProducer>, Arc<Mutex<ProgramLog>>) {
    let log = Arc::new(Mutex::new(ProgramLog::default()));
    (
        Box::new(FakeProducer {
            behavior,
            log: Arc::clone(&log),
        }),
        log,
    )
}

fn tone(value: u16) -> CtcssTone {
    CtcssTone::from_tenths_hz(value).unwrap()
}

fn publish_advanced(
    publisher: &mut ReceivePublisher,
    sample: f32,
    metadata: ReceiveMetadata,
) -> PublishReport {
    publisher
        .publish(&[sample; ADVANCED_FRAME_SAMPLES], metadata)
        .unwrap()
}

fn drain(state: &mut ControllerState) -> usize {
    let mut count = 0;
    while next(state).0.is_some() {
        count += 1;
    }
    count
}

fn next(state: &mut ControllerState) -> (Option<DeliveryAction>, ControllerPcmFrame) {
    let mut voice = ControllerPcmFrame::silence(state.mode);
    let action = state.next_action(&mut voice);
    (action, voice)
}

#[test]
fn setup_is_typed_and_bounded() {
    assert_eq!(CtcssToneIndex::new(38), None);
    assert_eq!(CtcssToneIndex::new(37).unwrap().get(), 37);
    let (program, _) = producer(ProducerBehavior::All);
    assert!(matches!(
        ControllerState::prepare_advanced(program, 1),
        Err(AdapterSetupError::Handoff(HandoffError::InvalidSlotCount))
    ));
    let (program, _) = producer(ProducerBehavior::All);
    let (publisher, state) = ControllerState::prepare_advanced(program, 4).unwrap();
    assert_eq!(publisher.observe(), HandoffObservation::default());
    assert_eq!(state.handoff_observation(), HandoffObservation::default());
    assert_eq!(state.status_observation(), StatusObservation::default());
    assert!(
        !AdapterSetupError::Handoff(HandoffError::InvalidSlotCount)
            .to_string()
            .is_empty()
    );
}

#[test]
fn advanced_receive_assembles_and_orders_state_status_voice() {
    let (program, program_log) = producer(ProducerBehavior::All);
    let (mut publisher, mut state) = ControllerState::prepare_advanced(program, 4).unwrap();
    assert_eq!(
        publisher.publish(&[], ReceiveMetadata::default()),
        Err(AdapterError::InvalidNativeFrame)
    );
    assert_eq!(
        publisher.publish(
            &[0.0; ADVANCED_FRAME_SAMPLES + 1],
            ReceiveMetadata::default()
        ),
        Err(AdapterError::InvalidNativeFrame)
    );

    let first = ReceiveMetadata {
        statuses: [Some(ReceiveStatus::VoterRssi(123)), None],
        ..ReceiveMetadata::default()
    };
    assert_eq!(
        publisher.publish(&[0.5; 480], first).unwrap(),
        PublishReport::default()
    );
    let qualification = ReceiveQualification {
        carrier_active: true,
        subaudible_active: true,
        receiver_keyed: true,
        ctcss_decoded: Some(CtcssToneIndex::new(3).unwrap()),
        dcs_valid: false,
    };
    let second = ReceiveMetadata {
        qualification,
        controller_ctcss: Some(tone(1_000)),
        statuses: [Some(ReceiveStatus::TransmitCtcssReady(tone(885))), None],
    };
    assert_eq!(
        publisher.publish(&[1.0; 480], second).unwrap(),
        PublishReport {
            completed_frame: true,
            handoff: Some(PublishOutcome::Published),
        }
    );
    assert_eq!(
        next(&mut state).0,
        Some(DeliveryAction::ReceiverKey {
            ctcss: Some(tone(1_000))
        })
    );
    assert_eq!(state.latest_receive_qualification(), qualification);
    assert_eq!(next(&mut state).0, Some(DeliveryAction::VoterRssi(123)));
    assert_eq!(
        next(&mut state).0,
        Some(DeliveryAction::TransmitCtcssReady(tone(885)))
    );
    let (action, frame) = next(&mut state);
    assert_eq!(action, Some(DeliveryAction::Voice));
    assert_eq!(frame.mode(), AsteriskPcmMode::Advanced);
    let samples = frame.samples();
    assert_eq!(&samples[..480], &[16_384; 480]);
    assert_eq!(&samples[480..], &[i16::MAX; 480]);
    assert_eq!(next(&mut state).0, None);
    state
        .write_program(&ControllerPcmFrame::advanced([1; ADVANCED_FRAME_SAMPLES]))
        .unwrap();
    assert_eq!(
        program_log.lock().unwrap().qualifications,
        vec![qualification]
    );

    publish_advanced(&mut publisher, 0.75, ReceiveMetadata::default());
    assert_eq!(next(&mut state).0, Some(DeliveryAction::ReceiverUnkey));
    let (action, frame) = next(&mut state);
    assert_eq!(action, Some(DeliveryAction::Voice));
    assert_eq!(frame.mode(), AsteriskPcmMode::Advanced);
    assert_eq!(frame.samples(), &[0; ADVANCED_FRAME_SAMPLES]);
    assert_eq!(next(&mut state).0, None);
}

#[test]
fn newest_audio_carries_the_bounded_status_journal() {
    let (program, _) = producer(ProducerBehavior::All);
    let (mut publisher, mut state) = ControllerState::prepare_advanced(program, 2).unwrap();
    for value in 0..18 {
        publish_advanced(
            &mut publisher,
            0.0,
            ReceiveMetadata {
                statuses: [Some(ReceiveStatus::VoterRssi(value)), None],
                ..ReceiveMetadata::default()
            },
        );
    }
    for expected in 2..18 {
        assert_eq!(
            next(&mut state).0,
            Some(DeliveryAction::VoterRssi(expected))
        );
    }
    assert_eq!(next(&mut state).0, Some(DeliveryAction::Voice));
    assert_eq!(next(&mut state).0, None);
    assert_eq!(
        state.status_observation(),
        StatusObservation {
            next_sequence: 18,
            discarded: 2,
        }
    );
    assert_eq!(state.handoff_observation().discarded, 17);
}

#[test]
fn app_rpt_converter_preserves_callback_partitioning_and_startup_padding() {
    let (program, _) = producer(ProducerBehavior::All);
    let (mut publisher, mut state) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        4,
        EchoConfiguration::disabled(),
    )
    .unwrap();
    assert!(
        !publisher
            .publish(
                &[0.25; 479],
                ReceiveMetadata {
                    qualification: ReceiveQualification {
                        receiver_keyed: true,
                        ..ReceiveQualification::default()
                    },
                    ..ReceiveMetadata::default()
                }
            )
            .unwrap()
            .completed_frame
    );
    assert!(
        publisher
            .publish(
                &[0.25; 481],
                ReceiveMetadata {
                    qualification: ReceiveQualification {
                        receiver_keyed: true,
                        ..ReceiveQualification::default()
                    },
                    ..ReceiveMetadata::default()
                }
            )
            .unwrap()
            .completed_frame
    );
    assert!(matches!(
        next(&mut state).0,
        Some(DeliveryAction::ReceiverKey { .. })
    ));
    let (action, frame) = next(&mut state);
    assert_eq!(action, Some(DeliveryAction::Voice));
    assert_eq!(frame.mode(), AsteriskPcmMode::AppRpt);
    assert_eq!(frame.samples(), &[8_192; APP_RPT_FRAME_SAMPLES]);

    let (program, _) = producer(ProducerBehavior::All);
    let (mut publisher, mut state) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::DropFirst),
        program,
        2,
        EchoConfiguration::disabled(),
    )
    .unwrap();
    publisher
        .publish(
            &[0.5; ADVANCED_FRAME_SAMPLES],
            ReceiveMetadata {
                qualification: ReceiveQualification {
                    receiver_keyed: true,
                    ..ReceiveQualification::default()
                },
                ..ReceiveMetadata::default()
            },
        )
        .unwrap();
    let _ = next(&mut state);
    let (action, frame) = next(&mut state);
    assert_eq!(action, Some(DeliveryAction::Voice));
    assert_eq!(frame.mode(), AsteriskPcmMode::AppRpt);
    let samples = frame.samples();
    assert_eq!(&samples[..159], &[16_384; 159]);
    assert_eq!(samples[159], 0);
}

#[test]
fn converter_contract_failures_are_bounded() {
    for (behavior, expected) in [
        (
            ConverterBehavior::Fail,
            AdapterError::SampleRate(ConversionError),
        ),
        (ConverterBehavior::Stall, AdapterError::ConversionStalled),
        (
            ConverterBehavior::InvalidInputCount,
            AdapterError::InvalidConversionResult,
        ),
        (
            ConverterBehavior::InvalidOutputCount,
            AdapterError::InvalidConversionResult,
        ),
    ] {
        let (program, _) = producer(ProducerBehavior::All);
        let (mut publisher, _) = ControllerState::prepare_app_rpt(
            FakeConverter::boxed(behavior),
            program,
            2,
            EchoConfiguration::disabled(),
        )
        .unwrap();
        assert_eq!(
            publisher.publish(&[0.0; 1], ReceiveMetadata::default()),
            Err(expected)
        );
    }

    let (program, _) = producer(ProducerBehavior::All);
    let (mut publisher, _) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Flood),
        program,
        2,
        EchoConfiguration::disabled(),
    )
    .unwrap();
    assert!(
        publisher
            .publish(&[0.0; ADVANCED_FRAME_SAMPLES], ReceiveMetadata::default())
            .is_ok()
    );
    assert!(
        publisher
            .publish(&[0.0; ADVANCED_FRAME_SAMPLES], ReceiveMetadata::default())
            .is_ok()
    );
    assert_eq!(
        publisher.publish(&[0.0; ADVANCED_FRAME_SAMPLES], ReceiveMetadata::default()),
        Err(AdapterError::Pcm(PcmBoundaryError::AssemblyOverflow))
    );
}

#[test]
fn one_program_owner_substitutes_only_active_legacy_echo() {
    let (program, log) = producer(ProducerBehavior::All);
    let echo = EchoConfiguration::new(true, 1).unwrap();
    let (mut publisher, mut state) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        4,
        echo,
    )
    .unwrap();
    assert!(state.echo_enabled());
    assert!(!state.echo_playing());

    publisher
        .publish(
            &[0.5; ADVANCED_FRAME_SAMPLES],
            ReceiveMetadata {
                qualification: ReceiveQualification {
                    carrier_active: true,
                    subaudible_active: true,
                    receiver_keyed: true,
                    ctcss_decoded: Some(CtcssToneIndex::new(7).unwrap()),
                    dcs_valid: true,
                },
                ..ReceiveMetadata::default()
            },
        )
        .unwrap();
    assert_eq!(drain(&mut state), 2);
    publisher
        .publish(&[0.25; ADVANCED_FRAME_SAMPLES], ReceiveMetadata::default())
        .unwrap();
    assert_eq!(drain(&mut state), 2);
    assert!(state.echo_playing());

    let controller = ControllerPcmFrame::app_rpt([1_000; APP_RPT_FRAME_SAMPLES]);
    assert_eq!(
        state.write_program(&controller).unwrap(),
        ProgramWrite {
            source: ProgramSource::LegacyEcho,
            submitted_samples: APP_RPT_FRAME_SAMPLES,
            accepted_samples: APP_RPT_FRAME_SAMPLES,
            echo_completed: true,
        }
    );
    assert!(!state.echo_playing());
    assert_eq!(
        state.write_program(&controller).unwrap().source,
        ProgramSource::Controller
    );
    {
        let log = log.lock().unwrap();
        assert_eq!(log.pushes.len(), 2);
        assert_eq!(log.pushes[0], vec![0.5; APP_RPT_FRAME_SAMPLES]);
        assert_eq!(
            log.pushes[1],
            vec![s16_to_f32(1_000); APP_RPT_FRAME_SAMPLES]
        );
        assert_eq!(log.qualifications.len(), 2);
        assert_eq!(log.qualifications[0], ReceiveQualification::default());
        assert_eq!(log.qualifications[1], ReceiveQualification::default());
    }
    assert_eq!(
        state.write_program(&ControllerPcmFrame::advanced([0; ADVANCED_FRAME_SAMPLES])),
        Err(AdapterError::WrongInterface)
    );

    state.set_echo_enabled(false);
    assert!(!state.echo_enabled());
    state.set_echo_enabled(true);
    assert!(state.echo_enabled());
}

#[test]
fn program_ring_results_are_validated_and_advanced_has_no_echo() {
    for (behavior, expected) in [
        (
            ProducerBehavior::Fail,
            Err(AdapterError::ProgramRing(ProgramProducerError)),
        ),
        (
            ProducerBehavior::OverReport,
            Err(AdapterError::InvalidProgramRingResult),
        ),
    ] {
        let (program, _) = producer(behavior);
        let (_, mut state) = ControllerState::prepare_advanced(program, 2).unwrap();
        assert_eq!(
            state.write_program(&ControllerPcmFrame::advanced([0; ADVANCED_FRAME_SAMPLES])),
            expected
        );
        state.set_echo_enabled(true);
        assert!(!state.echo_enabled());
    }

    let (program, _) = producer(ProducerBehavior::Prefix(7));
    let (_, mut state) = ControllerState::prepare_advanced(program, 2).unwrap();
    assert_eq!(
        state
            .write_program(&ControllerPcmFrame::advanced([1; ADVANCED_FRAME_SAMPLES]))
            .unwrap()
            .accepted_samples,
        7
    );
}

#[test]
fn dtmf_state_preserves_legacy_detector_rules_only_for_app_rpt() {
    let (program, _) = producer(ProducerBehavior::All);
    let (_, mut advanced) = ControllerState::prepare_advanced(program, 2).unwrap();
    assert!(!advanced.dtmf_detection_enabled());
    advanced.set_dtmf_detection(true);
    assert!(!advanced.dtmf_detection_enabled());
    assert_eq!(
        advanced.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'1'
            },
            10
        ),
        DtmfAction::PassVoice
    );

    let (program, _) = producer(ProducerBehavior::All);
    let (_, mut legacy) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        2,
        EchoConfiguration::disabled(),
    )
    .unwrap();
    assert!(legacy.dtmf_detection_enabled());
    legacy.set_dtmf_detection(true);
    assert!(legacy.dtmf_detection_enabled());
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'u'
            },
            0
        ),
        DtmfAction::MutePseudoDigit
    );
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'm'
            },
            1
        ),
        DtmfAction::MutePseudoDigit
    );
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'5'
            },
            100
        ),
        DtmfAction::ForwardBegin(b'5')
    );
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'6'
            },
            120
        ),
        DtmfAction::SuppressRepeatedBegin
    );
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::End,
                digit: b'5'
            },
            90
        ),
        DtmfAction::ForwardEnd {
            digit: b'5',
            duration_ms: 0,
        }
    );
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::End,
                digit: b'7'
            },
            200
        ),
        DtmfAction::ForwardEnd {
            digit: b'7',
            duration_ms: 0,
        }
    );
    legacy.set_dtmf_detection(false);
    assert!(!legacy.dtmf_detection_enabled());
    assert_eq!(
        legacy.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::End,
                digit: b'u'
            },
            300
        ),
        DtmfAction::PassVoice
    );
}

#[test]
fn control_state_returns_typed_product_actions() {
    let (program, _) = producer(ProducerBehavior::All);
    let (_, mut state) = ControllerState::prepare_advanced(program, 2).unwrap();
    assert_eq!(state.control_snapshot(), ControlSnapshot::default());
    assert_eq!(
        state.apply_control(ControlMessage::TransmitKey(Some(tone(1_000)))),
        ControlAction::SetTransmit {
            keyed: true,
            forced_ctcss: Some(tone(1_000)),
        }
    );
    assert!(state.control_snapshot().transmit_keyed);
}

#[test]
fn adapter_errors_expose_stable_diagnostics() {
    for error in [
        AdapterError::InvalidNativeFrame,
        AdapterError::SampleRate(ConversionError),
        AdapterError::ConversionStalled,
        AdapterError::InvalidConversionResult,
        AdapterError::Pcm(PcmBoundaryError::InvalidFrameSize),
        AdapterError::WrongInterface,
        AdapterError::ProgramRing(ProgramProducerError),
        AdapterError::InvalidProgramRingResult,
    ] {
        assert!(!error.to_string().is_empty());
    }
    assert!(!ConversionError.to_string().is_empty());
    assert!(!ProgramProducerError.to_string().is_empty());
}

#[test]
fn echo_ignores_non_app_frames_and_full_recordings() {
    let mut echo = LegacyEcho::new(EchoConfiguration::new(true, 1).unwrap());
    echo.receive(
        &ControllerPcmFrame::advanced([1; ADVANCED_FRAME_SAMPLES]),
        true,
    );
    assert_eq!(echo.length, 0);
    echo.receive(
        &ControllerPcmFrame::app_rpt([1; APP_RPT_FRAME_SAMPLES]),
        true,
    );
    echo.receive(
        &ControllerPcmFrame::app_rpt([2; APP_RPT_FRAME_SAMPLES]),
        true,
    );
    assert_eq!(echo.length, APP_RPT_FRAME_SAMPLES);
    echo.receive(
        &ControllerPcmFrame::app_rpt([0; APP_RPT_FRAME_SAMPLES]),
        false,
    );
    assert!(echo.playing());
    echo.receive(
        &ControllerPcmFrame::app_rpt([3; APP_RPT_FRAME_SAMPLES]),
        true,
    );
    assert_eq!(echo.length, APP_RPT_FRAME_SAMPLES);

    let mut long_echo = LegacyEcho::new(EchoConfiguration::new(true, 2).unwrap());
    long_echo.receive(
        &ControllerPcmFrame::app_rpt([1; APP_RPT_FRAME_SAMPLES]),
        true,
    );
    long_echo.receive(
        &ControllerPcmFrame::app_rpt([2; APP_RPT_FRAME_SAMPLES]),
        true,
    );
    long_echo.receive(
        &ControllerPcmFrame::app_rpt([0; APP_RPT_FRAME_SAMPLES]),
        false,
    );
    long_echo.finish_playback_frame(APP_RPT_FRAME_SAMPLES);
    assert!(long_echo.playing());
    long_echo.finish_playback_frame(APP_RPT_FRAME_SAMPLES);
    assert!(!long_echo.playing());
}

#[test]
fn reload_state_preserves_echo_dtmf_timing_and_controller_state() {
    let (program, log) = producer(ProducerBehavior::All);
    let echo = EchoConfiguration::new(true, 2).unwrap();
    let (mut publisher, mut original) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        2,
        echo,
    )
    .unwrap();
    publisher
        .publish(
            &[0.5; ADVANCED_FRAME_SAMPLES],
            ReceiveMetadata {
                qualification: ReceiveQualification {
                    receiver_keyed: true,
                    ..ReceiveQualification::default()
                },
                ..ReceiveMetadata::default()
            },
        )
        .unwrap();
    assert_eq!(drain(&mut original), 2);
    publisher
        .publish(&[0.0; ADVANCED_FRAME_SAMPLES], ReceiveMetadata::default())
        .unwrap();
    assert_eq!(drain(&mut original), 2);
    assert!(original.echo_playing());
    assert_eq!(
        original.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::Begin,
                digit: b'5',
            },
            100,
        ),
        DtmfAction::ForwardBegin(b'5')
    );
    original.apply_control(ControlMessage::TransmitKey(Some(tone(1_000))));
    let retained = original.reload_state().unwrap();

    let (program, _) = producer(ProducerBehavior::All);
    let (_, mut replacement) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        2,
        EchoConfiguration::default(),
    )
    .unwrap();
    replacement.restore_reload_state(retained);

    assert!(replacement.echo_playing());
    assert_eq!(
        replacement
            .write_program(&ControllerPcmFrame::app_rpt([0; APP_RPT_FRAME_SAMPLES]))
            .unwrap()
            .source,
        ProgramSource::LegacyEcho
    );
    assert_eq!(log.lock().unwrap().pushes.len(), 0);
    assert_eq!(
        replacement.handle_dtmf(
            DtmfEvent {
                kind: DtmfEventKind::End,
                digit: b'5',
            },
            175,
        ),
        DtmfAction::ForwardEnd {
            digit: b'5',
            duration_ms: 75,
        }
    );
    assert_eq!(replacement.control_snapshot(), original.control_snapshot());
    assert_eq!(
        replacement.latest_receive_qualification(),
        original.latest_receive_qualification()
    );
}

#[test]
fn reload_state_requires_completed_receive_delivery() {
    let (program, _) = producer(ProducerBehavior::All);
    let (mut publisher, mut state) = ControllerState::prepare_advanced(program, 2).unwrap();
    publish_advanced(
        &mut publisher,
        0.5,
        ReceiveMetadata {
            qualification: ReceiveQualification {
                receiver_keyed: true,
                ..ReceiveQualification::default()
            },
            ..ReceiveMetadata::default()
        },
    );
    assert!(state.reload_state().is_none());
    assert!(matches!(
        next(&mut state).0,
        Some(DeliveryAction::ReceiverKey { .. })
    ));
    assert!(state.reload_state().is_none());
    assert_eq!(next(&mut state).0, Some(DeliveryAction::Voice));
    assert!(state.reload_state().is_some());
}

#[test]
fn reload_state_from_another_pcm_interface_is_ignored() {
    let (program, _) = producer(ProducerBehavior::All);
    let (_, source) = ControllerState::prepare_app_rpt(
        FakeConverter::boxed(ConverterBehavior::Downsample),
        program,
        2,
        EchoConfiguration::default(),
    )
    .unwrap();
    let retained = source.reload_state().unwrap();

    let (program, _) = producer(ProducerBehavior::All);
    let (_, mut target) = ControllerState::prepare_advanced(program, 2).unwrap();
    let before = target.control_snapshot();
    target.restore_reload_state(retained);
    assert_eq!(target.control_snapshot(), before);
}
