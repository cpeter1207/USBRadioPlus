//! Synchronous ownership transfer from a borrowed physical preparation to a host plan.
use super::*;
use std::fmt::Write as _;

/// Borrowed station identity and its provider preparation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpBoundStationRequest {
    /// Complete request size.
    pub struct_size: u32,
    /// One.
    pub abi_version: u32,
    /// Registry UUID bytes.
    pub port_uuid: [u8; 16],
    /// Checked nonzero semantic staging identity.
    pub candidate_identity: u64,
    /// Producer of the immutable preparation.
    pub product: *const UrpAstDescriptor,
    /// Borrowed through the synchronous call.
    pub prepared: *const c_void,
}

/// Sized operations for taking an independent copy of a typed station preparation.
#[repr(C)]
pub struct UrpStationOwnershipDescriptor {
    /// Number of readable descriptor bytes.
    pub struct_size: u32,
    /// Station ownership ABI, one.
    pub abi_version: u32,
    /// Exact NUL-terminated capability name.
    pub capability_name: *const c_char,
    /// Copy a matched immutable provider preparation. The product1 destroy operation releases it.
    pub copy_prepared:
        unsafe extern "C" fn(*const UrpAstDescriptor, *const c_void, *mut *mut c_void) -> c_int,
    /// Reserve on the exact registered driver, after publication.
    pub channel_reserve: unsafe extern "C" fn(
        *mut c_void,
        *const UrpBoundStationRequest,
        *const UrpAstChannelReserveArgs,
        *mut *mut c_void,
    ) -> c_int,
    /// Stage on the existing channel's serialized control owner.
    pub channel_stage: unsafe extern "C" fn(*mut c_void, *const UrpBoundStationRequest) -> c_int,
    /// Confirm adoption or the return of the original generation on the same owner.
    pub channel_finish: unsafe extern "C" fn(*mut c_void, u32) -> c_int,
}
// SAFETY: the only data pointer addresses a static string, and the operation has no mutable state.
unsafe impl Sync for UrpStationOwnershipDescriptor {}

static DESCRIPTOR: UrpStationOwnershipDescriptor = UrpStationOwnershipDescriptor {
    struct_size: size_of::<UrpStationOwnershipDescriptor>() as u32,
    abi_version: 1,
    capability_name: c"usbradioplus.station-ownership1".as_ptr(),
    copy_prepared,
    channel_reserve: reserve,
    channel_stage: stage,
    channel_finish: finish,
};

/// Return the immutable station-ownership table for this exact product module.
#[unsafe(no_mangle)]
pub extern "C" fn usbradioplus_station_ownership_descriptor() -> *const UrpStationOwnershipDescriptor
{
    &DESCRIPTOR
}

unsafe extern "C" fn copy_prepared(
    product: *const UrpAstDescriptor,
    prepared: *const c_void,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        if product != super::usbradioplus_product_descriptor_v1() {
            return Err(Status::IncompatibleAbi);
        }
        // SAFETY: the caller retains an immutable typed configuration from the verified product.
        let copy = unsafe { native_configuration::copy_typed(prepared)? };
        // SAFETY: successful admission transfers the independent owner to the writable output.
        unsafe { output.as_ptr().write(Box::into_raw(Box::new(copy)).cast()) };
        Ok(())
    })
}

pub(super) struct TypedChannel {
    pub(super) port: [u8; 16],
    identity: u64,
    pub(super) active: native::ResolvedRequest,
    pub(super) selected: Option<usbradioplus_station::SelectedHardwarePlan>,
    pending: Option<TypedUpdate>,
}
struct TypedUpdate {
    identity: u64,
    prepared: native::ResolvedRequest,
    acknowledged: bool,
}

