//! Focused module-lifecycle transaction tests.

use std::ffi::c_void;
use std::mem::{size_of, size_of_val};
use std::ptr;

use super::lifecycle::{
    AsteriskLoaderDescriptor, LOADER_ABI_VERSION, LifecycleCoordinator, LifecycleOperations,
    LoaderProviderManifest, manifest_is_valid,
};
use crate::{URP_AST_CHANNEL_BUSY as BUSY, URP_AST_NOT_READY as NOT_READY, URP_AST_OK as OK};

const FAILED: i32 = -8;

fn providers() -> LoaderProviderManifest {
    let present = ptr::dangling::<u8>().cast::<c_void>();
    LoaderProviderManifest {
        struct_size: size_of::<LoaderProviderManifest>() as u32,
        abi_version: LOADER_ABI_VERSION,
        ffmpeg: present,
        rnnoise: present,
        ring: present,
        radio: present,
        samplerate: present,
        audio: present,
        gpio: present,
    }
}

#[test]
fn provider_manifest_requires_the_complete_loader_abi() {
    let mut manifest = providers();
    assert!(manifest_is_valid(&manifest));

    manifest.struct_size -= 1;
    assert!(!manifest_is_valid(&manifest));
    manifest = providers();
    manifest.abi_version += 1;
    assert!(!manifest_is_valid(&manifest));

    for missing in 0..7 {
        manifest = providers();
        let provider = match missing {
            0 => &mut manifest.ffmpeg,
            1 => &mut manifest.rnnoise,
            2 => &mut manifest.ring,
            3 => &mut manifest.radio,
            4 => &mut manifest.samplerate,
            5 => &mut manifest.audio,
            _ => &mut manifest.gpio,
        };
        *provider = ptr::null();
        assert!(!manifest_is_valid(&manifest));
    }
}

#[test]
fn loader_descriptor_layout_starts_with_size_version_and_capability() {
    let descriptor = AsteriskLoaderDescriptor {
        struct_size: size_of::<AsteriskLoaderDescriptor>() as u32,
        abi_version: LOADER_ABI_VERSION,
        capability: c"usbradioplus.asterisk-loader".as_ptr(),
        load: None,
        reload: None,
        unload: None,
    };

    assert_eq!(descriptor.struct_size as usize, size_of_val(&descriptor));
    assert_eq!(descriptor.abi_version, 4);
    assert!(!descriptor.capability.is_null());
    assert!(descriptor.load.is_none());
    assert!(descriptor.reload.is_none());
    assert!(descriptor.unload.is_none());
}

#[derive(Default)]
struct Fixture {
    events: Vec<&'static str>,
    fail: Option<&'static str>,
    channels_active: bool,
}

impl Fixture {
    fn status(&mut self, event: &'static str) -> i32 {
        self.events.push(event);
        if self.fail == Some(event) { FAILED } else { OK }
    }
}

impl LifecycleOperations for Fixture {
    fn validate_providers(&mut self) -> i32 {
        self.status("validate providers")
    }

    fn create_driver(&mut self) -> i32 {
        self.status("create driver")
    }

    fn destroy_driver(&mut self) {
        self.events.push("destroy driver");
    }

    fn register_channels(&mut self) -> i32 {
        self.status("register channels")
    }

    fn unregister_channels(&mut self) -> i32 {
        self.events.push("unregister channels");
        if self.channels_active { BUSY } else { OK }
    }

    fn register_cli(&mut self) -> i32 {
        self.status("register cli")
    }

    fn unregister_cli(&mut self) {
        self.events.push("unregister cli");
    }

    fn start_links(&mut self) -> i32 {
        self.status("start links")
    }

    fn stop_links(&mut self) {
        self.events.push("stop links");
    }

    fn reload(&mut self) -> i32 {
        self.status("reload")
    }
}

