//! Owned binding between prepared station media and released runtime providers.

use std::fmt;
use std::sync::Arc;

use usbradioplus_asl3::{
    AdapterSetupError, AppRptConverter, ControllerState, EchoConfiguration, ReceivePublisher,
};
use usbradioplus_radio::{
    ControlObserver, RadioError, RadioProvider, ReceiveEndpoint, TransmitEndpoint,
};
use usbradioplus_ring::RingProvider;
use usbradioplus_runtime::ProcessingGeneration;

use crate::{
    ControllerTransport, PreparedStation, ProgramRingConsumer, ProgramRingSetupError, StationPlan,
    prepare_program_ring,
};

/// ASL3 controller resources selected for one prepared station.
pub enum ControllerSetup {
    /// Fixed 8 kHz app_rpt compatibility transport.
    AppRpt {
        /// Prepared native-to-app_rpt sample-rate converter.
        converter: Box<dyn AppRptConverter>,
        /// Bounded callback-to-Asterisk handoff slots.
        handoff_slots: usize,
        /// Retained app_rpt echo configuration.
        echo: EchoConfiguration,
    },
    /// Native 48 kHz rpt_advanced transport.
    RptAdvanced {
        /// Bounded callback-to-Asterisk handoff slots.
        handoff_slots: usize,
    },
}

impl ControllerSetup {
    const fn transport(&self) -> ControllerTransport {
        match self {
            Self::AppRpt { .. } => ControllerTransport::AppRpt,
            Self::RptAdvanced { .. } => ControllerTransport::RptAdvanced,
        }
    }

    fn prepare(
        self,
        program: Box<dyn usbradioplus_asl3::ProgramProducer>,
    ) -> Result<(ReceivePublisher, ControllerState), AdapterSetupError> {
        match self {
            Self::AppRpt {
                converter,
                handoff_slots,
                echo,
            } => ControllerState::prepare_app_rpt(converter, program, handoff_slots, echo),
            Self::RptAdvanced { handoff_slots } => {
                ControllerState::prepare_advanced(program, handoff_slots)
            }
        }
    }
}

/// Failure while binding one already-prepared station to runtime providers.
#[derive(Debug)]
pub enum StationMediaError {
    /// Controller resources target the other configured transport.
    ControllerTransportMismatch,
    /// The released program-ring provider rejected setup.
    ProgramRing(ProgramRingSetupError),
    /// The ASL3 callback handoff rejected setup.
    Controller(AdapterSetupError),
    /// The released radio provider rejected setup or warmup.
    Radio(RadioError),
}

impl fmt::Display for StationMediaError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ControllerTransportMismatch => {
                formatter.write_str("controller setup does not match the station transport")
            }
            Self::ProgramRing(error) => error.fmt(formatter),
            Self::Controller(error) => error.fmt(formatter),
            Self::Radio(error) => write!(formatter, "radio session setup failed: {error}"),
        }
    }
}

impl std::error::Error for StationMediaError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::ControllerTransportMismatch => None,
            Self::ProgramRing(error) => Some(error),
            Self::Controller(error) => Some(error),
            Self::Radio(error) => Some(error),
        }
    }
}

impl From<ProgramRingSetupError> for StationMediaError {
    fn from(error: ProgramRingSetupError) -> Self {
        Self::ProgramRing(error)
    }
}

impl From<AdapterSetupError> for StationMediaError {
    fn from(error: AdapterSetupError) -> Self {
        Self::Controller(error)
    }
}

impl From<RadioError> for StationMediaError {
    fn from(error: RadioError) -> Self {
        Self::Radio(error)
    }
}

/// Persistent owners for one warmed station media binding.
pub struct StationMedia {
    /// Immutable effective station plan.
    pub plan: StationPlan,
    /// Sole local receive callback owner.
    pub receive: StationReceive,
    /// Sole radio transmit callback owner.
    pub transmit: StationTransmit,
    /// Sole radio event and diagnostic observer.
    pub control: StationControl,
    /// Serialized ASL3 delivery, program, and compatibility owner.
    pub controller: ControllerState,
}

/// Sole local receive callback owner and its ASL3 publication endpoint.
pub struct StationReceive {
    radio: ReceiveEndpoint<'static>,
    controller: ReceivePublisher,
    _resources: Arc<MediaResources>,
}

