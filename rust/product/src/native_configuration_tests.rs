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
