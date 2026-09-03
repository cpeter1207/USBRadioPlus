from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def test_quality_matrix_covers_every_supported_platform():
    workflow = read(".github/workflows/quality.yml")
    for debian, architecture, runner in (
        ("12", "amd64", "ubuntu-24.04"),
        ("12", "arm64", "ubuntu-24.04-arm"),
        ("13", "amd64", "ubuntu-24.04"),
        ("13", "arm64", "ubuntu-24.04-arm"),
    ):
        assert f'debian: "{debian}"\n            arch: {architecture}' in workflow
        assert f"runner: {runner}" in workflow
    assert "make ci" in workflow
    assert "Required quality gate" in workflow


def test_container_workflow_builds_clean_and_installed_multiarch_images():
    workflow = read(".github/workflows/containers.yml")
    assert "debian: ['12', '13']" in workflow
    assert workflow.count("platforms: linux/amd64,linux/arm64") == 2
    assert "ubuntu-24.04-arm" in workflow
    assert "RUN_SMOKE_TEST=1" in workflow
    assert "needs: [verify, build]" in workflow
    assert "target: asl3-clean" in workflow
    assert "target: usbradioplus-installed" in workflow
    assert "usbradioplus-asl3-debian" in workflow
    assert "usbradioplus-debian" in workflow
    assert "sha-${GITHUB_SHA::12}" in workflow
    assert "release_version" in workflow
    assert "Required container gate" in workflow


def test_installed_image_derives_from_clean_image_and_runs_smoke_test():
    dockerfile = read("containers/Dockerfile")
    assert "FROM asl3-clean AS usbradioplus-installed" in dockerfile
    assert "COPY --from=staged /stage/ /" in dockerfile
    assert "container-smoke-test.sh" in dockerfile
    assert "/usr/local/libexec/usbradioplus/container-smoke-test.sh" in dockerfile
    smoke = read("tests/container-smoke-test.sh")
    assert "module load chan_usbradioplus.so" in smoke
    assert "module show like chan_usbradioplus" in smoke
    assert "radioplus processing show" in smoke


def test_coverage_gate_requires_python_and_c_line_and_branch_coverage():
    makefile = read("Makefile")
    assert "coverage run --branch" in makefile
    assert "coverage report --fail-under=100" in makefile
    assert "--fail-under-line 100 --fail-under-branch 100" in makefile
