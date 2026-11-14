# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of packaging/appimage/AppRun and tools/linux/appimage.sh (WI-0.070).

linuxdeploy, the Qt plugin and a built preset are not available in the fast lane, so appimage.sh runs against fake
`cmake` and `linuxdeploy` executables that record what they were given. The real AppImage on three distributions is
the manual record docs/acceptance/wi/WI-0.070.md and the container job of the release workflow.
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
APPRUN = REPO / "packaging" / "appimage" / "AppRun"
SCRIPT = REPO / "tools" / "linux" / "appimage.sh"

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="Linux packaging scripts")


def _exe(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


@pytest.fixture
def appdir(tmp_path: Path) -> Path:
    d = tmp_path / "AppDir"
    env_dump = '#!/bin/sh\necho "$0 $*"\necho "PLUGINS=$OVT_PLUGIN_PATH"\necho "LD=$LD_LIBRARY_PATH"\n'
    _exe(d / "usr/bin/ovt", env_dump)
    _exe(d / "usr/bin/ovt-studio", env_dump)
    return d


def _run_apprun(
    appdir: Path,
    *args: str,
    env: dict[str, str] | None = None,
    argv0: str | None = None,
):
    full = {"PATH": "/usr/bin:/bin", "APPDIR": str(appdir)}
    if argv0:
        full["ARGV0"] = argv0
    full.update(env or {})
    return subprocess.run(
        ["sh", str(APPRUN), *args],
        env=full,
        capture_output=True,
        text=True,
        check=False,
    )


def test_apprun_starts_the_gui_by_default(appdir: Path) -> None:
    r = _run_apprun(appdir, "--self-test")
    assert r.returncode == 0, r.stderr
    assert f"{appdir}/usr/bin/ovt-studio --self-test" in r.stdout


def test_apprun_sets_plugin_path_to_the_bundled_directory(appdir: Path) -> None:
    r = _run_apprun(appdir, "--ovt-cli", "plugins")
    assert f"{appdir}/usr/bin/ovt plugins" in r.stdout
    assert f"PLUGINS={appdir}/usr/lib/ovt/plugins\n" in r.stdout
    assert f"LD={appdir}/usr/lib" in r.stdout


def test_apprun_keeps_a_user_plugin_path_and_appends_ours(appdir: Path) -> None:
    r = _run_apprun(appdir, "--ovt-cli", "plugins", env={"OVT_PLUGIN_PATH": "/home/u/plugins:"})
    assert f"PLUGINS=/home/u/plugins:{appdir}/usr/lib/ovt/plugins\n" in r.stdout


def test_apprun_does_not_duplicate_the_bundled_directory(appdir: Path) -> None:
    ours = f"{appdir}/usr/lib/ovt/plugins"
    r = _run_apprun(appdir, "--ovt-cli", "plugins", env={"OVT_PLUGIN_PATH": f"/x:{ours}"})
    assert f"PLUGINS=/x:{ours}\n" in r.stdout


def test_apprun_acts_as_the_cli_when_invoked_as_ovt(appdir: Path) -> None:
    r = _run_apprun(appdir, "probe", argv0="/usr/local/bin/ovt")
    assert f"{appdir}/usr/bin/ovt probe" in r.stdout


def test_apprun_sources_linuxdeploy_hooks(appdir: Path) -> None:
    (appdir / "apprun-hooks").mkdir()
    (appdir / "apprun-hooks" / "qt.sh").write_text('export QT_PLUGIN_PATH="$APPDIR/usr/plugins"\n')
    _exe(appdir / "usr/bin/ovt-studio", '#!/bin/sh\necho "QTP=$QT_PLUGIN_PATH"\n')
    r = _run_apprun(appdir)
    assert f"QTP={appdir}/usr/plugins" in r.stdout


@pytest.fixture
def fake_tools(tmp_path: Path):
    """A fake build tree, `cmake` and `linuxdeploy`; returns (env, out_dir, log)."""
    log = tmp_path / "linuxdeploy.log"
    bin_dir = tmp_path / "bin"
    # cmake: --build ... is a no-op that creates the notices; --install --prefix P creates the install tree.
    _exe(
        bin_dir / "cmake",
        """#!/bin/sh
case "$1" in
  --build) exit 0 ;;
  --install)
    prefix=$4
    mkdir -p "$prefix/bin" "$prefix/lib/ovt/plugins"
    printf '#!/bin/sh\\n' > "$prefix/bin/ovt"; printf '#!/bin/sh\\n' > "$prefix/bin/ovt-studio"
    chmod +x "$prefix/bin/ovt" "$prefix/bin/ovt-studio"
    : > "$prefix/lib/ovt/plugins/libovt_plugin_aom.so"
    : > "$prefix/lib/ovt/plugins/libovt_plugin_ffmpeg.so"
    : > "$prefix/lib/ovt/plugins/libovt_plugin_de265.so"
    : > "$prefix/lib/ovt/plugins/libovt_plugin_ffmpeg_hevc.so"
    exit 0 ;;
esac
exit 1
""",
    )
    _exe(
        bin_dir / "linuxdeploy",
        f"""#!/bin/sh
{{
  echo "ARGS: $*"
  echo "QMAKE=$QMAKE"; echo "EXTRA_QT_MODULES=$EXTRA_QT_MODULES"; echo "QML_SOURCES_PATHS=$QML_SOURCES_PATHS"
  echo "DEPLOY_PLATFORM_THEMES=$DEPLOY_PLATFORM_THEMES"; echo "LDAI_UPDATE_INFORMATION=$LDAI_UPDATE_INFORMATION"
}} > "{log}"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --appdir) appdir=$2; shift ;;
    --custom-apprun) cp "$2" "$appdir/AppRun" ;;
  esac
  shift
done
: > "$LDAI_OUTPUT"; : > "$LDAI_OUTPUT.zsync"
""",
    )
    qt = tmp_path / "qt"
    _exe(qt / "gcc_64/bin/qmake", "#!/bin/sh\n")
    preset = "linux-x64-release"
    build = REPO / "build" / preset
    created = []
    # Inside a real build tree (CTest's tools_pytest) CMakeCache.txt exists but ovt_notices has not been built, so the
    # two stand-in files are decided separately and the test removes only what it made.
    for name, text in (("CMakeCache.txt", ""), ("THIRD_PARTY_NOTICES.md", "# notices\n")):
        if not (build / name).exists():
            build.mkdir(parents=True, exist_ok=True)
            (build / name).write_text(text)
            created.append(build / name)
    env = {
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "QT_ROOT_DIR": str(qt),
        "OVT_APPIMAGE_VERSION": "9.9.9",
        "HOME": str(tmp_path),
    }
    yield env, tmp_path / "out", log, preset
    for f in created:
        f.unlink()
    if created:
        try:
            build.rmdir()
            build.parent.rmdir()
        except OSError:
            pass


def test_script_runs_linuxdeploy_as_the_spec_says(fake_tools) -> None:
    env, out, log, preset = fake_tools
    r = subprocess.run(
        [str(SCRIPT), preset, "--out", str(out), "--skip-notices-build"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 0, r.stderr
    text = log.read_text()
    assert "--plugin qt --output appimage" in text
    assert "--custom-apprun" in text and "packaging/appimage/AppRun" in text
    assert "EXTRA_QT_MODULES=svg;quickcontrols2;graphs" in text
    assert "DEPLOY_PLATFORM_THEMES=1" in text
    assert (
        "LDAI_UPDATE_INFORMATION=gh-releases-zsync|openvideotools|ovt|latest|OVT_Studio-*-x86_64.AppImage.zsync" in text
    )
    assert "qt/gcc_64/bin/qmake" in text
    assert (out / "OVT_Studio-9.9.9-x86_64.AppImage").exists()
    assert (out / "OVT_Studio-9.9.9-x86_64.AppImage.zsync").exists()
    assert (out / "AppDir/usr/share/doc/ovt/THIRD_PARTY_NOTICES.md").exists()
    assert (out / "AppDir/usr/share/doc/ovt/PATENTS").exists()  # the aom plugin ships, so its patent licence does


def test_script_passes_plugins_to_linuxdeploy(fake_tools) -> None:
    env, out, log, preset = fake_tools
    subprocess.run(
        [str(SCRIPT), preset, "--out", str(out), "--skip-notices-build"],
        env=env,
        check=True,
        capture_output=True,
    )
    assert f"--library {out}/AppDir/usr/lib/ovt/plugins/libovt_plugin_aom.so" in log.read_text()


def test_script_rejects_a_non_linux_preset() -> None:
    r = subprocess.run(
        [str(SCRIPT), "macos-arm64-release"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 1
    assert "not a linux-x64-* or linux-arm64-* preset" in r.stderr


def test_script_requires_qt_root_dir(fake_tools) -> None:
    env, out, _log, preset = fake_tools
    env = {k: v for k, v in env.items() if k != "QT_ROOT_DIR"}
    r = subprocess.run(
        [str(SCRIPT), preset, "--out", str(out)],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 1
    assert "QT_ROOT_DIR" in r.stderr


def test_script_keeps_patent_codec_plugins_out_of_the_image(fake_tools) -> None:
    env, out, log, preset = fake_tools
    r = subprocess.run(
        [str(SCRIPT), preset, "--out", str(out), "--skip-notices-build"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 0, r.stderr
    plugins = out / "AppDir/usr/lib/ovt/plugins"
    assert sorted(p.name for p in plugins.iterdir()) == ["libovt_plugin_aom.so"]
    assert (out / "codec-pack-plugins/libovt_plugin_ffmpeg.so").exists()
    assert (out / "codec-pack-plugins/libovt_plugin_de265.so").exists()
    assert (out / "codec-pack-plugins/libovt_plugin_ffmpeg_hevc.so").exists()
    args = log.read_text()
    assert "ffmpeg" not in args and "de265" not in args


def test_apprun_survives_a_hook_that_reads_an_unset_variable(appdir: Path) -> None:
    # linuxdeploy-plugin-qt's platform-theme hook does `case "${XDG_CURRENT_DESKTOP}"` (unset in xvfb containers).
    (appdir / "apprun-hooks").mkdir()
    (appdir / "apprun-hooks" / "theme.sh").write_text('case "${XDG_CURRENT_DESKTOP}" in *GNOME*) X=1 ;; esac\n')
    r = _run_apprun(appdir, "--self-test")
    assert r.returncode == 0, r.stderr
    assert "ovt-studio --self-test" in r.stdout
