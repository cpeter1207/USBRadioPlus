#!/usr/bin/env python3
"""Static release checks that do not require Asterisk or radio hardware."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []

def require(path, text):
    body = (ROOT / path).read_text(encoding="utf-8")
    if text not in body:
        errors.append(f"{path}: missing {text!r}")

for path in (
    "src/chan_usbradioplus.c", "examples/usbradioplus.conf.sample",
    "README.md", "Makefile", "man/usbradioplus.7",
    "man/usbradioplus.conf.5", "man/usbradioplus-processing.conf.5",
    "man/usbradioplus-tune.8", "src/usbradioplus-tune.c", "COPYING",
    "VERSION", "doc/packaging.md", "install.sh",
    "scripts/install-build-deps.sh"):
    if not (ROOT / path).exists(): errors.append(f"missing artifact: {path}")

installer = (ROOT / "Makefile").read_text(encoding="utf-8")
for pattern in (r"sed\s+-i.*modules\.conf", r"sed\s+-i.*rpt\.conf",
                r"systemctl\s+(restart|reload)", r"service\s+asterisk"):
    if re.search(pattern, installer):
        errors.append(f"installer may alter runtime state: {pattern}")

module = (ROOT / "src/chan_usbradioplus.c").read_text(encoding="utf-8")
for option in (ROOT / "tests/data/legacy-options.txt").read_text().splitlines():
    if option and not option.startswith("#") and f'"{option}"' not in module:
        errors.append(f"module does not recognize legacy option {option}")

require("src/chan_usbradioplus.c", "DUPLEX3_MODE_SOFTWARE")
require("examples/usbradioplus.conf.sample", "duplex3mode = hardware")
require("README.md", "replacement for the ASL3 `chan_usbradio` channel driver")
if (ROOT / "patches/app_rpt-radioplus-duplex.patch").exists():
    errors.append("obsolete app_rpt duplex patch is still shipped")
if errors:
    print("RELEASE VALIDATION FAILED")
    print("\n".join(f"- {item}" for item in errors))
    sys.exit(1)
print("Release artifact validation passed.")