fn generation(driver: &DriverInner) -> Result<u64, Status> {
    driver
        .station_generation
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| {
            old.checked_add(1)
        })
        .map_err(|_| Status::InternalFailure)
}
fn station_plan(name: &str, mut original: native::ResolvedRequest, generation: u64) -> StationPlan {
    original.set_generation(generation, MAXIMUM_FRAME_COUNT);
    StationPlan::native(
        name,
        original.configuration,
        original.hardware,
        original.radio,
        original.processing,
    )
    .with_remote_configuration(original.remote)
}
unsafe fn request<'a>(
    value: *const UrpBoundStationRequest,
) -> Result<(&'a UrpBoundStationRequest, &'a native::ResolvedRequest), Status> {
    if value.is_null() || !value.is_aligned() {
        return Err(Status::InvalidArgument);
    }
    // SAFETY: every non-null request supplies its readable size prefix.
    if (unsafe { value.cast::<u32>().read() } as usize) < size_of::<UrpBoundStationRequest>() {
        return Err(Status::IncompatibleAbi);
    }
    // SAFETY: the complete request is available after the size check.
    let value = unsafe { &*value };
    if value.abi_version != 1
        || value.product != usbradioplus_product_descriptor_v1()
        || value.candidate_identity == 0
    {
        return Err(Status::IncompatibleAbi);
    }
    // SAFETY: the matched caller retains its physical-preparation owner throughout this call.
    Ok((value, unsafe {
        native_configuration::typed_request(value.prepared)?
    }))
}

unsafe extern "C" fn reserve(
    driver: *mut c_void,
    value: *const UrpBoundStationRequest,
    args: *const UrpAstChannelReserveArgs,
    out: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let out = output_pointer(out)?;
        // SAFETY: the registered driver, dormant plan and callback arguments are retained by the requester.
        let (driver, (request, original), args) =
            unsafe { (&driver_ref(driver)?.0, request(value)?, copy_abi(args)?) };
        if args.transport != URP_AST_TRANSPORT_RPT_ADVANCED || args.channel_context.is_null() {
            return Err(Status::InvalidArgument);
        }
        let (mode, controller) = controller_configuration(args.transport)?;
        let mut name = String::with_capacity(32);
        for byte in request.port_uuid {
            write!(name, "{byte:02x}").expect("writing to a String cannot fail");
        }
        if !driver
            .reservations
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .typed_ports
            .insert(request.port_uuid)
        {
            return Err(Status::ChannelBusy);
        }
        let result = (|| {
            let generation = generation(driver)?;
            let mut resolved = original.clone();
            let (preflight, selected) = HardwarePreflight::native(
                &resolved.hardware,
                resolved.configuration.station.hardware.eeprom_enabled,
                driver.audio,
                driver.gpio,
                MAXIMUM_FRAME_COUNT,
            )
            .map_err(|error| {
                driver.operations.log(URP_AST_LOG_ERROR, &error.to_string());
                Status::SetupFailed
            })?;
            if preflight.apply_startup_tuning(&mut resolved.configuration) {
                usbradioplus_station::apply_native_radio_tuning(
                    &resolved.configuration,
                    &mut resolved.radio,
                );
            }
            let jitter = jitter_config(&resolved.configuration.station.asterisk);
            let plan = station_plan(&name, resolved, generation);
            let remote = prepare_remote(&plan).map_err(|_| Status::SetupFailed)?;
            let media = driver
                .factory
                .prepare_plan(plan, controller)
                .map_err(|_| Status::SetupFailed)?;
            let typed = TypedChannel {
                port: request.port_uuid,
                identity: request.candidate_identity,
                active: original.clone(),
                selected: Some(selected),
                pending: None,
            };
            prepared_channel(
                driver,
                &name,
                args.channel_context,
                mode,
                remote,
                media,
                preflight,
                jitter,
                Some(typed),
            )
        })();
        let handle = match result {
            Ok(value) => value,
            Err(status) => {
                driver
                    .reservations
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .typed_ports
                    .remove(&request.port_uuid);
                return Err(status);
            }
        };
        // SAFETY: the requester receives the same unique channel type as a legacy reservation.
        unsafe { out.as_ptr().write(Box::into_raw(handle).cast()) };
        Ok(())
    })
}