impl StationReceive {
    /// Borrow the released radio receive endpoint for one bounded callback.
    pub fn radio(&mut self) -> &mut ReceiveEndpoint<'static> {
        &mut self.radio
    }

    /// Borrow the matching non-waiting ASL3 receive publisher.
    pub fn controller(&mut self) -> &mut ReceivePublisher {
        &mut self.controller
    }
}

/// Sole hardware-paced radio transmit callback owner.
pub struct StationTransmit {
    radio: TransmitEndpoint<'static>,
    _resources: Arc<MediaResources>,
}

impl StationTransmit {
    /// Borrow the released radio transmit endpoint for one bounded callback.
    pub fn radio(&mut self) -> &mut TransmitEndpoint<'static> {
        &mut self.radio
    }
}

/// Sole control-plane observer of radio events and diagnostics.
pub struct StationControl {
    radio: ControlObserver<'static>,
    _resources: Arc<MediaResources>,
}

impl StationControl {
    /// Borrow the released radio control observer.
    pub fn radio(&mut self) -> &mut ControlObserver<'static> {
        &mut self.radio
    }
}

struct MediaResources {
    processing: ProcessingGeneration,
    program_ring: ProgramRingConsumer,
}

// SAFETY: MediaResources has no public access after binding. Session creation
// splits every processor and the ring consumer into the radio ABI's one serial
// receive owner or one serial transmit owner. The three endpoint wrappers each
// retain this allocation, and field declaration order drops the endpoint before
// its resource lease, so the final session destruction always precedes resource
// destruction.
unsafe impl Send for MediaResources {}
// SAFETY: The same split gives distinct callback contexts exclusive mutation;
// only the radio ABI's lock-free observer reads concurrently.
unsafe impl Sync for MediaResources {}

impl PreparedStation {
    /// Bind prepared processing to the released program-ring and radio providers.
    ///
    /// This control-plane operation allocates and warms all remaining state. It
    /// opens no device and does not publish the resulting callbacks.
    ///
    /// # Errors
    ///
    /// Returns an error when controller resources target the wrong transport or
    /// any released provider rejects construction or warmup.
    pub fn bind_media(
        self,
        ring_provider: RingProvider,
        radio_provider: RadioProvider,
        controller_setup: ControllerSetup,
    ) -> Result<StationMedia, StationMediaError> {
        if self.plan().transport() != controller_setup.transport() {
            return Err(StationMediaError::ControllerTransportMismatch);
        }
        let (program, program_ring) =
            prepare_program_ring(ring_provider, self.plan().program_ring())?;
        let (controller_publisher, controller) = controller_setup.prepare(program)?;
        let (plan, processing) = self.into_parts();
        let mut resources = Arc::new(MediaResources {
            processing,
            program_ring,
        });
        // A fresh Arc is uniquely mutable until the callback ports are captured.
        let resources_mut = Arc::get_mut(&mut resources).expect("a new Arc has one owner");
        // SAFETY: each returned endpoint retains an Arc lease to this stable
        // allocation, and its endpoint field is dropped before that lease.
        let (receive, transmit, control) =
            unsafe { prepare_owned_session(radio_provider, plan.radio(), resources_mut)? };
        Ok(StationMedia {
            plan,
            receive: StationReceive {
                radio: receive,
                controller: controller_publisher,
                _resources: Arc::clone(&resources),
            },
            transmit: StationTransmit {
                radio: transmit,
                _resources: Arc::clone(&resources),
            },
            control: StationControl {
                radio: control,
                _resources: resources,
            },
            controller,
        })
    }
}

type OwnedEndpoints = (
    ReceiveEndpoint<'static>,
    TransmitEndpoint<'static>,
    ControlObserver<'static>,
);

unsafe fn prepare_owned_session(
    provider: RadioProvider,
    config: &usbradioplus_radio::SessionConfig,
    resources: &mut MediaResources,
) -> Result<OwnedEndpoints, RadioError> {
    let program_ring = resources.program_ring.radio_port();
    let ports = resources.processing.session_ports(program_ring);
    let endpoints = provider.prepare(config, ports)?.split();
    // SAFETY: the caller keeps the stable MediaResources allocation live until
    // all returned endpoints have been dropped. Erasing only the borrow marker
    // does not alter any endpoint representation or callback pointer.
    Ok(unsafe {
        std::mem::transmute::<
            (
                ReceiveEndpoint<'_>,
                TransmitEndpoint<'_>,
                ControlObserver<'_>,
            ),
            OwnedEndpoints,
        >(endpoints)
    })
}

#[cfg(test)]
#[path = "tests/media_tests.rs"]
pub(crate) mod tests;
