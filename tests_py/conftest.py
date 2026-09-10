"""Shared pytest isolation for the USBRadioPlus tuning utility."""

import subprocess

import pytest
from test_processing_tuner import MODULE


@pytest.fixture(autouse=True)
def isolate_tuner_from_host_asterisk(monkeypatch):
    """Keep configuration tests from contacting an Asterisk instance on the runner."""

    original_run = subprocess.run

    def reject_asterisk_command(args, *extra, **kwargs):
        """Make accidental Asterisk CLI calls behave like an unavailable executable."""
        if isinstance(args, (list, tuple)) and args and args[0] == "asterisk":
            raise FileNotFoundError("Asterisk CLI is unavailable in unit tests")
        return original_run(args, *extra, **kwargs)

    monkeypatch.setitem(MODULE, "OFFLINE", True)
    monkeypatch.setitem(MODULE, "NODE", None)
    monkeypatch.setattr(subprocess, "run", reject_asterisk_command)
