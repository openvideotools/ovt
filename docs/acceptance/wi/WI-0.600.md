# WI-0.600 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, reuse 5.0.2, Python 3.13. Date: 2026-11-04. Spec version:
1.0.0-rc1. Commit: the head of the WI-0.600 pull request.

## Sources of the texts

spdx.org and aomedia.org are not reachable from this machine (HTTP 403). The texts therefore come from:

- **SPDX texts.** The SPDX License List data repository at tag `v3.29.0`, file `text/<id>.txt`, fetched from
  raw.githubusercontent.com/spdx/license-list-data. Each file equals the `licenseText` field of
  `json/details/<id>.json` (step 1 of the WI):

  ```console
  LGPL-2.1-or-later match
  GPL-3.0-or-later match
  LGPL-3.0-or-later match
  BSD-2-Clause match
  BSD-3-Clause match
  CC-BY-SA-4.0 match
  ```

- **`LicenseRef-AOM-Patent-License-1.0.txt`.** The `PATENTS` file of `libaom-3.12.1.tar.gz`, byte for byte.
  The archive was downloaded from storage.googleapis.com/aom-releases and verified against the SHA-256 in
  `third_party/VERSIONS.md`.

## Test 1: `reuse lint` reports compliance

The run is on a clean worktree of this branch, without the untracked `third_party/vcpkg` of the development
checkout:

```console
$ reuse lint
# SUMMARY

* Bad licenses: 0
* Deprecated licenses: 0
* Licenses without file extension: 0
* Missing licenses: 0
* Unused licenses: 0
* Used licenses: BSD-2-Clause, LGPL-2.1-or-later, GPL-3.0-or-later, BSD-3-Clause, CC-BY-SA-4.0, LicenseRef-AOM-Patent-License-1.0, LGPL-3.0-or-later
* Read errors: 0
* Files with copyright information: 104 / 104
* Files with license information: 104 / 104

Congratulations! Your project is compliant with version 3.3 of the REUSE Specification :-)
```

Getting there needs files that use BSD-2-Clause, BSD-3-Clause, LGPL-3.0-or-later and the AOM text, because
REUSE rejects unused licence texts. That is spec question #20 (placeholder plugin `CMakeLists.txt` files and
`plugins/ovt-plugin-aom/PATENTS`). Result: pass.

## Test 2: the AOM text's SHA-256 equals the `PATENTS sha256=` note of the libaom row

```console
$ sha256sum LICENSES/LicenseRef-AOM-Patent-License-1.0.txt
661fb8e504744e95587b556b94a58343448300606a41bea8c7a9b97125696e61  LICENSES/LicenseRef-AOM-Patent-License-1.0.txt
```

The libaom row of `third_party/VERSIONS.md` (WI-0.010, PR #16) records `PATENTS sha256=661fb8e504744e95587b556b94a58343448300606a41bea8c7a9b97125696e61`.

Automated: `tools/tests/test_licence_texts.py::test_aom_patent_text_matches_the_versions_md_pin`. It is
skipped on this branch, which does not contain VERSIONS.md, and runs once #16 is merged too. Result: pass
(hashes equal).
