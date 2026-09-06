## @file
## @brief Quality infrastructure regression checks.
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
    assert "pull_request:" in workflow
    assert "workflow_dispatch:" in workflow
    assert f"uses: {WORKFLOW_REF.format('quality.yml')}" in workflow
    assert "contents: write" in workflow
    assert "pages: write" in workflow
    assert "id-token: write" in workflow
    assert "runs-on:" not in workflow
    assert "make " not in workflow


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
    assert "/usr/local/libexec/usbradioplus/container-smoke-test.sh" in dockerfile
    smoke = read("tests/container-smoke-test.sh")
    assert "module load res_usbradio.so" in smoke
    assert "module load chan_usbradioplus.so" in smoke
    assert "wait_for_module chan_usbradioplus" in smoke
    assert "radioplus processing show" in smoke


def test_coverage_gate_requires_python_and_c_line_and_branch_coverage():
    """Verify coverage gate requires python and c line and branch coverage."""
    makefile = read("Makefile")
    assert "pytest -q -n auto" in makefile
    assert "--cov-branch --cov-fail-under=100" in makefile
    assert "--fail-under-line 100 --fail-under-branch 100" in makefile


def test_local_container_runner_cleans_only_labeled_test_containers():
    """Verify local container runner cleans only labeled test containers."""
    runner = read("tests/run-in-quality-container.sh")
    assert "org.usbradioplus.test.scope=" in runner
    assert "cleanup_stale" in runner
    assert "trap cleanup_current EXIT" in runner
    assert "trap 'exit 130' INT" in runner
    assert "trap 'exit 143' TERM" in runner
    assert 'docker run --rm --name "$name" --label "$label"' in runner
