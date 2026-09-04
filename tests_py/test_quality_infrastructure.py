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
    assert workflow.count("make lint & lint_pid=$!") == 1
    assert workflow.count("make static-analysis & static_pid=$!") == 1
    assert workflow.count("make docs & docs_pid=$!") == 1
    assert 'wait "$lint_pid"' in workflow
    assert 'wait "$static_pid"' in workflow
    assert 'wait "$docs_pid"' in workflow
    assert "needs: quality" in workflow
    assert "make platform-verify" in workflow
    assert "usbradioplus-quality-debian13:latest" in workflow
    assert "usbradioplus-quality-debian${{ matrix.debian }}:latest" in workflow
    assert "Install ASL3 and quality dependencies" not in workflow
    assert "Required quality gate" in workflow


def test_container_workflow_builds_clean_and_installed_multiarch_images():
    workflow = read(".github/workflows/containers.yml")
    assert "debian: ['12', '13']" in workflow
    assert workflow.count("platforms: linux/amd64,linux/arm64") == 3
    assert "ubuntu-24.04-arm" in workflow
    assert "RUN_SMOKE_TEST=1" in workflow
    assert "needs: [verify, build, build-quality, publish-quality]" in workflow
    assert "target: asl3-clean" in workflow
    assert "target: usbradioplus-installed" in workflow
    assert "usbradioplus-asl3-debian" in workflow
    assert "usbradioplus-debian" in workflow
    assert "usbradioplus-quality-debian" in workflow
    assert "target: quality" in workflow
    assert "sha-${GITHUB_SHA::12}" in workflow
    assert "release_version" in workflow
    assert "quality_only" in workflow
    assert "build-quality:" in workflow
    assert "publish-quality:" in workflow
    assert "Build prepared quality image natively" in workflow
    assert "docker buildx imagetools create" in workflow
    assert 'test "$BUILD_QUALITY_RESULT" = success' in workflow
    assert 'test "$PUBLISH_QUALITY_RESULT" = success' in workflow
    assert 'test "$VERIFY_RESULT" = success' in workflow
    assert "Required container gate" in workflow


def test_installed_image_derives_from_clean_image_and_runs_smoke_test():
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
    assert "module show like chan_usbradioplus" in smoke
    assert "radioplus processing show" in smoke


def test_coverage_gate_requires_python_and_c_line_and_branch_coverage():
    makefile = read("Makefile")
    assert "pytest -q -n auto" in makefile
    assert "--cov-branch --cov-fail-under=100" in makefile
    assert "--fail-under-line 100 --fail-under-branch 100" in makefile


def test_local_container_runner_cleans_only_labeled_test_containers():
    runner = read("tests/run-in-quality-container.sh")
    assert "org.usbradioplus.test.scope=" in runner
    assert "cleanup_stale" in runner
    assert "trap cleanup_current EXIT" in runner
    assert "trap 'exit 130' INT" in runner
    assert "trap 'exit 143' TERM" in runner
    assert 'docker run --rm --name "$name" --label "$label"' in runner
