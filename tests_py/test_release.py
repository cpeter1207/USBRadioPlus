"""Release checks for the Rust implementation and thin Asterisk shim."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    """Read a repository-relative text artifact."""
    return (ROOT / path).read_text(encoding="utf-8")


def test_only_the_asterisk_shim_remains_in_the_c_production_tree():
    """Keep configuration, media, hardware, and control semantics in Rust."""
    sources = sorted(path.relative_to(ROOT).as_posix() for path in (ROOT / "src").rglob("*.*"))
    assert sources == ["src/chan_usbradioplus_shim.c"]
    makefile = read("Makefile")
    assert "CHANNEL_SOURCE := src/chan_usbradioplus_shim.c" in makefile
    assert "MODULE_SOURCES := $(CHANNEL_SOURCE) $(ASTERISK_ADAPTER_HEADER)" in makefile
    assert '#include ".c"' not in read("src/chan_usbradioplus_shim.c")


def test_shim_is_only_the_versioned_rust_lifecycle_loader():
    """Keep every substantive Asterisk host operation in Rust."""
    shim = read("src/chan_usbradioplus_shim.c")
    header = read("rust/asterisk/include/usbradioplus_asterisk.h")
    assert "usbradioplus_asterisk_loader_descriptor" in shim
    assert "struct urp_asterisk_loader_descriptor" in header
    for callback in ("load", "reload", "unload"):
        assert f"urp_ast_loader_{callback}_fn {callback};" in header
    for retired in (
        "ast_channel_tech",
        "ast_audiohook",
        "ast_cli_entry",
        "ast_taskprocessor",
        "urp_ast_operations",
        "urp_ast_channel_status",
        "urp_ast_descriptor",
        "channel_reserve",
        "channel_service",
    ):
        assert retired not in shim
        assert retired not in header


def test_loader_descriptor_version_is_independent_of_provider_abis():
    """Reject incompatible module/adapter pairs before Rust host startup."""
    header = read("rust/asterisk/include/usbradioplus_asterisk.h")
    assert "URP_AST_LOADER_ABI_VERSION" in header
    assert "struct_size" in header
    assert "abi_version" in header
    assert "capability" in header


def test_rust_host_exports_only_the_loader_descriptor():
    """Keep channel, delivery, and control ownership private to the Rust DSO."""
    rust_host = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (ROOT / "rust/asterisk/src").rglob("*.rs")
        if "tests" not in path.parts and not path.stem.endswith("_tests")
    )
    assert rust_host.count("#[unsafe(no_mangle)]") == 1
    assert "fn usbradioplus_asterisk_loader_descriptor" in rust_host


def test_rust_cli_uses_the_generated_asterisk_abi_constants():
    """Keep CLI callbacks aligned with the installed Asterisk public ABI."""
    build = read("rust/asterisk/build.rs")
    cli = read("rust/asterisk/src/host/cli.rs")
    assert "cli_command" in build
    for constant in (
        "CLI_INIT",
        "CLI_GENERATE",
        "RESULT_SUCCESS",
        "RESULT_SHOWUSAGE",
        "RESULT_FAILURE",
    ):
        assert f"ffi::{constant}" in cli
    assert "const CLI_INIT" not in cli
    assert "const CLI_GENERATE" not in cli
    assert "const CLI_FAILURE" not in cli
    assert "const CLI_SHOWUSAGE" not in cli


def test_rust_reload_uses_the_generated_asterisk_allocator():
    """Release text returned by Asterisk through Asterisk's allocator."""
    build = read("rust/asterisk/build.rs")
    reload = read("rust/asterisk/src/host/reload.rs")
    assert "free_ptr" in build
    assert "ffi::ast_free_ptr" in reload
    assert "libc::free" not in reload


