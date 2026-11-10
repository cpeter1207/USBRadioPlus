use super::*;

use std::error::Error;

use usbradioplus_asl3::{LinkDirection, LinkProcessOutcome};

use crate::test_support::{
    failing_graph_create_provider, graph_provider, live_failing_graph_provider,
};

#[link(name = "rptadv_ffmpeg_adapter")]
unsafe extern "C" {
    fn rptadv_ffmpeg_adapter_descriptor() -> *const std::ffi::c_void;
}

#[test]
fn native_and_asl_links_forward_real_buffering_without_promoting_warmup_to_programme() {
    // SAFETY: the dynamically linked provider's immutable code/table live through
    // every derived graph, and each PreparedLink retains exactly one PCM owner.
    let provider =
        // SAFETY: immutable provider table/code outlive all serially owned graphs.
        unsafe { GraphProvider::from_raw_descriptor(rptadv_ffmpeg_adapter_descriptor()) }.unwrap();
    let factory = LinkProcessingFactory::new(
        provider,
        GraphDescriptionFactory::new("/unused/agc.so").unwrap(),
    );
    let mut profile = ProcessingChain::shipped(ChainRole::Link);
    profile.peak_limiter.enabled = true;
    for (rate, maximum) in [(48000, 960), (8000, 160)] {
        let mut link = factory.prepare(&profile, rate, maximum).unwrap();
        let prepared = link.buffering_state();
        assert_ne!(
            prepared.flags & usbradioplus_core::PROGRAMME_PREPARED_BOUND_VALID,
            0
        );
        assert_eq!(
            prepared.flags & usbradioplus_core::PROGRAMME_CURRENT_BOUND_VALID,
            0
        );
        if rate == 48000 {
            let mut pcm = [0.25; 960];
            assert_eq!(
                link.process_f32(LinkDirection::Read, rate, &mut pcm),
                LinkProcessOutcome::Processed
            );
        } else {
            let mut pcm = [8000_i16; 160];
            assert_eq!(
                link.process_s16(LinkDirection::Read, rate, &mut pcm),
                LinkProcessOutcome::Processed
            );
        }
        let current = link.buffering_state();
        assert_ne!(
            current.flags & usbradioplus_core::PROGRAMME_CURRENT_BOUND_VALID,
            0
        );
        assert_eq!(
            current.accepted_frames,
            prepared.accepted_frames + u64::from(maximum)
        );
        assert_eq!(
            link.advance_silence(maximum as usize),
            LinkProcessOutcome::Processed
        );
        assert_eq!(
            link.buffering_state().accepted_frames,
            current.accepted_frames + u64::from(maximum)
        );
    }
}

#[test]
fn factory_prepares_warms_and_processes_at_the_link_rate() {
    let factory = LinkProcessingFactory::new(
        graph_provider(),
        GraphDescriptionFactory::new("/usr/lib/agc.so").unwrap(),
    );
    let profile = ProcessingChain::shipped(ChainRole::Link);
    let mut link = factory.prepare(&profile, 8_000, 4).unwrap();
    let mut pcm = [8_000_i16, -8_000];
    assert_eq!(
        link.process_s16(LinkDirection::Read, 8_000, &mut pcm),
        LinkProcessOutcome::Processed
    );
    assert_eq!(pcm, [4_000, -4_000]);
}

#[test]
fn preparation_reports_role_description_and_adapter_failures() {
    let profile = ProcessingChain::shipped(ChainRole::LocalReceive);
    let factory = LinkProcessingFactory::new(
        graph_provider(),
        GraphDescriptionFactory::new("ok").unwrap(),
    );
    assert_eq!(
        factory.prepare(&profile, 8_000, 160).err(),
        Some(LinkPreparationError::IncorrectRole)
    );
    let mut profile = ProcessingChain::shipped(ChainRole::Link);
    profile.enabled = true;
    profile.agc.enabled = true;
    let mut invalid_crossover = profile.clone();
    invalid_crossover.compressor.enabled = true;
    invalid_crossover.compressor.layout = usbradioplus_core::BandLayout::ThreeBand;
    invalid_crossover.compressor.high_crossover_hz = 24_000.0;
    assert_eq!(
        factory.prepare(&invalid_crossover, 8_000, 160).err(),
        Some(LinkPreparationError::Description(
            GraphDescriptionError::CrossoverAboveNyquist
        ))
    );
    let factory = LinkProcessingFactory::new(
        graph_provider(),
        GraphDescriptionFactory::new("bad\0path").unwrap(),
    );
    assert_eq!(
        factory.prepare(&profile, 8_000, 160).err(),
        Some(LinkPreparationError::InvalidDescription)
    );
    let factory = LinkProcessingFactory::new(
        failing_graph_create_provider(),
        GraphDescriptionFactory::new("ok").unwrap(),
    );
    assert_eq!(
        factory.prepare(&profile, 8_000, 160).err(),
        Some(LinkPreparationError::Graph(GraphError::AdapterFailure))
    );
    for (rate, maximum) in [(8_000, 4_097), (7_999, 160), (384_001, 160)] {
        assert_eq!(
            factory.prepare(&profile, rate, maximum).err(),
            Some(LinkPreparationError::Link(LinkError::InvalidConfiguration))
        );
    }
    assert_eq!(
        LinkProcessingFactory::new(
            graph_provider(),
            GraphDescriptionFactory::new("ok").unwrap(),
        )
        .prepare(&profile, 0, 160)
        .err(),
        Some(LinkPreparationError::Link(LinkError::InvalidConfiguration))
    );
    assert_eq!(
        LinkProcessingFactory::new(
            graph_provider(),
            GraphDescriptionFactory::new("ok").unwrap(),
        )
        .prepare(&profile, 8_000, 0)
        .err(),
        Some(LinkPreparationError::Link(LinkError::InvalidConfiguration))
    );

    let factory = LinkProcessingFactory::new(
        live_failing_graph_provider(),
        GraphDescriptionFactory::new("ok").unwrap(),
    );
    let mut link = factory.prepare(&profile, 8_000, 160).unwrap();
    assert_eq!(
        link.process_s16(LinkDirection::Read, 8_000, &mut [1]),
        LinkProcessOutcome::GraphFailed
    );
}

#[test]
fn errors_retain_useful_sources() {
    let cases = [
        LinkPreparationError::IncorrectRole,
        LinkPreparationError::InvalidDescription,
        LinkPreparationError::Description(GraphDescriptionError::Formatting),
        LinkPreparationError::Graph(GraphError::AdapterFailure),
        LinkPreparationError::Link(LinkError::InvalidConfiguration),
    ];
    for error in &cases {
        assert!(!error.to_string().is_empty());
    }
    assert!(cases[0].source().is_none());
    assert!(cases[1].source().is_none());
    assert!(cases[2].source().is_some());
    assert!(cases[3].source().is_some());
    assert!(cases[4].source().is_some());
    assert_eq!(
        LinkPreparationError::from(LinkError::InvalidConfiguration),
        LinkPreparationError::Link(LinkError::InvalidConfiguration)
    );
}