#[test]
fn load_validates_then_constructs_in_dependency_order() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut fixture = Fixture::default();

    assert_eq!(coordinator.load(&mut fixture), OK);
    assert_eq!(
        fixture.events,
        [
            "validate providers",
            "create driver",
            "register channels",
            "register cli",
            "start links",
        ]
    );
}

#[test]
fn repeated_load_is_busy_and_unloaded_unload_is_harmless() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut fixture = Fixture::default();

    assert_eq!(coordinator.unload(&mut fixture), OK);
    assert!(fixture.events.is_empty());
    assert_eq!(coordinator.load(&mut fixture), OK);
    fixture.events.clear();
    assert_eq!(coordinator.load(&mut fixture), BUSY);
    assert!(fixture.events.is_empty());
}

#[test]
fn provider_and_configuration_failures_publish_nothing() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut providers = Fixture {
        fail: Some("validate providers"),
        ..Fixture::default()
    };
    assert_eq!(coordinator.load(&mut providers), FAILED);
    assert_eq!(providers.events, ["validate providers"]);

    let mut configuration = Fixture {
        fail: Some("create driver"),
        ..Fixture::default()
    };
    assert_eq!(coordinator.load(&mut configuration), FAILED);
    assert_eq!(
        configuration.events,
        ["validate providers", "create driver"]
    );

    assert_eq!(coordinator.reload(&mut Fixture::default()), NOT_READY);
}

#[test]
fn failed_load_rolls_back_only_completed_stages_in_reverse_order() {
    for (failure, expected) in [
        (
            "register channels",
            vec![
                "validate providers",
                "create driver",
                "register channels",
                "destroy driver",
            ],
        ),
        (
            "register cli",
            vec![
                "validate providers",
                "create driver",
                "register channels",
                "register cli",
                "unregister channels",
                "destroy driver",
            ],
        ),
        (
            "start links",
            vec![
                "validate providers",
                "create driver",
                "register channels",
                "register cli",
                "start links",
                "unregister cli",
                "unregister channels",
                "destroy driver",
            ],
        ),
    ] {
        let mut coordinator = LifecycleCoordinator::new();
        let mut fixture = Fixture {
            fail: Some(failure),
            ..Fixture::default()
        };

        assert_eq!(coordinator.load(&mut fixture), FAILED);
        assert_eq!(fixture.events, expected);
    }
}

#[test]
fn reload_forwards_only_while_loaded() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut fixture = Fixture::default();

    assert_eq!(coordinator.reload(&mut fixture), NOT_READY);
    assert_eq!(coordinator.load(&mut fixture), OK);
    assert_eq!(coordinator.reload(&mut fixture), OK);
    fixture.fail = Some("reload");
    assert_eq!(coordinator.reload(&mut fixture), FAILED);
    assert_eq!(
        fixture
            .events
            .iter()
            .filter(|event| **event == "reload")
            .count(),
        2
    );
}

#[test]
fn unload_refuses_active_channels_without_partial_teardown() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut fixture = Fixture::default();
    assert_eq!(coordinator.load(&mut fixture), OK);
    fixture.events.clear();
    fixture.channels_active = true;

    assert_eq!(coordinator.unload(&mut fixture), BUSY);
    assert_eq!(fixture.events, ["unregister channels"]);

    fixture.channels_active = false;
    fixture.events.clear();
    assert_eq!(coordinator.reload(&mut fixture), OK);
}

#[test]
fn unload_tears_down_in_reverse_order() {
    let mut coordinator = LifecycleCoordinator::new();
    let mut fixture = Fixture::default();
    assert_eq!(coordinator.load(&mut fixture), OK);
    fixture.events.clear();

    assert_eq!(coordinator.unload(&mut fixture), OK);
    assert_eq!(
        fixture.events,
        [
            "unregister channels",
            "stop links",
            "unregister cli",
            "destroy driver",
        ]
    );
    assert_eq!(coordinator.reload(&mut fixture), NOT_READY);
}
