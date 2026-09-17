## @file
## @brief Quality infrastructure regression checks.
import subprocess
from pathlib import Path

## Repository root containing the artifacts under test.
ROOT = Path(__file__).resolve().parents[1]
## Reusable workflow reference required by the code repository's callers.
WORKFLOW_REF = "cpeter1207/USBRadioPlus-Workflows/.github/workflows/{}@main"


def read(path):
    """Read a repository artifact as UTF-8 text.

    @param path Filesystem path to inspect or update.
    """
    return (ROOT / path).read_text(encoding="utf-8")


def test_quality_matrix_covers_every_supported_platform():
    """Verify quality matrix covers every supported platform."""
    workflow = read(".github/workflows/quality.yml")
    assert "push:" in workflow
    assert "pull_request:" in workflow
    assert "workflow_dispatch:" in workflow
    assert f"uses: {WORKFLOW_REF.format('preflight.yml')}" in workflow
    assert f"uses: {WORKFLOW_REF.format('quality.yml')}" in workflow
    assert "contents: read" in workflow
    assert "contents: write" not in workflow
    assert "pages: write" not in workflow
    assert "id-token: write" not in workflow
    assert "if: github.event_name == 'push'" in workflow
    assert "if: github.event_name != 'push'" in workflow
    assert "runs-on:" not in workflow
    assert "make " not in workflow


def test_documentation_publishes_after_main_push_with_its_own_permissions():
    """Keep Pages publication outside the read-only quality caller."""
    workflow = read(".github/workflows/documentation.yml")
    assert "push:" in workflow
    assert "branches: [main]" in workflow
    assert f"uses: {WORKFLOW_REF.format('documentation.yml')}" in workflow
    assert "code_ref: ${{ github.sha }}" in workflow
    assert "contents: write" in workflow
    assert "pages: write" in workflow
    assert "id-token: write" in workflow
    assert "runs-on:" not in workflow


def test_container_workflow_builds_and_publishes_native_multiarch_images():
    """Verify container workflow builds and publishes native multiarch images."""
    workflow = read(".github/workflows/containers.yml")
    assert "pull_request:" in workflow
    assert "workflow_dispatch:" in workflow
    assert f"uses: {WORKFLOW_REF.format('containers.yml')}" in workflow
    assert "publish: ${{ inputs.publish == true }}" in workflow
    assert "quality_only: ${{ inputs.quality_only == true }}" in workflow
    assert "release_version: ${{ inputs.release_version || '' }}" in workflow
    assert "packages: write" in workflow
    assert "runs-on:" not in workflow
    assert "docker/" not in workflow


def test_installed_image_derives_from_clean_image_and_runs_smoke_test():
    """Verify installed image derives from clean image and runs smoke test."""
    dockerfile = read("containers/Dockerfile")
    quality = dockerfile.split("FROM quality AS staged", maxsplit=1)[0]
    assert "COPY . ." not in quality
    assert "COPY scripts/install-rnnoise.sh" in quality
    assert "COPY packaging/rnnoise/debian/patches/" in quality
    assert "FROM asl3-clean AS usbradioplus-installed" in dockerfile
    assert "COPY --from=staged /stage/ /" in dockerfile
    assert "container-smoke-test.sh" in dockerfile
    for soname in (
        "librate_adjusting_pcm_ring2.so.2",
        "librptadvradio.so.4",
        "librptadv_samplerate_adapter.so.1",
        "librptadv_ffmpeg_adapter.so.1",
        "librptadv_portaudio_alsa_adapter.so.2",
        "librptadv_gpio_adapter.so.1",
        "librptadv_rnnoise_adapter.so.1",
    ):
        assert f"grep -Fq '{soname}' /tmp/module-libraries" in dockerfile
    assert "/usr/local/libexec/usbradioplus/container-smoke-test.sh" in dockerfile
    smoke = read("tests/container-smoke-test.sh")
    assert "res_usbradio" not in smoke
    assert "$(NM) -D --undefined-only $@" in read("Makefile")
    assert "ast_radio_" in read("Makefile")
    assert "module load chan_usbradioplus.so" in smoke
    assert "core waitfullybooted" in smoke
    assert "wait_for_module chan_usbradioplus" in smoke
    assert "fail_if_asterisk_exited" in smoke
    assert "require_asterisk_cli 'chan_usbradioplus module readiness'" in smoke
    assert "require_asterisk_cli 'radioplus channel-list CLI check'" in smoke
    assert "Asterisk exited unexpectedly after $phase (status $status)" in smoke
    assert "radioplus channel list" in smoke


def test_container_build_context_excludes_generated_quality_artifacts():
    """Keep stale local quality data out of Docker release-build contexts."""
    ignore = read(".dockerignore")
    for pattern in (
        ".coverage*",
        ".ruff_cache/",
        ".test*/",
        "**/*.gcda",
        "**/*.gcno",
        "**/*.gcov",
        "target",
        "**/*.profraw",
        "**/*.profdata",
    ):
        assert pattern in ignore


