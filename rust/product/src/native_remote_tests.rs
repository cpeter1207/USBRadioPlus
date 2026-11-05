use super::*;

#[derive(Default)]
struct Observed {
    remote: Option<usbradioplus_core::ResolvedRemoteConfiguration>,
    has_hid: bool,
    error: Option<c_int>,
    commands: Option<crate::UrpRemoteCommandOptions>,
}

unsafe extern "C" fn inspect(
    context: *mut c_void,
    request: *const UrpNativeStationConfig,
) -> c_int {
    // SAFETY: the test retains its observation and the visitor lends a complete request.
    let observed = unsafe { &mut *context.cast::<Observed>() };
    // SAFETY: all source spans remain valid through this visitor's synchronous resolution.
    match unsafe { resolve_request(&*request, 1, 960) } {
        Ok(resolved) => {
            observed.remote = resolved.remote;
            observed.has_hid = resolved.hardware.cm119.is_some();
        }
        Err(error) => observed.error = Some(error.code()),
    }
    0
}

fn resolve_remote(source: &str) -> Observed {
    let node = "524950";
    let args = crate::UrpNativeProcessingArgs {
        struct_size: size_of::<crate::UrpNativeProcessingArgs>() as u32,
        abi_version: 1,
        configuration: source.as_ptr(),
        configuration_length: source.len() as u32,
        node: node.as_ptr(),
        node_length: node.len() as u32,
    };
    let mut owner = std::ptr::null_mut();
    let mut observed = Observed::default();
    // SAFETY: source, node and all callback storage outlive this owned resolution/visit.
    unsafe {
        assert_eq!(
            crate::native_configuration::resolve(&args, None, std::ptr::null_mut(), &mut owner),
            0
        );
        assert_eq!(
            crate::native_configuration::visit(
                owner,
                Some(inspect),
                (&mut observed as *mut Observed).cast()
            ),
            0
        );
        // SAFETY: this initialized output and retained configuration remain live.
        let mut commands: crate::UrpRemoteCommandOptions = std::mem::zeroed();
        commands.struct_size = size_of::<crate::UrpRemoteCommandOptions>() as u32;
        commands.abi_version = 1;
        let code = crate::product_descriptor()
            .native_configuration_remote_options
            .unwrap()(owner, &mut commands);
        if code == 0 {
            observed.commands = Some(commands);
            assert_eq!(
                crate::product_descriptor()
                    .native_configuration_remote_options
                    .unwrap()(owner, std::ptr::null_mut()),
                crate::URP_AST_INVALID_ARGUMENT
            );
            commands.struct_size -= 1;
            assert_eq!(
                crate::product_descriptor()
                    .native_configuration_remote_options
                    .unwrap()(owner, &mut commands),
                crate::URP_AST_INCOMPATIBLE_ABI
            );
        } else {
            observed.error = Some(code);
        }
        crate::native_configuration::destroy(owner);
    }
    observed
}

#[test]
fn native_remote_command_options_use_the_shared_resolver_before_hardware() {
    let source = "[524950]\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n";
    for (source, access, prefix) in [
        (source.to_owned(), 2, "6"),
        (
            source.replace(
                "backend=hamlib",
                "backend=hamlib\ndtmf_access=open\ndtmf_prefix=65",
            ),
            1,
            "65",
        ),
    ] {
        let options = resolve_remote(&source).commands.unwrap();
        assert_eq!((options.enabled, options.access), (1, access));
        assert_eq!(
            &options.prefix[..options.prefix_length as usize],
            prefix.as_bytes()
        );
    }
    let options = resolve_remote("[524950]\n").commands.unwrap();
    assert_eq!(
        (options.enabled, options.access, options.prefix_length),
        (0, 0, 0)
    );
}

#[test]
fn native_request_retains_original_remote_configuration_before_hardware_acquisition() {
    let source = "[524950]\n[radio]\ndevice_identifier=hw:1,0\n[remote_base 524950]\nenabled=yes\nbackend=hamlib\nhamlib_model=RIG_MODEL_DUMMY\nstartup_memory=local\nallowed_modes=FM\nallowed_rx_ranges_hz=144000000-148000000\nallowed_tx_ranges_hz=144000000-148000000\n[remote_memory 524950 local]\nselector=1\nrx_frequency_hz=146520000\ntx_frequency_hz=146520000\nmode=FM\n";
    let cm119 = resolve_remote(source);
    assert_eq!(cm119.error, None);
    assert!(cm119.has_hid);
    assert_eq!(
        cm119.remote.unwrap().memories[0].state.receive_hz,
        146520000
    );
    let generic = resolve_remote(&source.replace("enabled=yes", "enabled=yes\naudio_source=alsa"));
    assert_eq!(generic.error, None);
    assert!(!generic.has_hid);
    assert!(generic.remote.is_some());
    let invalid = resolve_remote(&source.replace("startup_memory=local", "startup_memory=missing"));
    assert_eq!(invalid.error, Some(Status::InvalidConfiguration.code()));
    let ordinary = resolve_remote("[524950]\n[radio]\ndevice_identifier=hw:1,0\n");
    assert_eq!(ordinary.error, None);
    assert!(ordinary.has_hid);
    assert!(ordinary.remote.is_none());
}
