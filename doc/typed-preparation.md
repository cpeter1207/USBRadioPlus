# Typed station preparation ABI 1

The provisional alpha36 SDK adds a separate symbol to existing product SONAME 1. Consumers of typed preparation require alpha36 or newer with the complete capability. The product exports `usbradioplus_preparation_descriptor()`. Check the complete descriptor size, version 1, exact capability `usbradioplus.preparation1`, and every function pointer before admission. Retain the shared object while any candidate, station, token or peer is live. All calls run on the serialized control owner.

Pass `UrpTypedPreparation` with an ABI-4 station and an ABI-5 radio template. Source/node and raw graph lengths must be zero. Device identity, USB serial, selection, audio channels/buffering, CM119 profile, GPIO direction/initial/clip masks, PTT polarity, selected hardware gains, RX fallback gain, RX/TX emphasis and corners use the existing station fields. The radio template carries the fixed native stream, selected channel, gain, discriminator, VAD, signaling, qualification, delays and output routes. Station creation assigns the generation, frame bounds and publication cadence. Supply the standalone radio policy with external receive delay on, native delay frames zero, CPU savers off and advanced transport on. The prepared owner canonicalizes inactive VOX threshold and hang values.

The UTF-8 settings span is a version-one provider DTO of at most 1 MiB. The controller resolves resource identity, inheritance and named profiles before constructing this DTO. Strings remain exact UTF-8. Profile selection is represented by role presence. An omitted role uses the native fallback, an empty role selects the shipped chain for that role, and an explicit disabled role retains its gains and metering. The provider generates FFmpeg graph descriptions using its existing factories.

## Settings inventory

Top-level members are `station`, `local`, `link`, `voice_telemetry`, and `remote`. All are optional. Numeric fields are finite JSON numbers in the units indicated by the names. Integers retain their signedness and width on admission. Boolean fields take JSON booleans. Omit a role property to inherit its shipped value. Supplying null for a numeric or boolean role property or for a role group fails preparation. Enum strings use snake_case. A represented nested record has named members. Arrays preserve ordering. Unknown DTO members, malformed values and unsupported variants fail preparation. The diagnostic callback receives a static field token for a rejected value or its enclosing record. The caller owns the public warning and inherited fallback decision.

### Mapping the released radio settings

| Existing native or shared input | Prepared representation |
| --- | --- |
| Device selection, identifier, serial, capture/playback channels and extra buffering | Same selection, byte spans, channel counts and millisecond members in the station template. An exact selector can use GPIO USB topology alone |
| CM119 profile, PTT inversion, GPIO 1–8 mode and clipping LED | Station profile code, PTT polarity, output-enable/initial bit masks and clip mask |
| Capture and output A/B mixer gains | Station hardware-gain mask distinguishes omitted mixer controls from explicit 0 dB |
| EEPROM, voter reporting, USB topology, parallel device/address and individual pin modes | Station metadata below. Pins use physical connector order |
| Receive audio, emphasis and software gains | Disabled audio uses zero radio input gain. Active flat audio uses station de-emphasis. Speaker audio leaves de-emphasis off. Local role gains retain floating-point dB. The native fallback uses linear input gain and integral station output dB. A selected local role supersedes the fallback output gain in semantic comparisons |
| Carrier assignment, squelch threshold, hysteresis, noise filter and speech admission | Radio qualification carrier source and receive detector fields, including VAD mode and hang time |
| RX frequency, tail delay and TX frequency | Station metadata in Hz and milliseconds |
| RX-on and TX-off delays, settling, receive blanking and physical duplex | Radio qualification delays in 20 ms blocks, TX timing in milliseconds, and radio duplex flag |
| RX signaling, CTCSS qualification source, decoder level, relaxed decoding and override | Radio receive signaling and linked qualification fields. Linear decoder gain is converted once. Ordered or dormant tone metadata uses station.ctcss |
| TX signaling, CTCSS default/mappings/level/turnoff and DCS code/polarity/turnoff | Radio transmit signaling, with matching station.ctcss or station.dcs records to retain ordered and dormant definitions |
| TX output A/B assignment, pre-emphasis, and RX/TX corners | Radio output routes, station pre-emphasis and corner frequencies |
| Local repeat level, channel enablement and diagnostic publication | Station metadata. Native callback publication follows the lifecycle cadence |
| Local, incoming-link and voice/telemetry profile selectors | Each independently resolved role object. The controlling application's resource name never passes through section-name parsing |
| Remote-base, memory and scan sections | The resolved remote object contains transport options, operating policy and ordered definitions mapped below |

