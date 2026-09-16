#!/usr/bin/env python3
"""Validate the Rust implementation and minimal Asterisk shim release boundary."""

import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

REQUIRED_ARTIFACTS = (
    "Cargo.toml",
    "Cargo.lock",
    "rust-toolchain.toml",
    "rust/asterisk/include/usbradioplus_asterisk.h",
    "rust/asterisk/build.rs",
    "rust/asterisk/src/lib.rs",
    "rust/asterisk/src/host/mod.rs",
    "rust/asterisk/wrapper.h",
    "rust/rms-agc/src/lib.rs",
    "rust/tune/src/main.rs",
    "src/chan_usbradioplus_shim.c",
    "examples/usbradioplus.conf.sample",
    "README.md",
    "CHANGELOG.md",
    "Makefile",
    "man/usbradioplus.7",
    "man/usbradioplus.conf.5",
    "man/usbradioplus-tune.8",
    "COPYING",
    "VERSION",
    "doc/packaging.md",
    "doc/agc.md",
    "doc/native-radio.md",
    "install.sh",
    "scripts/install-build-deps.sh",
)

LOADER_HEADER_MARKERS = (
    "URP_AST_LOADER_ABI_VERSION",
    "struct urp_asterisk_loader_descriptor",
)

LOADER_CALLBACK_MARKERS = {
    "load": ("urp_ast_loader_load_fn load", "(*load)"),
    "reload": ("urp_ast_loader_reload_fn reload", "(*reload)"),
    "unload": ("urp_ast_loader_unload_fn unload", "(*unload)"),
}

RETIRED_SHIM_MARKERS = (
    "ast_channel_tech",
    "ast_audiohook",
    "ast_cli_entry",
    "ast_taskprocessor",
    "urp_ast_operations",
    "urp_ast_descriptor",
    "channel_reserve",
    "channel_service",
)


def validate(root: Path = ROOT) -> list[str]:
    """Return all release-boundary defects found below ``root``."""
    errors = [
        f"missing artifact: {path}" for path in REQUIRED_ARTIFACTS if not (root / path).is_file()
    ]
    if (root / "Cargo.toml").is_file():
        workspace = tomllib.loads((root / "Cargo.toml").read_text(encoding="utf-8"))
        for member in workspace.get("workspace", {}).get("members", []):
            if not (root / member / "Cargo.toml").is_file():
                errors.append(f"missing workspace member: {member}")

    sources = sorted(path.relative_to(root).as_posix() for path in (root / "src").rglob("*.*"))
    if sources != ["src/chan_usbradioplus_shim.c"]:
        errors.append(f"src contains superseded production files: {sources!r}")

    header_path = root / "rust/asterisk/include/usbradioplus_asterisk.h"
    if header_path.is_file():
        header = header_path.read_text(encoding="utf-8")
        for marker in LOADER_HEADER_MARKERS:
            if marker not in header:
                errors.append(f"Asterisk loader header: missing {marker!r}")
        for callback, markers in LOADER_CALLBACK_MARKERS.items():
            if not any(marker in header for marker in markers):
                errors.append(f"Asterisk loader header: missing {callback!r} callback")

    shim_path = root / "src/chan_usbradioplus_shim.c"
    if shim_path.is_file():
        shim = shim_path.read_text(encoding="utf-8")
        if "usbradioplus_asterisk_loader_descriptor" not in shim:
            errors.append("Asterisk shim does not resolve the Rust lifecycle loader")
        for marker in RETIRED_SHIM_MARKERS:
            if marker in shim:
                errors.append(f"Asterisk shim retains substantive host symbol: {marker}")

    if (root / "Makefile").is_file():
        makefile = (root / "Makefile").read_text(encoding="utf-8")
        for marker in (
            "CHANNEL_SOURCE := src/chan_usbradioplus_shim.c",
            "RUST_TUNER := $(CARGO_TARGET_DIR)/release/usbradioplus-tune",
            "ASTERISK_ADAPTER_SONAME := libusbradioplus_asterisk.so.1",
            "Shared library: [librate_adjusting_pcm_ring2.so.2]",
            "Shared library: [librptadvradio.so.3]",
            "Shared library: [librptadv_samplerate_adapter.so.1]",
            "Shared library: [librptadv_ffmpeg_adapter.so.1]",
            "Shared library: [librptadv_portaudio_alsa_adapter.so.2]",
            "Shared library: [librptadv_gpio_adapter.so.1]",
            "Shared library: [librptadv_rnnoise_adapter.so.1]",
        ):
            if marker not in makefile:
                errors.append(f"Makefile: missing {marker!r}")

        for pattern in (
            r"sed\s+-i.*modules\.conf",
            r"sed\s+-i.*rpt\.conf",
            r"systemctl\s+(restart|reload)",
            r"service\s+asterisk",
        ):
            if re.search(pattern, makefile):
                errors.append(f"installer may alter runtime state: {pattern}")

    for retired in ("scripts/usbradioplus-tune", "src/chan_usbradioplus.c"):
        if (root / retired).exists():
            errors.append(f"retired artifact is still shipped: {retired}")
    if (root / "patches/app_rpt-radioplus-duplex.patch").exists():
        errors.append("obsolete app_rpt duplex patch is still shipped")
    return errors


def main() -> int:
    """Print release validation results and return a process status."""
    errors = validate(ROOT)
    if not errors:
        print("Release artifact validation passed.")
        return 0
    print("RELEASE VALIDATION FAILED")
    print("\n".join(f"- {item}" for item in errors))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
