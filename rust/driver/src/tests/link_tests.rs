use super::*;

use std::error::Error;

use usbradioplus_asl3::{LinkDirection, LinkProcessOutcome};

use crate::test_support::{
    failing_graph_create_provider, graph_provider, live_failing_graph_provider,
};

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
