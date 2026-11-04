"""Exercise built ELF boundaries without loading Asterisk or touching hardware.

Run after ``make all`` with ``python3 tests/test_product_dso.py``. Set
``URP_BUILD_DIR`` when checking artifacts outside the default build directory.
Product Rust tests cover driver creation and destruction with provider fixtures.
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
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


class TunerInstallTests(unittest.TestCase):
    """Run the actual tuner in a root containing no Asterisk or product providers."""

    def test_native_checks_need_only_the_tuner_and_normal_system_libraries(self):
        """Check offline storage and the live local endpoint without a host adapter."""
        binary = (BUILD_DIR / "usbradioplus-tune").resolve()
        self.assertTrue(binary.is_file(), f"Missing built artifact: {binary}")
        dynamic = subprocess.check_output(
            ["readelf", "--wide", "--dynamic", str(binary)], text=True
        )
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        self.assertFalse(
            [
                name
                for name in needed
                if any(word in name for word in ("asterisk", "usbradioplus", "rptadv"))
            ]
        )
        libraries = subprocess.check_output(["ldd", str(binary)], text=True)
        environment = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"}
        with tempfile.TemporaryDirectory(prefix="usbradioplus-tuner-install-") as temporary:
            root = Path(temporary)
            destination = root / "usr/sbin/usbradioplus-tune"
            destination.parent.mkdir(parents=True)
            shutil.copy2(binary, destination)
            for name in set(re.findall(r"(/[^\s()]+)", libraries)):
                library = Path(name)
                destination = root / library.relative_to("/")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(library, destination)
            configuration = root / "etc/rpt_advanced/rpt_advanced.conf"
            configuration.parent.mkdir(parents=True)
            contents = "[1000]\n[radio]\neeprom_enabled=no\n"
            configuration.write_text(contents, encoding="utf-8")
            command = [
                "chroot",
                str(root),
                "/usr/sbin/usbradioplus-tune",
                "--backend",
                "standalone",
                "--check",
            ]
            offline = subprocess.run(
                command + ["--offline"],
                capture_output=True,
                text=True,
                timeout=10,
                env=environment,
                check=False,
            )
            self.assertEqual(offline.returncode, 0, offline.stdout + offline.stderr)
            self.assertIn("configuration storage and prerequisites are valid", offline.stdout)

            whiptail = root / "usr/bin/whiptail"
            whiptail.parent.mkdir(parents=True)
            whiptail.write_text("menu prerequisite fixture\n", encoding="utf-8")
            whiptail.chmod(0o755)
            runtime = root / "run/rpt-advanced"
            runtime.mkdir(parents=True, mode=0o700)
            control_path = runtime / "radio-control.sock"
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(control_path))
                control_path.chmod(0o600)
                listener.listen(1)
                listener.settimeout(5)

                def serve_configuration_path():
                    """Answer only the approved path-discovery request."""
                    with listener.accept()[0] as connection:
                        connection.settimeout(5)
                        stream = connection.makefile("rb")
                        length = int.from_bytes(stream.read(4), "big")
                        self.assertTrue(0 < length <= 65_536)
                        request = json.loads(stream.read(length))
                        self.assertEqual(request, {"version": 1, "operation": "configuration-path"})
                        reply = json.dumps(
                            {
                                "version": 1,
                                "ok": True,
                                "result": "/etc/rpt_advanced/rpt_advanced.conf",
                            }
                        ).encode()
                        connection.sendall(len(reply).to_bytes(4, "big") + reply)

                with ThreadPoolExecutor(max_workers=1) as executor:
                    response = executor.submit(serve_configuration_path)
                    online = subprocess.run(
                        command,
                        capture_output=True,
                        text=True,
                        timeout=10,
                        env=environment,
                        check=False,
                    )
                    response.result(timeout=6)
                self.assertEqual(online.returncode, 0, online.stdout + online.stderr)
                self.assertIn("configuration storage and prerequisites are valid", online.stdout)
            self.assertEqual(configuration.read_text(encoding="utf-8"), contents)
            self.assertFalse((root / "etc/asterisk").exists())
            self.assertFalse((root / "usr/sbin/asterisk").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