The standalone native frontend recognizes the shared hardware, receive, transmit, CTCSS, DCS, duplex, diagnostics, processing and remote sections. Asterisk's public channel-driver jitter-buffer settings belong to that driver. Existing raw graphs and retired VOX/CPU-saver keys have no controller resource fields.

### Station metadata

| Group | Members |
| --- | --- |
| station | `channel_enabled`, `eeprom_enabled`, `voter_reporting`, `gpio_usb_port_path`, `parallel_port`, `receive`, `transmit`, `ctcss`, `dcs`, `duplex`, `diagnostics` |
| parallel_port | `device`, `base_address`, `output_assignments`, `input_assignments` |
| receive | `frequency_hz`, `squelch_delay_ms` |
| transmit | `frequency_hz` |
| ctcss | `receive_frequencies`, `transmit_frequencies`, `receive_relax`, `transmit_default`, `transmit_peak_dbfs`, `phase_shift_degrees`, `tail_duration_ms`, `tail_frequency_hz` |
| dcs | `receive_code`, `transmit_code`, `turnoff_code_enabled`, `turnoff_duration_ms`, `peak_dbfs` |
| duplex | `local_repeat_level` |
| diagnostics | `status_publication_interval_ms` |

Parallel output assignments use eight entries for pins 2–9 and input assignments use four for pins 10, 12, 13 and 15. Null means unassigned. Output values are `low`, `high`, and `push_to_talk`. Inputs use `input`, `carrier`, and `ctcss`. CTCSS tones use supported integer tenths of hertz. DCS codes carry integer `value` (0–511) and `polarity` (`normal` or `inverted`). Signaling methods are `carrier`, `ctcss`, and `dcs`. Ordered and dormant CTCSS/DCS settings are retained for later rig retunes. When these groups are present, their active signaling must reproduce the ABI-5 signal. The carrier method, VAD, radio duplex, active decoder gain, override, turnoff mode, output routing and overlap with template fields have one authoritative representation in the radio/physical template. The common station builder enforces CTCSS source/route and mapping relationships. Public VOX and raw graph overrides are outside this controller contract. USB serial and named Hamlib transport options retain their existing device meanings. Generic serial-line or keyboard signal ownership needs a new physical capability.

### Source processing

| Group | Members |
| --- | --- |
| each role | `enabled`, `rnnoise_enabled`, `input_gain_db`, `output_gain_db`, `stage_order`, `receive`, `equalizer`, `deesser`, `agc`, `expander`, `compressor`, `limiter`, `peak_limiter`, `transmit_tail` |
| receive | `bandpass_enabled`, `bandpass_highpass_hz`, `bandpass_lowpass_hz`, `pl_filter`, `notch_width_hz`, `highpass_hz` |
| equalizer | `enabled`, `low_gain_db`, `low_frequency_hz`, `low_slope`, `mid_gain_db`, `mid_frequency_hz`, `mid_width_octaves`, `high_gain_db`, `high_frequency_hz`, `high_slope` |
| deesser | `enabled`, `frequency_hz`, `width_octaves`, `threshold_dbfs`, `ratio`, `max_reduction_db`, `attack_ms`, `release_ms` |
| agc | `enabled`, `target_lufs`, `max_gain_db`, `max_attenuation_db`, `loudness_window_ms`, `gain_increase_db_per_second`, `gain_decrease_db_per_second`, `activity_threshold_dbfs`, `activity_hysteresis_db`, `hold_ms`, `deadband_db` |
| expander | `enabled`, `threshold_dbfs`, `ratio`, `max_attenuation_db`, `attack_ms`, `release_ms`, `sidechain_highpass_hz`, `sidechain_lowpass_hz` |
| compressor | `enabled`, `layout`, `low_crossover_hz`, `high_crossover_hz`, `full`, `low`, `mid`, `high`, `sidechain_highpass_hz`, `sidechain_lowpass_hz` |
| limiter | `enabled`, `layout`, `low_crossover_hz`, `high_crossover_hz`, `full`, `low`, `mid`, `high` |
| full, low, mid, high | `threshold_dbfs`, `ratio`, `makeup_gain_db`, `knee_db`, `attack_ms`, `release_ms` |
| peak_limiter | `enabled`, `ceiling_dbfs`, `oversample`, `lookahead_ms`, `attack_ms`, `release_ms` |
| transmit_tail | `bandpass_enabled`, `bandpass_highpass_hz`, `bandpass_lowpass_hz` |

A `stage_order` is an ordered array of distinct `equalizer`, `expander`, `agc`, `deesser`, `compressor`, and `limiter` entries. Every enabled optional stage must appear. Dynamics layouts use `full_band` and `three_band`. A supplied dynamics band has all six fields. Numeric limits, role restrictions, mandatory tail stages and graph generation belong to `ProcessingChain::validate` and the existing FFmpeg factory.

