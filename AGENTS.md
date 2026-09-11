# USBRadioPlus development rules

## Shared rpt_advanced project baseline

This baseline applies to every production, shared-library, and workflow
repository in the rpt_advanced project. Repository-specific rules may add
constraints but must not weaken it.

Run platform-independent formatting, lint, static analysis—including
Cppcheck—and Doxygen once, concurrently where independent. Do not run Cppcheck
in each platform job. Run platform-dependent tests, coverage, build, packaging,
and staged-install checks concurrently across Debian 12 and 13 on native amd64
and arm64. Quality checks must not rewrite source files.

Complete the full quality gate before pushing, opening or updating a pull
request, merging, tagging, or releasing. Local recovery commits may follow
affected targeted checks, but must not be represented as fully verified or used
for a push, pull request, merge, tag, or release until the full gate passes.
Treat compiler warnings as errors and fail applicable formatting, Ruff,
ShellCheck, Cppcheck, Clang-Tidy, Doxygen, tests, installation checks, and 100%
line and branch coverage. Remove unreachable or dead code instead of
suppressing diagnostics or excluding it from coverage.

Update concise Doxygen comments, tests, user documentation, examples, and
build, install, and package artifacts whenever an interface changes. Consumers
of a shared project library must use its released, versioned dynamic shared
object rather than vendor or statically link a duplicate implementation.
Preserve published ABI/API compatibility whenever practical; when a change is
necessary, document its compatibility, SONAME/package consequences, and
migration. Start and clean only project-owned, labeled test containers
deterministically. Never deploy to a node or alter its configuration without
explicit approval.

USBRadioPlus uses `make ci` as its complete local quality gate.

Audio filtering, equalization, dynamics, and limiting are implemented only by
the shared FFmpeg graph. Do not add parallel native implementations.

Run the Debian 12 and 13 test-container matrix for amd64 and arm64. Release
workflows must consume the same required quality gate used by pushes and pull
requests. Update Doxygen comments, tests, manuals, examples, and install
artifacts with every affected interface.

Never deploy to a node or alter its configuration without explicit approval.
