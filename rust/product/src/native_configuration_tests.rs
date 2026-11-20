use super::*;
use crate::{ABI_VERSION, Status};
use std::ptr;
use usbradioplus_core::CtcssTone;
use usbradioplus_radio::{SessionConfig, TransmitSignaling};

#[derive(Default)]
struct Observation {
    radio: Option<SessionConfig>,
    source: String,
    node: String,
    gains: u32,
    channels: [u32; 2],
    device_selection: u32,
    cm119_profile: u32,
    output_gain_db: i32,
    preemphasis: u32,
    reject: bool,
}

unsafe extern "C" fn observe(
    context: *mut c_void,
    request: *const UrpNativeStationConfig,
) -> c_int {
    // SAFETY: each test supplies a live observation for this synchronous visitor.
    let output = unsafe { &mut *context.cast::<Observation>() };
    // SAFETY: the resolver's visitor contract supplies a complete borrowed request.
    let request = unsafe { &*request };
    // SAFETY: the radio template remains readable throughout the callback.
    output.radio = unsafe { SessionConfig::from_abi(request.radio) }.ok();
    // SAFETY: request spans remain readable throughout the callback.
    output.source = unsafe {
        native::text(
            request.processing_configuration,
            request.processing_configuration_length,
        )
    }
    .unwrap_or_default();
    // SAFETY: request spans remain readable throughout the callback.
    output.node = unsafe { native::text(request.processing_node, request.processing_node_length) }
        .unwrap_or_default();
    output.gains = request.hardware_gain_mask;
    output.channels = [
        request.input_device_channels,
        request.output_device_channels,
    ];
    output.device_selection = request.device_selection;
    output.cm119_profile = request.cm119_profile;
    output.output_gain_db = request.receive_output_gain_db;
    output.preemphasis = request.transmit_preemphasis;
    if output.reject { -1 } else { 0 }
}

fn args(text: &str, node: &str) -> UrpNativeProcessingArgs {
    UrpNativeProcessingArgs {
        struct_size: size_of::<UrpNativeProcessingArgs>() as u32,
        abi_version: ABI_VERSION,
        configuration: text.as_ptr(),
        configuration_length: text.len() as u32,
        node: node.as_ptr(),
        node_length: node.len() as u32,
    }
}

#[test]
fn remote_resolution_rejects_out_of_policy_memory_before_any_handoff() {
    for (frequency, expected) in [
        (146_520_000, 0),
        (140_000_000, Status::InvalidConfiguration.code()),
    ] {
        let source = format!(
            "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=one\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 one]\nselector=1\nrx_frequency_hz={frequency}\ntx_frequency_hz=146520000\nmode=FM\n"
        );
        let request = args(&source, "524950");
        let mut owner = ptr::null_mut();
        // SAFETY: this test retains the complete request, text and writable output.
        let result = unsafe { resolve(&request, None, ptr::null_mut(), &mut owner) };
        let absent = owner.is_null();
        // SAFETY: resolution transfers unique ownership; null destruction is permitted.
        unsafe { destroy(owner) };
        assert_eq!(result, expected);
        assert_eq!(absent, expected != 0);
    }
}

