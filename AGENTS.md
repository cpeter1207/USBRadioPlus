# USBRadioPlus development rules

Every code change must keep `make ci` passing. Do not commit, merge, tag, or
release code that fails compilation with warnings treated as errors, Ruff,
ShellCheck, Cppcheck, Clang-Tidy, Doxygen warnings, tests, install checks, or
100% line and branch coverage. Remove dead code instead of suppressing it.

Audio filtering, equalization, dynamics, and limiting are implemented only by
the shared FFmpeg graph. Do not add parallel native implementations.

Run the Debian 12 and 13 test-container matrix for amd64 and arm64. Release
workflows must consume the same required quality gate used by pushes and pull
requests. Update Doxygen comments, tests, manuals, examples, and install
artifacts with every affected interface.

Never deploy to a node or alter its configuration without explicit approval.

