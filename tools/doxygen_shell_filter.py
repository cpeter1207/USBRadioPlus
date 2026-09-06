#!/usr/bin/env python3
"""@file
Expose documented shell entry points to Doxygen's Python parser.

Shell statements become blank lines so source locations remain useful. Only
explicit double-hash documentation is copied; missing function comments remain
missing and are rejected by the same Doxygen warning policy as C and Python.
"""

import re
import sys
from pathlib import Path


def filter_shell(source):
    """Convert shell documentation and function declarations for Doxygen.

    @param source Complete shell source text.
    @return Parser input containing documentation and shell function names.
    """
    output = []
    for line in source.splitlines():
        declaration = re.match(r"^\s*([A-Za-z_]\w*)\s*\(\s*\)\s*(?:\{|$)", line)
        if line.lstrip().startswith("##"):
            output.append(line.lstrip())
        elif declaration:
            output.append(f"def {declaration.group(1)}(): pass")
        else:
            output.append("")
    return "\n".join(output) + "\n"


def main():
    """Write the requested shell file's parser input to standard output.

    @return None; file and argument errors propagate as a failed filter process.
    """
    print(filter_shell(Path(sys.argv[1]).read_text(encoding="utf-8")), end="")


if __name__ == "__main__":
    main()