unsafe extern "C" fn stage(raw: *mut c_void, value: *const UrpBoundStationRequest) -> c_int {
    // The product1 API uses positive one for asynchronous progress. The station
    // API distinguishes an untouched handoff (one) from continued polling (two).
    let result = ffi_status(|| {
        // SAFETY: the original Asterisk taskprocessor serializes this channel; the caller retains the complete request.
        let ((channel, control), (request, next)) =
            unsafe { (channel_control(raw)?, request(value)?) };
        if !control.running {
            return Err(Status::NotReady);
        }
        let typed = control.typed.as_mut().ok_or(Status::InvalidArgument)?;
        if typed.port != request.port_uuid || typed.identity == request.candidate_identity {
            return Err(Status::InvalidArgument);
        }
        if let Some(pending) = &typed.pending {
            if pending.identity != request.candidate_identity || &pending.prepared != next {
                return Err(Status::ChannelBusy);
            }
            if pending.acknowledged {
                return Ok(());
            }
        } else {
            let hardware = control.hardware.as_ref().ok_or(Status::NotReady)?;
            let mut resolved = next.clone();
            hardware
                .preserve_startup_tuning(&typed.active.configuration, &mut resolved.configuration);
            if resolved.configuration.station.receive.squelch_level
                != next.configuration.station.receive.squelch_level
                || resolved.configuration.station.ctcss.transmit_peak_dbfs
                    != next.configuration.station.ctcss.transmit_peak_dbfs
            {
                usbradioplus_station::apply_native_radio_tuning(
                    &resolved.configuration,
                    &mut resolved.radio,
                );
            }
            let candidate = station_plan(
                &channel.reservation,
                resolved,
                generation(channel.driver())?,
            );
            if !hardware.supports_live_update(&candidate) {
                return Err(Status::Activation(3));
            }
            let jitter = jitter_config(&next.configuration.station.asterisk);
            let mixers = std::array::from_fn(|i| typed.active.gains[i] != next.gains[i]);
            let snapshot =
                channel.setup_result(channel.driver().factory.update_snapshot(hardware))?;
            let worker = channel.setup_result(
                std::thread::Builder::new()
                    .name("urp-station-update".into())
                    .spawn(move || snapshot.prepare(candidate)),
            )?;
            control.reload = Some(PreparedChannelReload::Preparing {
                worker,
                jitter,
                mixers,
                cancelled: false,
            });
            typed.pending = Some(TypedUpdate {
                identity: request.candidate_identity,
                prepared: next.clone(),
                acknowledged: false,
            });
            return Err(Status::ReloadPending);
        }
        let preparing = matches!(
            control.reload,
            Some(PreparedChannelReload::Preparing { .. })
        );
        // SAFETY: no borrowed channel/control references are used after this point.
        if preparing {
            // SAFETY: the original serialized channel owns the staged replacement.
            let code = unsafe { super::channel_reload_prepare(raw) };
            if code != 0 {
                return Err(Status::Activation(code));
            }
        }
        // SAFETY: serialized channel and prepared generation are retained by the same taskprocessor.
        let code = unsafe { super::channel_reload_activate(raw) };
        if code != 0 {
            return Err(Status::Activation(code));
        }
        // SAFETY: the taskprocessor retains exclusive access after both audio endpoints acknowledged.
        let (_, control) = unsafe { channel_control(raw)? };
        control
            .typed
            .as_mut()
            .and_then(|typed| typed.pending.as_mut())
            .ok_or(Status::NotReady)?
            .acknowledged = true;
        Ok(())
    });
    match result {
        URP_AST_RELOAD_PENDING => 2,
        3 => 1,
        other => other,
    }
}

unsafe extern "C" fn finish(raw: *mut c_void, commit: u32) -> c_int {
    let code = ffi_status(|| {
        let commit = boolean(commit)?;
        // SAFETY: the live channel remains exclusively serialized throughout both phases.
        let (_, control) = unsafe { channel_control(raw)? };
        let Some(pending) = control
            .typed
            .as_ref()
            .ok_or(Status::InvalidArgument)?
            .pending
            .as_ref()
        else {
            return if commit {
                Err(Status::NotReady)
            } else {
                Ok(())
            };
        };
        if commit && !pending.acknowledged {
            return Err(Status::NotReady);
        }
        // SAFETY: all earlier references have ended, and the worker owns the exact active/pending plans.
        let code = unsafe { super::channel_reload_finish(raw, u32::from(commit)) };
        if code != 0 {
            return Err(Status::Activation(code));
        }
        // SAFETY: this is still the same serialized live station after its adoption ack.
        let (_, control) = unsafe { channel_control(raw)? };
        let typed = control.typed.as_mut().ok_or(Status::InvalidArgument)?;
        let TypedUpdate {
            identity, prepared, ..
        } = typed.pending.take().ok_or(Status::NotReady)?;
        if commit {
            control
                .hardware
                .as_mut()
                .ok_or(Status::NotReady)?
                .adopt_native_gain_intents(prepared.gains);
            typed.identity = identity;
            typed.active = prepared;
        }
        Ok(())
    });
    if code == URP_AST_RELOAD_PENDING {
        2
    } else {
        code
    }
}