### Remote composition

| Group | Members |
| --- | --- |
| remote | `backend`, `audio_source`, `hamlib_model`, `hamlib_options`, `timeout_ms`, `retries`, `timeout_retries`, `poll_interval_ms`, `ptt_source`, `cor_source`, `external_encode_output`, `external_encode_inverted`, `receive_ranges`, `transmit_ranges`, `receive_only_ranges`, `modes`, `maximum_power_mw`, `memories`, `scan_lists`, `startup_memory`, `dtmf_access`, `dtmf_prefix`, `offset_magnitude_hz`, `power_presets_mw`, `scan_presets_ms`, `tune_duration_ms` |
| memories[] | `selector`, `name`, `state` |
| state | `receive_hz`, `transmit_hz`, `mode`, `bandwidth_hz`, `receive_vfo`, `transmit_vfo`, `split`, `offset_hz`, `rit_hz`, `rit_enabled`, `xit_hz`, `xit_enabled`, `power_mw`, `encode`, `decode`, `channel` |
| encode, decode | `source`, `control_source` (decode only), `enabled`, `tone_tenths_hz`, `dcs_code`, `inverted` |
| scan_lists[] | `selector`, `name`, `members`, `dwell_ms`, `resume_delay_ms`, `settle_ms`, `priority` |

The enabled remote object contains fully resolved policy. Provide every non-optional field. Optional values may be null. An omitted remote means ordinary station operation. Backends are `hamlib`, `parallel_channel`, and `parallel_rtx`, with `cm119` or `alsa` audio. Hamlib options are an exact string-to-string map including `rig_pathname` when selected. DTMF access is `disabled`, `open`, or `locked`. The external encoder output is null or a single tagged object such as `{"usb":7}` or `{"parallel":3}`. Each range is an inclusive two-element integer Hz array. Mode and VFO selectors follow the released remote contract. Presets each have three nullable entries. Memory and scan selectors are positive. Scan members and priority reference memory selectors. Encode and decode independently specify one source (`disabled`, `dsp`, `external`, `rig`, or `automatic`). A decoded indication can come from external GPIO while the rig programs the decoder frequency. In the provider DTO, decode.source selects qualification and optional decode.control_source selects programming. Omission or null follows the qualification source. For example, `{"source":"external","control_source":"rig","enabled":true,"tone_tenths_hz":1148,"dcs_code":0,"inverted":false}` retains an external indication while programming 114.8 Hz on the rig. Controller bindings derive these internal values from their operating configuration. The public USBRadioPlus INI spelling is `decode_control_source=hamlib`. RF policy admission uses the released remote operating engine with synthetic capabilities. The existing hardware-free admission also checks selected external output wiring and Hamlib model availability. Actual capabilities and rig opening belong to the activation worker.

### Owned lifecycle

`prepare` copies all caller spans. `equal` compares effective typed values and role presence. Use the product descriptor's configuration visitor, remote options and destroy operations with the returned owner. `create` and `prepare_update` take the original lifecycle arguments with a null config pointer. Create retains its ordinary callbacks and descriptors. Each update token uses the existing begin, accept, finish and destroy operations. The prepared owner can be destroyed after the synchronous call. `link_prepare` takes the negotiated sampling rate and maximum frames, prepares a separate metered handle and transfers it to the ordinary product link destructor; retain the graph descriptor until then. An absent link role yields the established neutral metered processor.

## Independent link role for ports without a physical station

The provisional alpha36 product SONAME 1 also exports `usbradioplus_link_preparation_descriptor()`. Resolve this getter from the matched product DSO. Require the complete `UrpLinkPreparationDescriptor` size, ABI version 1, exact capability `usbradioplus.link-preparation1`, and all four operations before use. Retain the product DSO until all roles and peers are destroyed.

Call `prepare` on the serialized control owner with a positive UTF-8 DTO span of at most 1 MiB. It accepts only the optional `link` member from the settings inventory above. Use `{}` for an absent, neutral metered role, `{"link":{}}` for the shipped peer profile, or an explicit link role to override its settings. A disabled role retains its configured gains and meter. Preparation copies its inputs and needs no station template, hardware selector, provider descriptor or open device. It reports a rejected field through the optional synchronous diagnostic callback.

`equal` compares presence and effective profile settings between two independent role owners. `link_prepare` accepts the negotiated sample rate, maximum frames and retained FFmpeg descriptor. Each successful call creates an independent metered product1 peer. Destroy the role through this descriptor's `destroy` after its last call. Process and destroy peers through product1 and retain the FFmpeg provider until the last peer using it has been destroyed. The independent role handle is specific to this capability.
