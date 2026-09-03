#!/usr/bin/env python3
"""Static release checks that do not require Asterisk or radio hardware."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def validate(root=ROOT):
    """Return release-artifact validation errors found below *root*."""
    errors = []

    def require(path, text):
        artifact = root / path
        if not artifact.exists():
            return
        body = artifact.read_text(encoding="utf-8")
        if text not in body:
            errors.append(f"{path}: missing {text!r}")

    for path in (
        "src/chan_usbradioplus.c",
        "examples/usbradioplus.conf.sample",
        "README.md",
        "Makefile",
        "man/usbradioplus.7",
        "man/usbradioplus.conf.5",
        "man/usbradioplus-processing.conf.5",
        "man/usbradioplus-tune.8",
        "src/usbradioplus-tune.c",
        "COPYING",
        "VERSION",
        "doc/packaging.md",
        "install.sh",
        "scripts/install-build-deps.sh",
    ):
        if not (root / path).exists():
            errors.append(f"missing artifact: {path}")

    installer = (root / "Makefile").read_text(encoding="utf-8")
    for pattern in (
        r"sed\s+-i.*modules\.conf",
        r"sed\s+-i.*rpt\.conf",
        r"systemctl\s+(restart|reload)",
        r"service\s+asterisk",
    ):
        if re.search(pattern, installer):
            errors.append(f"installer may alter runtime state: {pattern}")

    module = "".join(
        (root / path).read_text(encoding="utf-8")
        for path in (
            "src/chan_usbradioplus.c",
            "src/usbradioplus_channel_common.inc",
            "src/usbradioplus_native_tick.inc",
        )
    )
    for option in (root / "tests/data/legacy-options.txt").read_text().splitlines():
        if option and not option.startswith("#") and f'"{option}"' not in module:
            errors.append(f"module does not recognize legacy option {option}")

    require("src/chan_usbradioplus.c", "DUPLEX3_MODE_SOFTWARE")
    require("examples/usbradioplus.conf.sample", "duplex3mode = hardware")
    require("README.md", "replacement for the ASL3 `chan_usbradio` channel driver")
    if (root / "patches/app_rpt-radioplus-duplex.patch").exists():
        errors.append("obsolete app_rpt duplex patch is still shipped")
    return errors


def main():
    """Print validation results and return a process exit status."""
    errors = validate(ROOT)
    if not errors:
        print("Release artifact validation passed.")
        return 0
    print("RELEASE VALIDATION FAILED")
    print("\n".join(f"- {item}" for item in errors))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
