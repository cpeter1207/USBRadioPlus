"""@file
Verify that shell documentation filtering preserves locations and missing docs.
"""

import importlib.util
import runpy
import sys
from pathlib import Path

## Repository containing the documentation filter under test.
ROOT = Path(__file__).resolve().parents[1]
## Import specification for the executable documentation filter.
SPEC = importlib.util.spec_from_file_location(
    "doxygen_shell_filter", ROOT / "tools/doxygen_shell_filter.py"
)
## Loaded filter module; no shell code is executed by the filter.
FILTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FILTER)


def test_filter_preserves_only_explicit_docs_and_shell_declarations():
    """Verify comments, both shell declaration styles, and source line positions."""
    source = (
        "#!/bin/sh\n## @file\n## Installer entry points.\n"
        "# Ordinary comment\necho hidden\n## Explain usage.\n"
        "usage() {\n}\ncleanup ()\n{\n}\n"
    )
    result = FILTER.filter_shell(source).splitlines()
    assert len(result) == len(source.splitlines())
    assert result[1:3] == ["## @file", "## Installer entry points."]
    assert result[3:5] == ["", ""]
    assert result[5:7] == ["## Explain usage.", "def usage(): pass"]
    assert result[8] == "def cleanup(): pass"
    assert "cleanup" not in "\n".join(result[:8])


def test_filter_main_reads_utf8_and_writes_parser_input(tmp_path, monkeypatch, capsys):
    """Exercise the same file-reading entry point invoked by Doxygen.

    @param tmp_path Isolated directory supplied by pytest.
    @param monkeypatch Pytest fixture for temporary process-argument replacement.
    @param capsys Pytest capture of standard output and standard error.
    """
    source = tmp_path / "helper.sh"
    source.write_text("## @file\n## Capture → playback.\nrun() {\n}\n", encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["doxygen_shell_filter.py", str(source)])
    FILTER.main()
    assert capsys.readouterr().out == FILTER.filter_shell(source.read_text(encoding="utf-8"))
    runpy.run_path(str(ROOT / "tools/doxygen_shell_filter.py"), run_name="__main__")
    assert capsys.readouterr().out == FILTER.filter_shell(source.read_text(encoding="utf-8"))
