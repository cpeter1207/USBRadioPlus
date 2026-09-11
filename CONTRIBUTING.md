# Contributing

Before an ordinary push, run `make lint static-analysis`. GitHub repeats only
those formatting, lint, and static-analysis checks for pushes. Do not run the
complete platform gate locally solely to prepare a push.

Every pull request must pass the mandatory full quality gate before it can
merge. The gate treats compiler, lint, static-analysis, Doxygen, test,
coverage, packaging, and staged-install failures as errors on Debian 13 for
amd64 and arm64. Production-code line and branch coverage is required on amd64;
arm64 runs the platform test, package, and staged-install checks. Debian 12
builds are aspirational and are run manually only when explicitly requested.
Releases use a merged main revision that has already passed that gate and run
only release-artifact validation.

Every function, structure, enumeration, macro, and externally meaningful data
member must have a concise Doxygen comment. Tests must exercise every reachable
production-code line and branch. Delete unreachable or unused code instead of
excluding it from coverage or suppressing diagnostics.

Run `make docs` with Doxygen 1.9.8 or newer after changing code comments. It checks
the module, tuner, developer tools, shell entry points, and test harnesses. Keep parameter units,
buffer ownership, return values, and locking requirements explicit where they
matter. Python uses docstrings with Doxygen commands; shell helpers use `##`
comments. The generated developer reference starts at
`build/doxygen/html/index.html`. Documentation warnings fail the pull-request
quality gate before platform tests. Generated documentation is published after
a pull request merges.
The negative documentation tests in `tests_docs` run with this platform-independent
check; the platform matrix runs runtime tests and coverage.

Do not duplicate processing supplied by the shared FFmpeg graph. Update tests,
manual pages, examples, and install artifacts whenever an interface changes.

The CI-built images are published in GitHub Container Registry as
`usbradioplus-asl3-debian13` and `usbradioplus-debian13`. Each manifest
contains amd64 and arm64 variants. Clean images use the `clean` tag; installed
images use `edge`, `sha-<commit>`, and release-version tags.
