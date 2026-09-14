use super::*;

use std::error::Error;

use usbradioplus_asl3::EchoConfiguration;
use usbradioplus_core::{ConfigDocument, ResolvedChannelConfiguration};

use crate::test_support::{
    failing_sample_rate_adapter, graph_provider, processing_failing_graph_provider,
    sample_rate_adapter, station_providers,
};

fn channel() -> ResolvedChannelConfiguration {
    ResolvedChannelConfiguration::from_document(
        &ConfigDocument::new("[usb]\n"),
        "radio.conf",
        "usb",
    )
    .unwrap()
}

#[test]
fn composes_both_controller_transports_from_the_same_validated_providers() {
    let factory = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let app = factory
        .prepare(
            channel(),
            7,
            ControllerConfiguration::AppRpt {
                handoff_slots: 2,
                echo: EchoConfiguration::default(),
            },
        )
        .unwrap();
    assert_eq!(app.plan.channel(), "usb");
    assert_eq!(app.plan.radio().generation_id, 7);
    assert!(!app.controller.echo_enabled());

    let native = factory
        .prepare(
            channel(),
            8,
            ControllerConfiguration::RptAdvanced { handoff_slots: 2 },
        )
        .unwrap();
    assert_eq!(native.plan.radio().generation_id, 8);
    assert!(!native.controller.echo_enabled());
}

#[test]
fn reports_each_composition_layer_with_its_source() {
    let station = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        0,
    )
    .err()
    .unwrap();

    let conversion_factory = StationFactory::new(
        station_providers(graph_provider(), failing_sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let conversion = conversion_factory
        .prepare(
            channel(),
            1,
            ControllerConfiguration::AppRpt {
                handoff_slots: 2,
                echo: EchoConfiguration::disabled(),
            },
        )
        .err()
        .unwrap();
    conversion_factory
        .prepare(
            channel(),
            3,
            ControllerConfiguration::RptAdvanced { handoff_slots: 2 },
        )
        .unwrap();

    let factory = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    let media = factory
        .prepare(
            channel(),
            2,
            ControllerConfiguration::RptAdvanced { handoff_slots: 0 },
        )
        .err()
        .unwrap();

    assert!(matches!(station, StationFactoryError::Station(_)));
    assert!(matches!(
        conversion,
        StationFactoryError::AppRptConverter(_)
    ));
    assert!(matches!(media, StationFactoryError::Media(_)));
    for error in [&station, &conversion, &media] {
        assert!(!error.to_string().is_empty());
        assert!(error.source().is_some());
    }
}

#[test]
fn propagates_plan_and_processing_failures_before_binding() {
    let mut invalid_plan = StationFactory::new(
        station_providers(graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    invalid_plan.maximum_frame_count = 0;
    assert!(matches!(
        invalid_plan.prepare(
            channel(),
            1,
            ControllerConfiguration::RptAdvanced { handoff_slots: 2 },
        ),
        Err(StationFactoryError::Station(_))
    ));

    let invalid_processing = StationFactory::new(
        station_providers(processing_failing_graph_provider(), sample_rate_adapter()),
        "agc.so",
        960,
    )
    .unwrap();
    assert!(matches!(
        invalid_processing.prepare(
            channel(),
            1,
            ControllerConfiguration::RptAdvanced { handoff_slots: 2 },
        ),
        Err(StationFactoryError::Station(_))
    ));
}
