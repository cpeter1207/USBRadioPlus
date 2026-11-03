//! Shared processing-profile resolution for native station and peer owners.

use crate::{LinkHandle, Status, copy_abi, ffi_status, link_mut, output_pointer};
use std::ffi::{c_int, c_void};
use usbradioplus_asl3::LinkDirection;
use usbradioplus_core::{
    ChainRole, ConfigDocument, GraphDescriptionFactory, ProcessingChain, ResolutionWarning,
    ResolvedProfile,
};
use usbradioplus_driver::LinkProcessingFactory;
use usbradioplus_ffmpeg::GraphProvider;
use usbradioplus_radio::SessionConfig;
use usbradioplus_runtime::ExplicitNativeProcessingPlan;

const SOURCE: &str = "rpt_advanced.conf";
const AGC_PLUGIN_PATH: &str = env!("USBRADIOPLUS_AGC_PLUGIN_PATH");

/// Borrowed source-chain sections and node selectors from the controller configuration.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UrpNativeProcessingArgs {
    /// Complete structure size in bytes.
    pub struct_size: u32,
    /// Request ABI, one.
    pub abi_version: u32,
    /// UTF-8 processing configuration, not NUL terminated.
    pub configuration: *const u8,
    /// Configuration byte count; zero selects no explicit profiles.
    pub configuration_length: u32,
    /// UTF-8 node name, not NUL terminated.
    pub node: *const u8,
    /// Node-name byte count.
    pub node_length: u32,
}

/// Synchronous, non-audio diagnostic callback; text is borrowed only for the call.
pub type UrpNativeProcessingDiagnostic = unsafe extern "C" fn(*mut c_void, *const u8, u32);

#[derive(Default)]
pub(super) struct Profiles {
    pub local: Option<ProcessingChain>,
    pub link: Option<ProcessingChain>,
    pub voice_telemetry: Option<ProcessingChain>,
    pub warnings: Vec<ResolutionWarning>,
}

impl Profiles {
    pub(super) fn resolve(configuration: &str, node: &str) -> Result<Self, Status> {
        let document = ConfigDocument::new(configuration);
        let sections = document.section_names();
        let selectors = document.explicit_values(node);
        let mut warnings = Vec::new();
        let mut role = |kind: &str, role| -> Result<Option<ProcessingChain>, Status> {
            let scoped = format!("{kind} {node}");
            let selector = format!("{kind}_profile");
            let configured = sections.iter().any(|section| {
                section.eq_ignore_ascii_case(kind) || section.eq_ignore_ascii_case(&scoped)
            }) || selectors
                .keys()
                .any(|name| name.eq_ignore_ascii_case(&selector));
            if !configured {
                return Ok(None);
            }
            let resolved = ResolvedProfile::from_document(&document, SOURCE, node, role)
                .map_err(|_| Status::InvalidConfiguration)?;
            warnings.extend_from_slice(resolved.warnings());
            Ok(Some(resolved.into_chain()))
        };
        let local = role("local", ChainRole::LocalReceive)?;
        let link = role("link", ChainRole::Link)?;
        let voice_telemetry = role("voice_telemetry", ChainRole::VoiceTelemetry)?;
        Ok(Self {
            local,
            link,
            voice_telemetry,
            warnings,
        })
    }

    pub(super) fn apply(
        self,
        radio: &mut SessionConfig,
        processing: &mut ExplicitNativeProcessingPlan,
    ) {
        for warning in &self.warnings {
            eprintln!("{warning}");
        }
        if let Some(local) = &self.local {
            // Zero represents a disabled source; finite configured dB gains are positive.
            if radio.receive_input_gain != 0.0 {
                radio.receive_input_gain = 10.0_f64.powf(local.input_gain_db / 20.0) as f32;
            }
        }
        processing.local = self.local;
        processing.voice_telemetry = self.voice_telemetry;
    }
}

unsafe fn from_args(args: *const UrpNativeProcessingArgs) -> Result<Profiles, Status> {
    // SAFETY: the synchronous caller supplies the advertised request and string spans.
    let args = unsafe { copy_abi(args)? };
    // SAFETY: the validated request borrows its readable text spans for this call.
    let (configuration, node) = unsafe {
        (
            crate::native::text(args.configuration, args.configuration_length)?,
            crate::native::text(args.node, args.node_length)?,
        )
    };
    Profiles::resolve(&configuration, &node)
}

pub(crate) unsafe extern "C" fn validate(
    args: *const UrpNativeProcessingArgs,
    diagnostic: Option<UrpNativeProcessingDiagnostic>,
    context: *mut c_void,
) -> c_int {
    ffi_status(|| {
        // SAFETY: configuration checking retains request storage through this call.
        let profiles = unsafe { from_args(args)? };
        if let Some(diagnostic) = diagnostic {
            for warning in profiles.warnings {
                let message = warning.to_string();
                // SAFETY: the host retains its context and callback while this function runs.
                unsafe { diagnostic(context, message.as_ptr(), message.len() as u32) };
            }
        }
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn link_prepare(
    args: *const UrpNativeProcessingArgs,
    ffmpeg: *const c_void,
    maximum_frames: u32,
    output: *mut *mut c_void,
) -> c_int {
    ffi_status(|| {
        let output = output_pointer(output)?;
        if !(1..=4096).contains(&maximum_frames) {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the caller retains the request strings throughout preparation.
        let Some(profile) = (unsafe { from_args(args)? }).link else {
            return Ok(());
        };
        // SAFETY: the host retains this released descriptor through graph destruction.
        let provider = unsafe { GraphProvider::from_raw_descriptor(ffmpeg) }
            .map_err(|_| Status::IncompatibleAbi)?;
        let descriptions = GraphDescriptionFactory::new(AGC_PLUGIN_PATH)
            .map_err(|_| Status::InvalidConfiguration)?;
        let description = descriptions
            .dynamics(&profile)
            .map_err(|_| Status::SetupFailed)?;
        let processing = LinkProcessingFactory::new(provider, descriptions)
            .prepare(&profile, 48_000, maximum_frames)
            .map_err(|_| Status::SetupFailed)?;
        let link = Box::new(LinkHandle {
            processing,
            description,
            sample_rate_hz: 48_000,
            maximum_frame_count: maximum_frames,
        });
        // SAFETY: the output was validated; ownership transfers to link_destroy.
        unsafe { output.as_ptr().write(Box::into_raw(link).cast()) };
        Ok(())
    })
}

pub(crate) unsafe extern "C" fn link_process_f32(
    link: *mut c_void,
    direction: u32,
    sample_rate_hz: u32,
    samples: *mut f32,
    sample_count: u32,
) -> c_int {
    ffi_status(|| {
        // SAFETY: the serial media owner retains one live prepared link handle.
        let link = unsafe { link_mut(link)? };
        let direction = match direction {
            crate::URP_AST_LINK_DIRECTION_READ => LinkDirection::Read,
            crate::URP_AST_LINK_DIRECTION_WRITE => LinkDirection::Write,
            _ => return Err(Status::InvalidArgument),
        };
        if samples.is_null() || (samples as usize) % std::mem::align_of::<f32>() != 0 {
            return Err(Status::InvalidArgument);
        }
        // SAFETY: the media owner advertises this complete writable normalized-F32 span.
        let samples = unsafe { std::slice::from_raw_parts_mut(samples, sample_count as usize) };
        let _ = link.process_f32(direction, sample_rate_hz, samples);
        Ok(())
    })
}

pub(super) const fn agc_plugin_path() -> &'static str {
    AGC_PLUGIN_PATH
}

#[cfg(test)]
#[path = "native_processing_tests.rs"]
mod tests;