#[test]
fn owned_resolution_visits_common_radio_policy_and_preserves_original_profile_presence() {
    let source = "[1000]\nvoice_telemetry_profile=limited\n[radio]\nreceive_signaling=ctcss\nreceive_ctcss_tones_hz=100.0,123.0\ntransmit_signaling=ctcss\ntransmit_ctcss_tones_hz=103.5,136.5\ntransmit_ctcss_default_hz=103.5\nreceive_cpu_saver_enabled=yes\ntransmit_cpu_saver_enabled=yes\nreceive_input_gain_db=-2\nreceive_graph=highpass=f=60\n[radio 1000]\nhardware_input_gain_db=1.5\noutput_device_channels=2\n[voice_telemetry limited]\noutput_gain_db=-3\n";
    let request = args(source, "1000");
    let mut owner = ptr::null_mut();
    assert_eq!(
        // SAFETY: request and output storage remain live for resolution.
        unsafe { resolve(&request, None, ptr::null_mut(), &mut owner) },
        0
    );
    let mut output = Observation::default();
    assert_eq!(
        // SAFETY: the unique resolved owner is retained until after this visitor.
        unsafe { visit(owner, Some(observe), ptr::from_mut(&mut output).cast()) },
        0
    );
    let radio = output.radio.as_ref().unwrap();
    assert!(!radio.receive.cpu_saver_enabled);
    assert!(!radio.transmit.cpu_saver_enabled);
    assert_eq!(radio.receive.native_squelch_delay_frames, 0);
    assert_eq!(radio.receive_input_gain, 10.0_f32.powf(-2.0 / 20.0));
    let TransmitSignaling::Ctcss(ctcss) = radio.transmit.signaling else {
        panic!("CTCSS expected")
    };
    for (receive, transmit) in [(1000, 1035), (1230, 1365)] {
        assert_eq!(
            ctcss.mapped_frequencies_tenths_hz
                [CtcssTone::from_tenths_hz(receive).unwrap().table_index()],
            transmit
        );
    }
    assert_eq!(output.source, source);
    assert_eq!(output.node, "1000");
    assert_eq!(output.gains, crate::URP_NATIVE_HARDWARE_GAIN_INPUT);
    assert_eq!(output.channels, [1, 2]);
    assert_eq!(output.preemphasis, 0);
    output.reject = true;
    assert_eq!(
        // SAFETY: callback storage and the owned configuration remain live.
        unsafe { visit(owner, Some(observe), ptr::from_mut(&mut output).cast()) },
        Status::SetupFailed.code()
    );
    // SAFETY: ownership was transferred exactly once by resolve.
    unsafe { destroy(owner) };
}

#[test]
fn warnings_preserve_inherited_values_and_explicit_device_selection_across_the_abi() {
    unsafe extern "C" fn diagnostic(context: *mut c_void, text: *const u8, length: u32) {
        // SAFETY: resolve retains this diagnostic collection for the synchronous callback.
        let messages = unsafe { &mut *context.cast::<Vec<String>>() };
        // SAFETY: the shared resolver borrows this complete UTF-8 span until return.
        messages.push(unsafe { native::text(text, length) }.unwrap());
    }
    let source = "[1000]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\ncm119_profile=custom\ninput_device_channels=2\n[radio 1000]\ninput_device_channels=3\n";
    let request = args(source, "1000");
    let mut owner = ptr::null_mut();
    let mut messages = Vec::<String>::new();
    assert_eq!(
        // SAFETY: all request spans, output storage and diagnostic context remain live.
        unsafe {
            resolve(
                &request,
                Some(diagnostic),
                ptr::from_mut(&mut messages).cast(),
                &mut owner,
            )
        },
        0
    );
    let mut output = Observation::default();
    assert_eq!(
        // SAFETY: this unique resolved owner retains the complete borrowed native request.
        unsafe { visit(owner, Some(observe), ptr::from_mut(&mut output).cast()) },
        0
    );
    // SAFETY: resolution transferred this owner exactly once; all callbacks have returned.
    unsafe { destroy(owner) };
    assert_eq!(output.device_selection, 1);
    assert_eq!(output.cm119_profile, 3);
    assert_eq!(output.channels, [2, 1]);
    assert_eq!(output.source, source);
    assert_eq!(output.node, "1000");
    assert_eq!(messages.len(), 1);
    assert!(messages[0].contains("rpt_advanced.conf [radio 1000]: input_device_channels=\"3\""));
    assert!(messages[0].ends_with("using 2"));
}

#[test]
fn raw_output_gain_fraction_warns_and_visits_the_inherited_integer_without_truncation() {
    unsafe extern "C" fn diagnostic(context: *mut c_void, text: *const u8, length: u32) {
        // SAFETY: this synchronous call retains the owned collection and borrowed diagnostic span.
        let messages = unsafe { &mut *context.cast::<Vec<String>>() };
        // SAFETY: the SDK supplies the complete borrowed UTF-8 diagnostic span.
        messages.push(unsafe { native::text(text, length) }.unwrap());
    }
    for (source, inherited) in [
        ("[1000]\n[radio]\nreceive_output_gain_db=-6.2\n", 0),
        (
            "[1000]\n[radio]\nreceive_output_gain_db=-3\n[radio 1000]\nreceive_output_gain_db=-6.2\n",
            -3,
        ),
    ] {
        let request = args(source, "1000");
        let mut owner = ptr::null_mut();
        let mut messages = Vec::<String>::new();
        // SAFETY: all request, owner and callback storage remains live through these synchronous calls.
        unsafe {
            assert_eq!(
                resolve(
                    &request,
                    Some(diagnostic),
                    ptr::from_mut(&mut messages).cast(),
                    &mut owner
                ),
                0
            );
            let mut output = Observation::default();
            assert_eq!(
                visit(owner, Some(observe), ptr::from_mut(&mut output).cast()),
                0
            );
            destroy(owner);
            assert_eq!(output.output_gain_db, inherited);
            assert_eq!(output.source, source);
        }
        assert_eq!(messages.len(), 1);
        assert!(messages[0].contains("receive_output_gain_db=\"-6.2\""));
        assert!(messages[0].ends_with(&format!("using {inherited}")));
    }
}

