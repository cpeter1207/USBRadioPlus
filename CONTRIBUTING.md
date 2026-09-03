# Contributing

Run `make ci` before submitting a change. It is the same mandatory quality
gate used for pushes, pull requests, and releases. The gate treats compiler,
lint, static-analysis, Doxygen, test, coverage, packaging, and staged-install
failures as errors on Debian 12 and 13 for amd64 and arm64.

Every function, structure, enumeration, macro, and externally meaningful data
member must have a concise Doxygen comment. Tests must exercise every reachable
line and branch. Delete unreachable or unused code instead of excluding it from
coverage or suppressing diagnostics.

Do not duplicate processing supplied by the shared FFmpeg graph. Update tests,
manual pages, examples, and install artifacts whenever an interface changes.

The CI-built images are published in GitHub Container Registry as
`usbradioplus-asl3-debian12`, `usbradioplus-asl3-debian13`,
`usbradioplus-debian12`, and `usbradioplus-debian13`. Each manifest contains
amd64 and arm64 variants. Clean images use the `clean` tag; installed images
use `edge`, `sha-<commit>`, and release-version tags.
