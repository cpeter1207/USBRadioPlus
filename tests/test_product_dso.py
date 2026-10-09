"""Exercise built ELF boundaries without loading Asterisk or touching hardware.

Run after ``make all`` with ``python3 tests/test_product_dso.py``. Set
``URP_BUILD_DIR`` when checking artifacts outside the default build directory.
Product Rust tests cover driver creation and destruction with provider fixtures.
"""

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

BUILD_DIR = Path(os.environ.get("URP_BUILD_DIR", Path(__file__).resolve().parents[1] / "build"))
PRODUCT = "libusbradioplus_product.so.1"
ADAPTER = "libusbradioplus_asterisk.so.1"
MODULE = "chan_usbradioplus.so"


class ProductDsoTests(unittest.TestCase):
    """Catch missing, statically embedded, or Asterisk-dependent product artifacts."""

    def artifact(self, name):
        """Require the actual artifact instead of skipping an incomplete build."""
        path = (BUILD_DIR / name).resolve()
        self.assertTrue(path.is_file(), f"Missing built artifact: {path}")
        return path

    def readelf(self, name, option):
        """Inspect ELF metadata, never implementation or build-script text."""
        result = subprocess.run(
            ["readelf", "--wide", option, str(self.artifact(name))],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_product_loads_and_exports_descriptor_without_asterisk(self):
        """A fresh process must resolve every product import immediately."""
        probe = """
import ctypes
import os
import sys

class DescriptorPrefix(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("capability_name", ctypes.c_char_p),
    ]

library = ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW | os.RTLD_LOCAL)
entry = library.usbradioplus_product_descriptor_v1
entry.argtypes = []
entry.restype = ctypes.POINTER(DescriptorPrefix)
pointer = entry()
assert pointer, "Product descriptor returned NULL"
descriptor = pointer.contents
assert descriptor.struct_size >= ctypes.sizeof(DescriptorPrefix), "Short descriptor"
assert descriptor.abi_version == 1, "Incompatible product ABI"
assert descriptor.capability_name == b"usbradioplus.product1", "Wrong product capability"
"""
        environment = os.environ.copy()
        environment.pop("LD_PRELOAD", None)
        environment.pop("LD_AUDIT", None)
        result = subprocess.run(
            [sys.executable, "-c", probe, str(self.artifact(PRODUCT))],
            capture_output=True,
            text=True,
            check=False,
            env=environment,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_product_has_no_asterisk_imports(self):
        """An embedded Asterisk host must not silently enter the shared product."""
        symbols = self.readelf(PRODUCT, "--dyn-syms")
        imports = []
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[6] == "UND":
                imports.append(fields[7].split("@", 1)[0])
        self.assertEqual([name for name in imports if re.match(r"^_*(?:ast_|ao2_)", name)], [])
        dynamic = self.readelf(PRODUCT, "--dynamic")
        self.assertEqual(re.findall(r"\(SONAME\).*\[(.*?)\]", dynamic), [PRODUCT])
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        self.assertEqual([name for name in needed if "asterisk" in name.lower()], [])

    def test_module_uses_versioned_dynamic_product_and_providers(self):
        """The shipped module must resolve shared implementations through DSOs."""
        needed = set(re.findall(r"\(NEEDED\).*\[(.*?)\]", self.readelf(MODULE, "--dynamic")))
        required = {
            PRODUCT,
            ADAPTER,
            "librate_adjusting_pcm_ring3.so.3",
            "librptadvradio.so.4",
            "librptadv_samplerate_adapter.so.2",
            "librptadv_ffmpeg_adapter.so.1",
            "librptadv_portaudio_alsa_adapter.so.2",
            "librptadv_gpio_adapter.so.1",
            "librptadv_rnnoise_adapter.so.1",
        }
        self.assertFalse(required - needed, f"Missing dynamic dependencies: {required - needed}")
        symbols = self.readelf(ADAPTER, "--dyn-syms")
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[7] == "usbradioplus_product_descriptor_v1":
                self.assertEqual(fields[6], "UND", "Adapter must not embed the product")


if __name__ == "__main__":
    unittest.main(verbosity=2)