#[test]
fn native_configuration_visits_all_shared_hardware_profile_codes() {
    for (profile, code) in [("dudeusb", 0), ("sphusb", 1), ("nhrc", 2), ("custom", 3)] {
        let source = format!("[1000]\n[radio]\ncm119_profile={profile}\n");
        let request = args(&source, "1000");
        let mut owner = ptr::null_mut();
        let mut output = Observation::default();
        // SAFETY: source/context/owner are retained through each borrowed synchronous visit.
        unsafe {
            assert_eq!(resolve(&request, None, ptr::null_mut(), &mut owner), 0);
            assert_eq!(
                visit(owner, Some(observe), ptr::from_mut(&mut output).cast()),
                0
            );
            destroy(owner);
        }
        assert_eq!(output.cm119_profile, code, "{profile}");
    }
}

#[test]
fn resolution_rejects_bad_abi_utf8_missing_node_and_invalid_visitors() {
    let node = "1000";
    let text = "[1000]\n";
    let mut request = args(text, node);
    for bad in [0, 2] {
        request.abi_version = bad;
        let mut output = ptr::dangling_mut::<c_void>();
        assert_ne!(
            // SAFETY: the complete request deliberately advertises an unsupported version.
            unsafe { resolve(&request, None, ptr::null_mut(), &mut output) },
            0
        );
        assert!(output.is_null());
    }
    request.abi_version = ABI_VERSION;
    let invalid = [0xff];
    request.configuration = invalid.as_ptr();
    request.configuration_length = 1;
    let mut owner = ptr::null_mut();
    assert_eq!(
        // SAFETY: the advertised invalid UTF-8 span is readable.
        unsafe { resolve(&request, None, ptr::null_mut(), &mut owner) },
        Status::InvalidArgument.code()
    );
    request = args("[other]\n", node);
    assert_ne!(
        // SAFETY: request/output storage is complete and live.
        unsafe { resolve(&request, None, ptr::null_mut(), &mut owner) },
        0
    );
    request = args(text, node);
    assert_ne!(
        // SAFETY: null output is intentionally rejected before any configuration is retained.
        unsafe { resolve(&request, None, ptr::null_mut(), ptr::null_mut()) },
        0
    );
    // SAFETY: a null owner/visitor are intentionally rejected.
    assert_ne!(unsafe { visit(ptr::null(), None, ptr::null_mut()) }, 0);
    // SAFETY: null destruction is explicitly supported.
    unsafe { destroy(ptr::null_mut()) };
}

