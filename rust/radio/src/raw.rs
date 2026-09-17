use super::*;

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawReceiveConfig {
    pub(super) noise_filter_profile: u32,
    pub(super) squelch_open_level: u32,
    pub(super) squelch_hysteresis: u32,
    pub(super) ctcss_decoder_gain: f32,
    pub(super) vox_threshold: i32,
    pub(super) vox_hang_ms: i32,
    pub(super) ctcss_enabled: u32,
    pub(super) ctcss_tone_mask: u64,
    pub(super) ctcss_relax: u32,
    pub(super) dcs_enabled: u32,
    pub(super) dcs_code: i32,
    pub(super) dcs_inverted: u32,
    pub(super) cpu_saver_enabled: u32,
    pub(super) native_squelch_delay_frames: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawQualificationConfig {
    pub(super) carrier_source: u32,
    pub(super) subaudible_source: u32,
    pub(super) subaudible_override: u32,
    pub(super) advanced_transport: u32,
    pub(super) radio_duplex: u32,
    pub(super) rx_on_delay_blocks: u32,
    pub(super) tx_off_delay_blocks: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawTransmitConfig {
    pub(super) ctcss_transmit_enabled: u32,
    pub(super) default_ctcss_frequency_tenths_hz: i32,
    pub(super) mapped_ctcss_frequency_tenths_hz: [i32; CTCSS_TONE_COUNT],
    pub(super) tone_off_mode: u32,
    pub(super) ctcss_turnoff_duration_ms: i32,
    pub(super) ctcss_turnoff_phase_shift_degrees: f64,
    pub(super) ctcss_turnoff_tail_tone_hz: f64,
    pub(super) dcs_transmit_enabled: u32,
    pub(super) dcs_turnoff_enabled: u32,
    pub(super) dcs_turnoff_duration_ms: i32,
    pub(super) receiver_blanking_ms: i32,
    pub(super) tx_settle_time_ms: i32,
    pub(super) cpu_saver_enabled: u32,
    pub(super) dcs_code: i32,
    pub(super) dcs_inverted: u32,
    pub(super) dcs_peak: f32,
    pub(super) ctcss_peak: f32,
    pub(super) output_a_route: u32,
    pub(super) output_b_route: u32,
    pub(super) output_a_tone_gain: f32,
    pub(super) output_a_tone_bias: f32,
    pub(super) output_b_tone_gain: f32,
    pub(super) output_b_tone_bias: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawSessionConfig {
    pub(super) struct_size: u32,
    pub(super) abi_version: u32,
    pub(super) generation_id: u64,
    pub(super) native_sample_rate_hz: u32,
    pub(super) interleaved_channels: u32,
    pub(super) maximum_receive_frame_count: u32,
    pub(super) maximum_transmit_frame_count: u32,
    pub(super) publication_interval_ms: u32,
    pub(super) receive_channel: u32,
    pub(super) receive_input_gain: f32,
    pub(super) receive: RawReceiveConfig,
    pub(super) qualification: RawQualificationConfig,
    pub(super) transmit: RawTransmitConfig,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawSessionPorts {
    pub(super) struct_size: u32,
    pub(super) receive_deemphasis: RawProcessorPort,
    pub(super) receive_filter: RawProcessorPort,
    pub(super) receive_ctcss_notch: [RawProcessorPort; CTCSS_TONE_COUNT],
    pub(super) receive_noise_reduction: RawProcessorPort,
    pub(super) receive_dynamics: RawProcessorPort,
    pub(super) transmit_program: RawProcessorPort,
    pub(super) transmit_dcs_normal_filter: RawProcessorPort,
    pub(super) transmit_dcs_turnoff_filter: RawProcessorPort,
    pub(super) program_ring: RawProgramRingPort,
    pub(super) receive_ctcss_tail_notch: RawProcessorPort,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(super) struct RawReceiveInput {
    pub(super) hardware_carrier: u32,
    pub(super) parallel_carrier: u32,
    pub(super) hardware_subaudible: u32,
    pub(super) parallel_subaudible: u32,
    pub(super) subaudible_override: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(super) struct RawTransmitInput {
    pub(super) external_ptt_request: u32,
    pub(super) physical_ptt_applied: u32,
    pub(super) render_admitted: u32,
    pub(super) ctcss_inhibit: u32,
    pub(super) calibrated_test_tone: u32,
    pub(super) forced_ctcss_tenths_hz: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct RawReceiveResult {
    pub(super) generation_id: u64,
    pub(super) first_sample_index: u64,
    pub(super) frame_count: u32,
    pub(super) carrier_active: u32,
    pub(super) subaudible_active: u32,
    pub(super) receiver_keyed: u32,
    pub(super) ctcss_decoded_index: i32,
    pub(super) dcs_valid: u32,
    pub(super) rssi_peak: i16,
    pub(super) rssi_updated: u32,
    pub(super) ctcss_decoder_peak: f32,
    pub(super) input_peak: f32,
    pub(super) input_rms: f32,
    pub(super) input_rail_samples: u64,
    pub(super) output_peak: f32,
    pub(super) output_rms: f32,
    pub(super) output_rail_samples: u64,
    pub(super) periodic_status_due: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct RawTransmitResult {
    pub(super) generation_id: u64,
    pub(super) first_sample_index: u64,
    pub(super) frame_count: u32,
    pub(super) logical_ptt: u32,
    pub(super) transmitter_state: i32,
    pub(super) selected_ctcss_tenths_hz: i32,
    pub(super) program_peak: f32,
    pub(super) program_rms: f32,
    pub(super) program_rail_samples: u64,
    pub(super) output_peak: f32,
    pub(super) output_rms: f32,
    pub(super) output_rail_samples: u64,
    pub(super) periodic_status_due: u32,
    pub(super) program_ring: RingObservation,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) struct RawEvent {
    pub(super) generation_id: u64,
    pub(super) sample_index: u64,
    pub(super) kind: u32,
    pub(super) value: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(super) struct RawSnapshot {
    pub(super) generation_id: u64,
    pub(super) receive_frames: u64,
    pub(super) transmit_frames: u64,
    pub(super) receive_input_peak: f32,
    pub(super) receive_input_rms: f32,
    pub(super) receive_ctcss_decoder_peak: f32,
    pub(super) receive_output_peak: f32,
    pub(super) receive_output_rms: f32,
    pub(super) transmit_program_peak: f32,
    pub(super) transmit_program_rms: f32,
    pub(super) transmit_output_peak: f32,
    pub(super) transmit_output_rms: f32,
    pub(super) receive_input_rail_samples: u64,
    pub(super) receive_output_rail_samples: u64,
    pub(super) transmit_program_rail_samples: u64,
    pub(super) transmit_output_rail_samples: u64,
    pub(super) provider_failures: u64,
    pub(super) receive_event_drops: u64,
    pub(super) transmit_event_drops: u64,
    pub(super) carrier_active: u32,
    pub(super) subaudible_active: u32,
    pub(super) receiver_keyed: u32,
    pub(super) logical_ptt: u32,
    pub(super) ctcss_decoded_index: i32,
    pub(super) dcs_valid: u32,
    pub(super) program_ring: RingObservation,
}

pub(super) type SessionCreate = unsafe extern "C" fn(
    *const RawSessionConfig,
    *const RawSessionPorts,
    *mut *mut OpaqueSession,
) -> c_int;
pub(super) type SessionWarm = unsafe extern "C" fn(*mut OpaqueSession) -> c_int;
pub(super) type SessionReceive = unsafe extern "C" fn(
    *mut OpaqueSession,
    *const f32,
    *mut f32,
    u32,
    *const RawReceiveInput,
    *mut RawReceiveResult,
) -> c_int;
pub(super) type SessionTransmit = unsafe extern "C" fn(
    *mut OpaqueSession,
    *mut f32,
    u32,
    *const RawTransmitInput,
    *mut RawTransmitResult,
) -> c_int;
pub(super) type SessionSnapshotFn =
    unsafe extern "C" fn(*const OpaqueSession, *mut RawSnapshot) -> c_int;
pub(super) type SessionPopEvent = unsafe extern "C" fn(*const OpaqueSession, *mut RawEvent) -> u32;
pub(super) type SessionDestroy = unsafe extern "C" fn(*mut OpaqueSession);

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawDescriptorHeader {
    pub(super) struct_size: u32,
    pub(super) abi_version: u32,
    pub(super) capability_name: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub(super) struct RawDescriptor {
    pub(super) struct_size: u32,
    pub(super) abi_version: u32,
    pub(super) capability_name: *const c_char,
    pub(super) session_create: Option<SessionCreate>,
    pub(super) session_warm: Option<SessionWarm>,
    pub(super) session_receive: Option<SessionReceive>,
    pub(super) session_transmit: Option<SessionTransmit>,
    pub(super) session_snapshot: Option<SessionSnapshotFn>,
    pub(super) session_pop_receive_event: Option<SessionPopEvent>,
    pub(super) session_pop_transmit_event: Option<SessionPopEvent>,
    pub(super) session_destroy: Option<SessionDestroy>,
}

pub(super) const REQUIRED_DESCRIPTOR_SIZE: usize =
    offset_of!(RawDescriptor, session_destroy) + size_of::<Option<SessionDestroy>>();

#[derive(Clone, Copy)]
pub(super) struct Functions {
    pub(super) create: SessionCreate,
    pub(super) warm: SessionWarm,
    pub(super) receive: SessionReceive,
    pub(super) transmit: SessionTransmit,
    pub(super) snapshot: SessionSnapshotFn,
    pub(super) pop_receive_event: SessionPopEvent,
    pub(super) pop_transmit_event: SessionPopEvent,
    pub(super) destroy: SessionDestroy,
}

impl SessionConfig {
    pub(super) fn as_raw(&self) -> RawSessionConfig {
        let (ctcss, receive_dcs) = match self.receive.signaling {
            ReceiveSignaling::Disabled => (None, None),
            ReceiveSignaling::Ctcss(config) => (Some(config), None),
            ReceiveSignaling::Dcs(config) => (None, Some(config)),
        };
        let (transmit_ctcss, transmit_dcs) = match self.transmit.signaling {
            TransmitSignaling::Disabled => (None, None),
            TransmitSignaling::Ctcss(config) => (Some(config), None),
            TransmitSignaling::Dcs(config) => (None, Some(config)),
        };
        let ctcss_config = ctcss.unwrap_or(CtcssReceiveConfig {
            tones: CtcssToneMask::EMPTY,
            relaxed: false,
        });
        let receive_dcs_config = receive_dcs.unwrap_or(DcsReceiveConfig {
            code: 0,
            inverted: false,
        });
        let transmit_ctcss_config = transmit_ctcss.unwrap_or_default();
        let transmit_dcs_config = transmit_dcs.unwrap_or_default();
        RawSessionConfig {
            struct_size: size_of::<RawSessionConfig>() as u32,
            abi_version: ABI_VERSION,
            generation_id: self.generation_id,
            native_sample_rate_hz: NATIVE_SAMPLE_RATE_HZ,
            interleaved_channels: CANONICAL_CHANNELS as u32,
            maximum_receive_frame_count: self.maximum_receive_frame_count,
            maximum_transmit_frame_count: self.maximum_transmit_frame_count,
            publication_interval_ms: self.publication_interval_milliseconds,
            receive_channel: self.receive_channel as u32,
            receive_input_gain: self.receive_input_gain,
            receive: RawReceiveConfig {
                noise_filter_profile: self.receive.noise_filter_profile as u32,
                squelch_open_level: self.receive.squelch_open_level,
                squelch_hysteresis: self.receive.squelch_hysteresis,
                ctcss_decoder_gain: self.receive.ctcss_decoder_gain,
                vox_threshold: self.receive.vox_threshold,
                vox_hang_ms: self.receive.vox_hang_milliseconds,
                ctcss_enabled: u32::from(ctcss.is_some()),
                ctcss_tone_mask: ctcss_config.tones.bits(),
                ctcss_relax: u32::from(ctcss_config.relaxed),
                dcs_enabled: u32::from(receive_dcs.is_some()),
                dcs_code: receive_dcs_config.code,
                dcs_inverted: u32::from(receive_dcs_config.inverted),
                cpu_saver_enabled: u32::from(self.receive.cpu_saver_enabled),
                native_squelch_delay_frames: self.receive.native_squelch_delay_frames,
            },
            qualification: RawQualificationConfig {
                carrier_source: self.qualification.carrier_source as u32,
                subaudible_source: self.qualification.subaudible_source as u32,
                subaudible_override: u32::from(self.qualification.subaudible_override),
                advanced_transport: u32::from(self.qualification.advanced_transport),
                radio_duplex: u32::from(self.qualification.radio_duplex),
                rx_on_delay_blocks: self.qualification.receive_on_delay_blocks,
                tx_off_delay_blocks: self.qualification.transmit_off_delay_blocks,
            },
            transmit: RawTransmitConfig {
                ctcss_transmit_enabled: u32::from(transmit_ctcss.is_some()),
                default_ctcss_frequency_tenths_hz: transmit_ctcss_config
                    .default_frequency_tenths_hz,
                mapped_ctcss_frequency_tenths_hz: transmit_ctcss_config
                    .mapped_frequencies_tenths_hz,
                tone_off_mode: self.transmit.tone_off_mode as u32,
                ctcss_turnoff_duration_ms: transmit_ctcss_config.turnoff_duration_milliseconds,
                ctcss_turnoff_phase_shift_degrees: transmit_ctcss_config
                    .turnoff_phase_shift_degrees,
                ctcss_turnoff_tail_tone_hz: transmit_ctcss_config.turnoff_tail_tone_hz,
                dcs_transmit_enabled: u32::from(transmit_dcs.is_some()),
                dcs_turnoff_enabled: u32::from(transmit_dcs_config.turnoff_enabled),
                dcs_turnoff_duration_ms: transmit_dcs_config.turnoff_duration_milliseconds,
                receiver_blanking_ms: self.transmit.receive_blanking_milliseconds,
                tx_settle_time_ms: self.transmit.settle_time_milliseconds,
                cpu_saver_enabled: u32::from(self.transmit.cpu_saver_enabled),
                dcs_code: transmit_dcs_config.code,
                dcs_inverted: u32::from(transmit_dcs_config.inverted),
                dcs_peak: transmit_dcs_config.peak,
                ctcss_peak: transmit_ctcss_config.peak,
                output_a_route: self.transmit.output_a.route as u32,
                output_b_route: self.transmit.output_b.route as u32,
                output_a_tone_gain: self.transmit.output_a.tone_gain,
                output_a_tone_bias: self.transmit.output_a.tone_bias,
                output_b_tone_gain: self.transmit.output_b.tone_gain,
                output_b_tone_bias: self.transmit.output_b.tone_bias,
            },
        }
    }
}

impl ReceiveControls {
    pub(super) fn as_raw(self) -> RawReceiveInput {
        RawReceiveInput {
            hardware_carrier: u32::from(self.hardware_carrier),
            parallel_carrier: u32::from(self.parallel_carrier),
            hardware_subaudible: u32::from(self.hardware_subaudible),
            parallel_subaudible: u32::from(self.parallel_subaudible),
            subaudible_override: u32::from(self.subaudible_override),
        }
    }
}

impl TransmitControls {
    pub(super) fn as_raw(self) -> RawTransmitInput {
        RawTransmitInput {
            external_ptt_request: u32::from(self.external_ptt_request),
            physical_ptt_applied: u32::from(self.physical_ptt_applied),
            render_admitted: u32::from(self.render_admitted),
            ctcss_inhibit: u32::from(self.ctcss_inhibit),
            calibrated_test_tone: u32::from(self.calibrated_test_tone),
            forced_ctcss_tenths_hz: self.forced_ctcss_tenths_hz,
        }
    }
}

impl SessionPorts<'_> {
    pub(super) fn as_raw(&self) -> RawSessionPorts {
        RawSessionPorts {
            struct_size: size_of::<RawSessionPorts>() as u32,
            receive_deemphasis: self.receive_deemphasis.raw,
            receive_filter: self.receive_filter.raw,
            receive_ctcss_notch: std::array::from_fn(|index| self.receive_ctcss_notch[index].raw),
            receive_noise_reduction: self.receive_noise_reduction.raw,
            receive_dynamics: self.receive_dynamics.raw,
            transmit_program: self.transmit_program.raw,
            transmit_dcs_normal_filter: self.transmit_dcs_normal_filter.raw,
            transmit_dcs_turnoff_filter: self.transmit_dcs_turnoff_filter.raw,
            program_ring: self.program_ring.raw,
            receive_ctcss_tail_notch: self.receive_ctcss_tail_notch.raw,
        }
    }
}
