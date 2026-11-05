# WI-0.018 acceptance record

Machine: owner's Mac Mini (self-hosted build host), Apple M4, macOS 15.7.3 arm64, user `ovt-build`.
Python 3.15.2 (Homebrew) in a venv with `tools/requirements.txt` (aqtinstall 3.1.21).
Date: 2026-11-05. Spec version: 1.0-rc1. Script: `tools/install_qt.py` as on `main` at `6730890`.

Automated tests: `tools/tests/test_install_qt.py` (merged with the script in #40) cover the kit plan, the aqt
command line and the skip logic with a fake `qmake`. This record covers the real download, which the cloud
containers cannot do.

## Test 1: a machine without Qt gets 6.8.3 for every kit

`~/Qt/ovt` did not exist before the run.

```console
$ python -I tools/install_qt.py --target desktop --target android --dest ~/Qt/ovt
install_qt: .../python -m aqt install-qt mac desktop 6.8.3 clang_64 -O /Users/ovt-build/Qt/ovt -m qtshadertools qtgraphs qtimageformats
install_qt: .../python -m aqt install-qt mac android 6.8.3 android_arm64_v8a -O /Users/ovt-build/Qt/ovt -m qtshadertools qtgraphs qtimageformats
install_qt: .../python -m aqt install-qt mac android 6.8.3 android_x86_64 -O /Users/ovt-build/Qt/ovt -m qtshadertools qtgraphs qtimageformats
QT_ROOT_DIR=/Users/ovt-build/Qt/ovt/6.8.3
real 1:09.22, exit 0
$ for k in macos android_arm64_v8a android_x86_64; do ~/Qt/ovt/6.8.3/$k/bin/qmake -query QT_VERSION; done
6.8.3
6.8.3
6.8.3
```

Installed size: 5.8 GiB. Result: pass.

## Test 2: a re-run skips every kit in under 5 s

```console
$ python -I tools/install_qt.py --target desktop --target android --dest ~/Qt/ovt
install_qt: macos already at 6.8.3, skipped
install_qt: android_arm64_v8a already at 6.8.3, skipped
install_qt: android_x86_64 already at 6.8.3, skipped
QT_ROOT_DIR=/Users/ovt-build/Qt/ovt/6.8.3
real 0.127 s, exit 0
```

Result: pass.

## Observations

- aqtinstall writes `aqtinstall.log` into the current directory. Running the script from the repository root
  leaves an untracked file there; run it from another directory or add the file to `.gitignore`.
- The `gcc_64` and `gcc_arm64` kits are not covered by this record; they need a Linux host.
