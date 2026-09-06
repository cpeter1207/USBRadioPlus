"""@file
Verify the platform-independent documentation policy rejects real defects.
"""

import subprocess
from pathlib import Path

import pytest

## Repository whose Doxyfile is used without weakening its warning policy.
ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    ("suffix", "source", "expected_diagnostic"),
    [
        (
            ".c",
            "/** @file\n * @brief Minimal complete API. */\n"
            "/** @brief Return the supplied sample.\n"
            " * @param sample Input sample.\n * @return The input sample.\n */\n"
            "int identity(int sample);\n",
            "",
        ),
        (
            ".c",
            "/** @file\n * @brief Missing function documentation. */\n"
            "static int undocumented(int sample);\n",
            "not documented",
        ),
        (
            ".c",
            "/** @file\n * @brief Incorrect parameter documentation. */\n"
            "/** @brief Return a sample.\n * @param wrong Input sample.\n"
            " * @return The sample.\n */\nint identity(int sample);\n",
            "wrong",
        ),
        (
            ".c",
            "/** @file\n * @brief Incomplete parameter documentation. */\n"
            "/** @brief Add samples.\n * @param left First sample.\n"
            " * @return Sum.\n */\nint add(int left, int right);\n",
            "right",
        ),
        (
            ".c",
            "/** @file\n * @brief Undocumented enum value. */\n"
            "/** Radio mode. */\nenum mode { UNDOCUMENTED };\n",
            "UNDOCUMENTED",
        ),
        (
            ".c",
            "/** @file\n * @brief Broken cross-reference.\n * @ref nonexistent_radio_symbol\n */\n",
            "nonexistent_radio_symbol",
        ),
        (
            ".py",
            '"""@file\nUndocumented Python entry point."""\n'
            "def undocumented(value):\n    return value\n",
            "not documented",
        ),
        (
            ".sh",
            "#!/bin/sh\n## @file\n## Undocumented shell entry point.\nundocumented() {\n    :\n}\n",
            "not documented",
        ),
    ],
)
def test_documentation_policy_rejects_defects(tmp_path, suffix, source, expected_diagnostic):
    """Run Doxygen on complete and deliberately broken isolated source files.

    @param tmp_path Isolated fixture and output directory supplied by pytest.
    @param suffix Language suffix used by the fixture.
    @param source Source text containing one documentation scenario.
    @param expected_diagnostic Required error fragment, or empty for a valid fixture.
    """
    fixture = tmp_path / f"fixture{suffix}"
    fixture.write_text(source, encoding="utf-8")
    warnings = tmp_path / "warnings.log"
    configuration = (
        f'@INCLUDE = "{ROOT / "Doxyfile"}"\n'
        f'INPUT = "{fixture}"\n'
        f'OUTPUT_DIRECTORY = "{tmp_path / "output"}"\n'
        f'WARN_LOGFILE = "{warnings}"\n'
        "GENERATE_HTML = YES\nGENERATE_XML = NO\nHAVE_DOT = NO\n"
    )
    result = subprocess.run(
        ["doxygen", "-"],
        cwd=ROOT,
        input=configuration,
        text=True,
        capture_output=True,
        check=False,
    )
    diagnostics = warnings.read_text(encoding="utf-8") + result.stderr
    assert (result.returncode == 0) == (expected_diagnostic == ""), diagnostics
    assert expected_diagnostic in diagnostics
    assert expected_diagnostic or not diagnostics
