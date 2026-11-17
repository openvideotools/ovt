# WI-1.462 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Qt 6.4.2 (distro; no PictureHost, so no libovt-render in
the app), offscreen platform. Date: 2026-11-16. Spec version: 1.0-rc39.

The panels only display what the core produces (WI notes; FR-GUI-101). Two of the producers are not merged yet, so
the acceptance tests run against the HdrMetadata table and DisplayCaps the core would return, and the end-to-end parts
are deferred (spec question #682).

## AT1: `DEC-HDR-0001` ⚠ row

Automated, model and panel halves:

- `ovt_tests_app_models` "HDR metadata rows carry value, provenance and the core's DEC-HDR-0001": an HdrMetadata
  FlatBuffer shaped like `syn_h265_main10_pq_hdr_mismatch` (container `colr` nclc 1/1/1 wins, SPS VUI BT.2020/PQ,
  `box_vs_bitstream_mismatch`, the core's `DEC-HDR-0001`) gives a ⚠ row with the diagnostic verbatim, ⚠ on the colour
  rows (provenance `container colr`) and a "Bitstream colour" row from `SPS VUI #1`.
- `ovt_tests_app_qml` `HdrMetadataModel::test_mismatchShowsWarningRow`: the HDR panel opens on the Metadata tab, the
  tab reads "Metadata ⚠" and the list shows the `DEC-HDR-0001` row with its ⚠ icon.

```console
$ build/linux-x64-debug/app/tests/ovt_tests_app_models "[hdr]"
All tests passed (61 assertions in 5 test cases)
$ QT_QPA_PLATFORM=offscreen build/linux-x64-debug/app/tests/ovt_tests_app_qml -input app/tests/qml/tst_HdrMetadataModel.qml
Totals: 7 passed, 0 failed, 0 skipped, 0 blacklisted, 67ms
```

Deferred (#682): opening the real `syn_h265_main10_pq_hdr_mismatch.mp4` needs `ovt_container_hdr_metadata`
(WI-1.223; a stub returning `OVT_E_UNSUPPORTED` on main) and the container path of `Project` (#243).

## AT2: HDR output toggle on an SDR display

Automated: `ovt_tests_app_qml` `HdrMetadataModel::test_sdrDisplayDisablesHdrOutput` (DisplayCaps with
`hdr_capable = false`, reason "display has no EDR headroom": the Display tab's HDR output switch is disabled and its
tooltip is the reason; an HDR-capable display enables it) and the Catch2 case "an SDR display: not capable, the
reason, SDR status and unknown rows".

## AT3: declared-vs-measured verdict `VQ-HDR-0002`

Deferred (#682). The verdict is computed by the core as `ovt hdr --measure --compare-declared` (WI-3.532) over a
range scan of `measure()`, whose GUI binding is a later work item (FR-GUI-016 note). This WI shows the declared
MaxCLL/MaxFALL on the Measurements tab (`test_sdrContentAndDeclaredValues`); the Scan range button is disabled.