def test_coverage_gate_requires_production_line_and_branch_coverage():
    """Require each production language's complete line and branch report."""
    makefile = read("Makefile")
    assert "pytest -q -n auto" in makefile
    assert "--cov=tools --cov-branch --cov-fail-under=100" in makefile
    assert "--fail-under-line 100 --fail-under-branch 100" in makefile
    assert "$(MAKE) rust-coverage" in makefile
    assert "tools/validate_rust_coverage.py" in makefile
    assert "--workspace --all-targets --locked --branch --json" in makefile
    assert "--lcov" in makefile
    assert "coverage.lcov" in makefile
    validator = read("tools/validate_rust_coverage.py")
    assert "missing_lines" in validator
    assert "missing_branches" in validator
    assert "--object-directory $(BUILD_DIR)" in makefile
    # The shared library owns its independent counters and coverage report;
    # the consumer gate must not reach into its installed or staged tree.
    assert "find $(RPCR_SOURCE)/build" not in makefile


def test_complete_local_gate_runs_each_test_suite_once():
    """Keep the complete gate on the deduplicated platform-verification path."""
    makefile = read("Makefile")
    recipe = makefile.split("\nci:\n", maxsplit=1)[1].split("\n\n", maxsplit=1)[0]
    assert "$(MAKE) platform-verify" in recipe
    assert "$(MAKE) check" not in recipe
    assert "$(MAKE) coverage" not in recipe
    assert "$(MAKE) distcheck" not in recipe


def test_local_container_runner_cleans_only_labeled_test_containers():
    """Verify local container runner cleans only labeled test containers."""
    runner = read("tests/run-in-quality-container.sh")
    assert "org.usbradioplus.test.scope=" in runner
    assert "cleanup_stale" in runner
    assert "trap cleanup_current EXIT" in runner
    assert "trap 'exit 130' INT" in runner
    assert "trap 'exit 143' TERM" in runner
    assert 'docker run --rm --name "$name" --label "$label"' in runner
    assert "--label rpt_advanced.test=true" in runner
    assert "MSYS_NO_PATHCONV=1" in runner


def test_rust_agc_retains_dynamic_ladspa_artifact_and_pinned_tools():
    """Keep the owned Rust slice dynamic without introducing a runtime toolchain."""
    makefile = read("Makefile")
    crate = read("rust/rms-agc/Cargo.toml")
    assert 'crate-type = ["cdylib"]' in crate
    assert '"rust/rms-agc"' in read("Cargo.toml")
    assert 'channel = "1.85.0"' in read("rust-toolchain.toml")
    assert "AGC_PLUGIN_SONAME := usbradioplus_agc.so.1" in makefile
    assert "$(CARGO) rustc --release --locked -p usbradioplus_agc" in makefile
    assert "$(CARGO) fmt --all --check" in makefile
    clippy = "$(CARGO) clippy --workspace --all-targets --all-features --locked -- -D warnings"
    assert clippy in makefile
    assert 'RUSTDOCFLAGS="-D warnings"' in makefile
    assert "libstd-|RPATH|RUNPATH" in makefile
    assert "src/txagc/rms_agc_ladspa.c" not in makefile
    dockerfile = read("containers/Dockerfile")
    assert "ARG RUST_NIGHTLY=nightly-2025-02-20" in dockerfile
    assert "ARG CARGO_LLVM_COV_VERSION=0.6.21" in dockerfile
    assert "--component llvm-tools-preview" in dockerfile


def test_rust_asterisk_bindgen_uses_the_configured_public_header_directory():
    """Forward the Make include override to the Rust binding generator."""
    makefile = read("Makefile")
    assert 'USBRADIOPLUS_ASTERISK_INCLUDEDIR="$(ASTERISK_INCLUDEDIR)"' in makefile


def test_local_container_runner_preserves_parallel_running_checks(tmp_path, monkeypatch):
    """Remove stopped tests without killing another invocation or a restarted test."""
    log = tmp_path / "docker.log"
    docker = tmp_path / "docker"
    docker.write_text(
        "#!/bin/sh\n"
        'printf "%s\\n" "$*" >> "$DOCKER_TEST_LOG"\n'
        'if [ "$1 $2" = "container ls" ]; then\n'
        '  case "$*" in\n'
        '    *status=exited*status=dead*) printf "stopped-id\\nrestarted-id\\n" ;;\n'
        '    *) printf "stopped-id\\nrunning-id\\n" ;;\n'
        "  esac\n"
        'elif [ "$1 $2 $3" = "container rm restarted-id" ]; then\n'
        "  exit 1\n"
        "fi\n",
        encoding="utf-8",
    )
    docker.chmod(0o755)
    monkeypatch.setenv("DOCKER_TEST_LOG", str(log))
    monkeypatch.setenv("PATH", f"{tmp_path}:/usr/bin:/bin")
    subprocess.run(
        ["sh", str(ROOT / "tests/run-in-quality-container.sh"), "test-image", "true"],
        check=True,
        capture_output=True,
        text=True,
    )
    commands = log.read_text(encoding="utf-8").splitlines()
    assert any("status=exited" in command and "status=dead" in command for command in commands)
    assert "container rm stopped-id" in commands
    assert "container rm restarted-id" in commands
    assert not any("running-id" in command for command in commands)
    assert not any(
        "--force stopped-id" in command or "--force restarted-id" in command for command in commands
    )
    assert any(command.startswith("run --rm ") for command in commands)