def test_build_and_install_use_dynamic_rust_artifacts():
    """Ship the Rust tuner and versioned Rust adapter rather than retired scripts."""
    makefile = read("Makefile")
    assert "ASTERISK_ADAPTER_SONAME := libusbradioplus_asterisk.so.1" in makefile
    for soname in (
        "librate_adjusting_pcm_ring2.so.2",
        "librptadvradio.so.4",
        "librptadv_samplerate_adapter.so.1",
        "librptadv_ffmpeg_adapter.so.1",
        "librptadv_portaudio_alsa_adapter.so.2",
        "librptadv_gpio_adapter.so.1",
        "librptadv_rnnoise_adapter.so.1",
    ):
        assert f"Shared library: [{soname}]" in makefile
    assert "RUST_TUNER := $(CARGO_TARGET_DIR)/release/usbradioplus-tune" in makefile
    assert "$(INSTALL_PROGRAM) $(TUNER) $(DESTDIR)$(sbindir)/usbradioplus-tune" in makefile
    assert not (ROOT / "scripts/usbradioplus-tune").exists()
    control = read("debian/control")
    binary = control.split("\nPackage: usbradioplus\n", maxsplit=1)[1]
    assert "python3" not in binary


def test_bindgen_header_invalidates_the_rust_build_stamp():
    """Rebuild generated Asterisk bindings whenever their wrapper changes."""
    makefile = read("Makefile")
    assert "RUST_BINDGEN_INPUTS := rust/asterisk/wrapper.h" in makefile
    assert "$(RUST_SOURCES) \\\n\t$(RUST_BINDGEN_INPUTS) \\" in makefile


def test_source_archive_boundary_excludes_generated_files():
    """Keep Rust sources, the shim, and release inputs in deterministic archives."""
    makefile = read("Makefile")
    for marker in ("Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "src", "rust"):
        assert marker in makefile
    for suffix in ("*.gcda", "*.gcno", "*.profraw", "*.profdata"):
        assert suffix in makefile


def test_retired_no_op_hardware_selectors_are_not_shipped():
    """Do not expose settings which cannot change the selected hardware."""
    sources = "\n".join(
        read(path)
        for path in (
            "rust/core/src/station_config.rs",
            "rust/station/src/hardware.rs",
            "rust/tune/src/catalog.rs",
            "examples/usbradioplus.conf.sample",
            "man/usbradioplus.conf.5",
        )
    )
    for retired in (
        "hardware_audio_backend",
        "hardware_gpio_backend",
        "hardware_portaudio_input_device_index",
        "hardware_portaudio_output_device_index",
        "ignored_numeric_audio_indexes",
    ):
        assert retired not in sources


def test_core_does_not_ship_an_unused_meter_implementation():
    """Use provider/radio observations rather than a parallel dead meter."""
    assert not (ROOT / "rust/core/src/meter.rs").exists()
    assert "mod meter;" not in read("rust/core/src/lib.rs")


def test_rust_runtime_omits_self_tested_only_state_and_api():
    """Do not retain fields or interfaces that have no production consumer."""
    hardware = read("rust/station/src/hardware.rs")
    stream = read("rust/core/src/native_stream.rs") + read("rust/core/src/lib.rs")
    assert "ignored_output_mask" not in hardware
    for retired in (
        "InvalidFrameCount",
        "pub struct FrameCount",
        "pub fn frame_count",
        "pub const fn sample_rate_hz",
    ):
        assert retired not in stream


def test_inline_rust_tests_are_excluded_from_production_coverage():
    """Keep inline test helpers outside the production coverage denominator."""
    marker = "#[cfg(test)]\n#[cfg_attr(coverage, coverage(off))]\nmod tests {"
    for path in ("rust/ffmpeg/src/lib.rs", "rust/rnnoise/src/lib.rs"):
        assert marker in read(path)


def test_install_lists_every_required_runtime_provider():
    """Name the RNNoise adapter separately from its upstream DSP library."""
    assert "`rptadv_rnnoise_adapter`" in read("INSTALL.md")


def test_realtime_handoff_publication_has_bounded_control_flow():
    """The callback producer must never spin while Asterisk consumes audio."""
    source = read("rust/asl3/src/handoff.rs")
    producer = source.split("impl<T: Copy + Send> Producer<T> {", maxsplit=1)[1]
    producer = producer.split("/// Sole non-real-time Asterisk delivery endpoint.", maxsplit=1)[0]
    assert "compare_exchange_weak" not in producer
    assert "loop {" not in producer