#[test]
fn typed_preparation_matches_ini_station_roles_and_owned_remote_policy() {
    struct Comparison<'a> {
        dto: &'a [u8],
        owner: *mut c_void,
    }
    unsafe extern "C" fn prepare(
        context: *mut c_void,
        station: *const UrpNativeStationConfig,
    ) -> c_int {
        // SAFETY: visit lends a full native request and this synchronous test context.
        let (context, mut station) = unsafe { (&mut *context.cast::<Comparison>(), *station) };
        station.processing_configuration_length = 0;
        station.processing_node_length = 0;
        station.receive_graph_length = 0;
        station.transmit_graph_length = 0;
        let args = crate::preparation::UrpTypedPreparation {
            struct_size: size_of::<crate::preparation::UrpTypedPreparation>() as u32,
            abi_version: 1,
            station: &station,
            settings: context.dto.as_ptr(),
            settings_length: context.dto.len() as u32,
        };
        // SAFETY: all borrowed values live through synchronous preparation.
        unsafe {
            (crate::preparation::descriptor().prepare)(
                &args,
                None,
                ptr::null_mut(),
                &mut context.owner,
            )
        }
    }
    let source = "[a]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\n[hardware]\nhardware_voter_reporting=1\n[local]\noutput_gain_db=-3\n[link]\ninput_gain_db=2\n[voice_telemetry]\noutput_gain_db=-2\n";

    let remote_source = concat!(
        "[a]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\n",
        "receive_signaling=ctcss\nctcss_source=usb\n",
        "[remote_base a]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\n",
        "hamlib_device=/dev/ttyUSB0\nhamlib_option_ptt_type=RIG\nstartup_memory=local A\n",
        "allowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\n",
        "allowed_tx_ranges_hz=144000000-148000000\nmax_power_w=10\n",
        "dtmf_access=open\ndtmf_prefix=65\npower_low_w=1\nscan_slow_ms=3000\n",
        "timeout_ms=900\nretries=3\ntimeout_retries=4\nptt_source=hamlib\ncor_source=dsp\n",
        "[remote_memory a local A]\nselector=5\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\n",
        "ctcss_decode_source=external\nctcss_decode_hz=114.8\ndecode_control_source=hamlib\n",
        "power_w=5\nreceive_vfo=A\ntransmit_vfo=B\nsplit=yes\nrit_hz=100\nrit_enabled=no\n",
        "[remote_scan a channel list]\nselector=2\nmembers=local A\npriority=local A\ndwell_ms=900\n"
    );
    let remote_dto = r#"{"remote":{
        "backend":"hamlib","audio_source":"cm119","hamlib_model":"RIG_MODEL_DUMMY",
        "hamlib_options":{"rig_pathname":"/dev/ttyUSB0","ptt_type":"RIG"},
        "timeout_ms":900,"retries":3,"timeout_retries":4,"poll_interval_ms":500,
        "ptt_source":"hamlib","cor_source":"dsp","external_encode_inverted":false,
        "receive_ranges":[[144000000,148000000]],"transmit_ranges":[[144000000,148000000]],"receive_only_ranges":[],
        "modes":[1],"maximum_power_mw":10000,"startup_memory":5,"dtmf_access":"open","dtmf_prefix":"65",
        "power_presets_mw":[1000,null,null],"scan_presets_ms":[3000,null,null],"tune_duration_ms":1000,
        "memories":[{"selector":5,"name":"local A","state":{"receive_hz":146520000,"transmit_hz":146520000,
        "mode":1,"bandwidth_hz":0,"receive_vfo":1,"transmit_vfo":2,"split":true,"offset_hz":0,
        "rit_hz":100,"rit_enabled":false,"xit_hz":0,"xit_enabled":false,"power_mw":5000,
        "encode":{"source":"disabled","enabled":false,"tone_tenths_hz":1000,"dcs_code":0,"inverted":false},
        "decode":{"source":"external","control_source":"rig","enabled":true,"tone_tenths_hz":1148,"dcs_code":0,"inverted":false}}}],
        "scan_lists":[{"selector":2,"name":"channel list","members":[5],"priority":5,"dwell_ms":900,"resume_delay_ms":2000,"settle_ms":0}]
    }}"#;
    let ordinary_dto=br#"{"station":{"voter_reporting":1},"local":{"output_gain_db":-3},"link":{"input_gain_db":2},"voice_telemetry":{"output_gain_db":-2}}"#;
    let dcs_source = "[a]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\nreceive_signaling=dcs\ndcs_receive_code=125I\ntransmit_signaling=dcs\ntransmit_dcs_code=134N\ntransmit_dcs_level_dbfs=-16\ntransmit_dcs_turnoff_enabled=no\ntransmit_dcs_turnoff_duration_ms=155\n";
    for (source, dto) in [
        (source, ordinary_dto.as_slice()),
        (
            "[a]\n[hardware]\nhardware_gpio_usb_port_path=1-5\n",
            br#"{"station":{"gpio_usb_port_path":"1-5"}}"#.as_slice()
        ),
        (remote_source, remote_dto.as_bytes()),
        (dcs_source, b"{}".as_slice()),
        (
            dcs_source,
            br#"{"station":{"dcs":{"receive_code":{"value":85,"polarity":"inverted"},"transmit_code":{"value":92,"polarity":"normal"},"peak_dbfs":-16,"turnoff_code_enabled":false,"turnoff_duration_ms":155}}}"#.as_slice()
        ),
        (
            "[a]\n[radio]\ndevice_selection=automatic_lowest_alsa_card\nreceive_signaling=ctcss\nctcss_source=usb\n",
            b"{}".as_slice()
        ),
        (
            include_str!("tests/preparation/complete.ini"),
            include_bytes!("tests/preparation/complete.json").as_slice(),
        ),
    ] {
        let request = args(source, "a");
        let mut ini = ptr::null_mut();
        let mut comparison = Comparison {
            dto,
            owner: ptr::null_mut(),
        };
        let mut same = 0;
        // SAFETY: each complete borrowed request, output and visitor context lives through its call.
        unsafe {
            assert_eq!(resolve(&request, None, ptr::null_mut(), &mut ini), 0);
            assert_eq!(
                visit(ini, Some(prepare), ptr::from_mut(&mut comparison).cast()),
                0
            );
            assert_eq!(
                (crate::preparation::descriptor().equal)(ini, comparison.owner, &mut same),
                0
            );
            if same == 0 {
                let left = prepared(ini).unwrap();
                let right = prepared(comparison.owner).unwrap();
                assert_eq!(left.configuration.station, right.configuration.station);
                assert_eq!(left.configuration.local, right.configuration.local);
                assert_eq!(left.configuration.link, right.configuration.link);
                assert_eq!(left.configuration.voice_telemetry, right.configuration.voice_telemetry);
                assert_eq!(left, right);
            }
            assert_eq!(same, 1);
            if source == remote_source {
                let decoder = prepared(comparison.owner).unwrap().remote.as_ref().unwrap().memories[0].state.decode;
                assert_eq!(decoder.source, usbradioplus_core::RemoteSignalSource::External);
                assert_eq!(decoder.control_source, Some(usbradioplus_core::RemoteSignalSource::Rig));
                assert_eq!(decoder.tone_tenths_hz, 1148);
                let follow_qualification = remote_dto.replace("\"control_source\":\"rig\",", "");
                let mut follow = Comparison { dto: follow_qualification.as_bytes(), owner: ptr::null_mut() };
                assert_eq!(visit(ini, Some(prepare), ptr::from_mut(&mut follow).cast()), 0);
                assert_eq!(prepared(follow.owner).unwrap().remote.as_ref().unwrap().memories[0].state.decode.control_source, None);
                assert_eq!((crate::preparation::descriptor().equal)(comparison.owner, follow.owner, &mut same), 0);
                assert_eq!(same, 0);
                destroy(follow.owner);
                for invalid in [
                    remote_dto.replace("\"encode\":{\"source\"", "\"encode\":{\"control_source\":\"rig\",\"source\""),
                    remote_dto.replace("RIG_MODEL_DUMMY", "RIG_MODEL_NOT_REAL"),
                    remote_dto.replace("\"external_encode_inverted\":false", "\"external_encode_inverted\":false,\"external_encode_output\":{\"usb\":1}"),
                    remote_dto.replace("\"ptt_type\":\"RIG\"", "\"ptt_type\":\"RIG\",\"ptt_type\":\"RIG\""),
                ] {
                    let mut invalid = Comparison {dto: invalid.as_bytes(), owner: ptr::null_mut()};
                    assert_ne!(visit(ini, Some(prepare), ptr::from_mut(&mut invalid).cast()), 0);
                    assert!(invalid.owner.is_null());
                }
                let changed = remote_dto
                    .replace("local A", "  A;# memory  ")
                    .replace("/dev/ttyUSB0", "  /dev/tty;#  ");
                let mut changed = Comparison {
                    dto: changed.as_bytes(),
                    owner: ptr::null_mut(),
                };
                assert_eq!(
                    visit(ini, Some(prepare), ptr::from_mut(&mut changed).cast()),
                    0
                );
                let plan = prepared(changed.owner).unwrap();
                let remote = plan.remote.as_ref().unwrap();
                assert_eq!(remote.memories[0].name, "  A;# memory  ");
                assert_eq!(remote.hamlib_options["rig_pathname"], "  /dev/tty;#  ");
                let mut options: UrpRemoteCommandOptions = std::mem::zeroed();
                options.struct_size = size_of::<UrpRemoteCommandOptions>() as u32;
                options.abi_version = 1;
                assert_eq!(remote_options(changed.owner, &mut options), 0);
                assert_eq!(options.access, 1);
                assert_eq!(&options.prefix[..options.prefix_length as usize], b"65");
                assert_eq!(
                    (crate::preparation::descriptor().equal)(
                        comparison.owner,
                        changed.owner,
                        &mut same
                    ),
                    0
                );
                assert_eq!(same, 0);
                destroy(changed.owner);
            }
            destroy(ini);
            destroy(comparison.owner);
        }
    }
}
