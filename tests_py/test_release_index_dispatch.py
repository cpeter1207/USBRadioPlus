"""Check the thin publisher's release-event admission."""

import json
import re
from pathlib import Path


def test_shared_release_dispatch_accepts_audio_processing_adapters():
    """Released FFmpeg and RNNoise providers must reach the signed index job."""
    source = (Path(__file__).resolve().parents[1] / ".github/workflows/packages.yml").read_text()
    refresh = source.split("  refresh-after-shared-release:\n", 1)[1]
    allowed = json.loads(re.search(r"contains\(fromJSON\('([^']+)'\)", refresh)[1])
    assert {
        "cpeter1207/rptadv-ffmpeg-adapter",
        "cpeter1207/rptadv-rnnoise-adapter",
    }.issubset(allowed)
    assert "untrusted/repository" not in allowed
