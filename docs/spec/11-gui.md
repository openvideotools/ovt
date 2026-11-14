# 11 — GUI specification (ovt-studio)

> **Scope.** The Qt 6 Quick application `ovt-studio` (`/app/`, GPL-3.0-or-later; ADR-004) and its Android/ChromeOS shell (`/android/`). It specifies the application shell, every panel, the interaction model, keyboard and mouse/touch maps, the colour language, Android adaptations, accessibility, error/progress UX, empty states and the GUI test plan. Everything the GUI shows comes from `libovt-core` FlatBuffers (§0.5) through the C API (§0.6.1) or from `libovt-render` (Chapter 10); the GUI computes nothing codec-specific itself.
>
> **Conventions used here.** "Panel" = a dockable view with a title; "Model" = a C++ `QAbstractItemModel`/`QObject` in `app/src/models/` exposed to QML; "Selection" = the `SelectionModel` singleton (§11.4.1). QML files live in `app/qml/`, panels in `app/qml/panels/<Name>Panel.qml`, reusable controls in `app/qml/controls/`. Qt version: **6.8.3 LTS** (pinned, chapter 14 §14.9.3 / D5; a later Qt 6.x is adopted only by a pin change in chapter 13).

---

## 11.1 Principles and requirements realised by this chapter

**Design principles (informative).** (1) Three-pane skeleton: timeline, picture canvas with overlays, and syntax/statistics of the selection visible together (usability lesson 1, Appendix A.5). (2) The GUI shows only what `libovt-core`/`libovt-render` produce; it never parses or computes codec data itself. (3) Keyboard-first; every action has a shortcut in the single keyboard map of §11.4.3. (4) Desktop idioms behave identically on macOS, Linux and Android/ChromeOS with a keyboard and mouse. (5) Spec terminology in labels, explanations in tooltips. (6) One `SelectionModel` for cursor synchronisation. (7) Nothing blocks the GUI thread.

### 11.1.1 Requirements realised by this chapter

Requirement ids and their normative text live only in chapter 4 (D1). The table copies the chapter 4 sentence verbatim and says where this chapter realises it; where this chapter is more specific than chapter 4 the Note column says so.

| ID | Requirement (chapter 4, verbatim) | Realised in | Note |
|---|---|---|---|
| **FR-GUI-001** | `ovt-studio` MUST implement the three-pane skeleton of UX-1: a timeline region (bottom), a picture region (centre) and a tabbed side region (right) with the panels Container, Syntax, Hex, Selection Info, Statistics, Metrics, HDR, Diagnostics, Reference Graph, Histogram; every panel MUST be dockable, floatable, closable and re-openable from the View menu. | §11.2.2, §11.3 | "Histogram" is the Scopes tab of the HDR panel (§11.3.14) plus the Statistics panel; floating is desktop-only (§11.5.2). |
| **FR-GUI-002** | Panel layout, window geometry, open panels, last overlay mode, colour map and plane mode MUST persist across restarts in the per-user JSON settings file `settings.json` in `QStandardPaths::AppConfigLocation` (`~/.config/ovt/settings.json`, `~/Library/Application Support/ovt/settings.json`, the app files directory on Android; chapter 11 §11.2.5 is the single source, no `QSettings`), and a menu item View → Reset Layout MUST restore the default. | §11.2.5, §11.2.2, `Ctrl+0` | Settings are a plain JSON file, not `QSettings` (D8). |
| **FR-GUI-003** | The timeline MUST draw one bar per frame from `StreamIndex` (height = `size_bytes`, colour by `FrameType` per UX-6), MUST overlay `qp_base` as a line on a secondary axis, MUST support zoom (wheel) and pan (drag) over up to 1 000 000 frames with level-of-detail aggregation (max/mean per pixel column), MUST mark RAPs with a tick, and MUST draw within 16 ms per frame at any zoom level. | §11.3.1, WI-1.455 |  |
| **FR-GUI-004** | Clicking a bar MUST set the current frame; the current frame MUST be a single application-wide property (`AppState.currentFrame` holding a `FrameId`) and every panel MUST bind to it so that a change propagates to all panels within one render frame (UX-7). | §11.4.1 | The property is `Selection.frame` of the `SelectionModel` singleton; `AppState.currentFrame` is its chapter 4 name. |
| **FR-GUI-005** | Decode order and display order MUST be switchable globally by the `O` key and a toolbar toggle (UX-8); in display order the timeline MUST reorder bars by `display_index`, hidden frames (`display_index = −1`) MUST be shown as hatched bars attached to the frame that shows them, and tooltips MUST show `decode #<d> · display #<p> · POC/OrderHint <n> · <type> · <size> B · <pts s>`. | §11.3.1, §11.2.4 |  |
| **FR-GUI-006** | Frame search (`Ctrl+G`) MUST accept decode index (`d123`), display index (`123` or `p123`), POC/order hint (`poc45`), time (`12.5s`, `00:01:30.250`) and file byte offset (`@0x1A2B3C`, `@1715004`) and MUST navigate to the matching frame or report "no frame" without changing the current frame. | §11.2.3 Go to…, WI-1.466 |  |
| **FR-GUI-007** | The picture region MUST support wheel/pinch zoom about the cursor from 1/32× to 64×, primary-button drag pan, `R` fit-to-window, double-click of any mouse button fits, and MUST keep the zoom centre fixed under the cursor to within 1 pixel (UX-12). | §11.3.2, §11.4.4 | §11.3.2 allows 1/32×; double-click (any button) fits. |
| **FR-GUI-008** | Overlay modes MUST be bound to F1–F12 exactly as chapter 11 §11.4.2 lists them (F1 Coding flow, F2 Prediction, F3 Motion vectors, F4 Transform/residual, F5 QP map, F6 Bits heatmap, F7 Reference indices, F8 Loop filter, F9 In-loop filter params, F10 Metrics heatmap, F11 Plain picture, F12 HDR false colour), the letter toggles `P`, `I`, `G`, `M`, `T`, `L`, `H`, `D`, `K` (colour-blind palette), `E`, `S`, `A` (analyse), `B` (bookmark), planes `1`–`5`, `Space`, `←`/`→`, `Shift+←`/`→` (±10), `Ctrl+←`/`→` (RAP), `Tab` (block level) and `Ctrl+↓`/`↑` (next/previous error) MUST follow the canonical keyboard map of chapter 11 §11.4.3 (which chapter 3 UX-2 cites), and every binding MUST be remappable in Settings → Keyboard with conflicts rejected. | §11.4.2, §11.4.3, WI-1.466 | §11.4.3 is the single keyboard map (D8); the F-key order of UX-2 and the meanings of `K`, `A`, `B`, `Shift+←/→` in chapter 3/4 are to be rewritten from §11.4.3. |
| **FR-GUI-009** | Repeated left-clicks at the same canvas position MUST cycle the selection through the block hierarchy largest → smallest and wrap (UX-4): H.264 MB → sub-MB partition; HEVC CTU → CU → PU → TU; AV1 SB → block → TXB; the Selection Info header MUST show the current level and the selection outline MUST be drawn on the picture. | §11.3.9, §11.4.1 |  |
| **FR-GUI-013** | The Syntax panel MUST present the `SyntaxNode` tree with columns Name, Value (dec/hex per `H`/`D`), Descriptor, Derived, and MUST be backed by a lazily-populated `QAbstractItemModel` so that a `StreamIndex` with 1 000 000 units opens without materialising more than the visible rows plus 200. | §11.3.4, WI-1.457 |  |
| **FR-GUI-014** | The Hex panel MUST show the selected unit's raw bytes 16 per row with offset, hex and ASCII columns, MUST toggle to binary with `Ctrl+B`, MUST mark EPB bytes in a distinct colour, MUST highlight the selected `SyntaxNode.span` after EPB mapping, and MUST support search for a hex byte sequence across the whole file (`Ctrl+F`) returning results within 2 s for a 4 GiB file. | §11.3.5, WI-1.458 | Binary ribbon toggle is `Ctrl+B` (`B` is Add bookmark, §11.4.3). |
| **FR-GUI-016** | The HDR panel MUST display the fields of FR-HDR-002 and the live measurements of FR-HDR-006 for the current frame, and the status bar MUST show the display's HDR capability (`SDR` / `HDR headroom 4.0×` / `PQ pass-through`), distinguishing *declared* (container/bitstream metadata) from *measured* (FR-HDR-006) values by label. | §11.3.14, §11.2.4 | Also shows the declared-vs-measured verdict (§11.3.14). Until the HDR scopes/measurement work of phase 4 (WI-4.452) lands, the Measurements tab shows the declared MaxCLL/MaxFALL and the per-frame measurement of FR-HDR-006 only. |
| **FR-GUI-021** | Dual view MUST show two `VideoSurface` instances (horizontal or vertical arrangement) for slots A and B, with frame, zoom, pan, plane mode and overlay mode synchronised by default and individually unlockable, plus the compare modes of FR-PLN-007 in single-surface mode (UX-7; chapter 11 §11.10, realised by WI-2.489). | §11.3.2 (Dual view), WI-2.489 |  |
| **FR-GUI-022** | All operations that may take longer than 50 ms (the GUI budget of chapter 11 §11.7; 100 ms in the C API rule of FR-API-004) MUST run on `jobs::Pool` threads with a progress indicator in the status bar and a cancel button, and the UI thread MUST never block on core calls other than the cheap getters listed in `docs/dev/threading.md`. | §11.7 | This chapter uses the stricter 50 ms threshold. |
| **FR-GUI-024** | Every panel MUST be operable by keyboard alone (Tab order, arrow keys in trees and lists, `Enter` to activate, `Esc` to close floating panels), focus MUST be visibly indicated, and text/background contrast MUST be ≥ 4.5:1 in both light and dark themes. | §11.6, §11.4.3 |  |
| **FR-GUI-027** | `ovt-studio` MUST accept a file path as its first positional argument, MUST accept drag-and-drop of files onto the window, MUST keep a recent-files list of 20 entries, and MUST open a second file in a new window (`Ctrl+N`) or as compare slot B (`Ctrl+Shift+O`). | §11.2.3, §11.8, WI-1.454 |  |
| **FR-GUI-030** | `ovt-studio` MUST render the picture region through `libovt-render`'s `VideoSurface` embedded in a `QQuickRhiItem`/`QSGRenderNode` (ADR-004) using Vulkan (MoltenVK on macOS, Vulkan or GLES on Android, Vulkan on Linux), and MUST NOT draw video or overlays with Qt Quick scene-graph primitives. | §11.2.1, §11.3.2 | Backends per chapter 10 §10.2.2 (Metal on macOS, GLES on Android). |
| **FR-VQ-016** | The Metrics panel MUST plot per-frame scores for every selected metric and plane as line charts over the timeline, MUST overlay frame size from `StreamIndex` on a secondary axis, MUST show a cumulative mean line, and clicking a point MUST seek the picture region to that frame (UX-7). | §11.3.13, WI-3.451 |  |
| **FR-VQ-018** | The GUI MUST support a **comparison project** of up to 16 distorted files against one reference (an encoding ladder of chapter 9 §9.8.4 commonly has 10–14 rungs), run metrics for all of them as one cancellable job set with a progress bar per file, and show RD points (bit rate from `StreamIndex.frames[].size_bytes` and container duration vs pooled metric) in an RD plot with BD-rate between any two selected files. | §11.3.13, WI-3.452 |  |
| **FR-PLN-009** | The **first-mismatch finder** MUST compare slots A and B frame by frame (after alignment) and report the first frame, plane, and (x, y) at which samples differ, with the two values, and MUST run as a cancellable job on a worker thread at ≥ 300 fps for 1080p 8-bit on the M1 reference. | §11.3.13, §11.3.16 (`VQ-DIFF-0001`) |  |
| **FR-AND-008** | The layout MUST adapt by width class exactly as chapter 11 §11.5 defines: ≥ 1200 dp shows the full three-pane skeleton (FR-GUI-001); 840–1199 dp shows the two-pane layout (picture + right tabbed area, compact timeline, left drawer); 600–839 dp shows the picture region with the tabbed panel area below the picture; < 600 dp (phones, unsupported) a single pane with bottom navigation that MUST not crash; the timeline MUST remain visible in every class. | §11.5.2 | §11.5.2 uses a tabbed area below the picture at 600–839 dp rather than bottom sheets. |
| **NFR-002** | The first frame MUST be displayed ≤ 500 ms after the open command for a 1080p stream. | §11.9 `bench_gui_open_first_frame` |  |
| **NFR-008** | Cold start of `ovt-studio` to an interactive empty window MUST take ≤ 1.5 s on the M1 reference and ≤ 3 s on the Pixel Tablet. | §11.9 `bench_gui_cold_start` |  |
| **NFR-023** | All GUI text MUST meet WCAG 2.1 AA contrast (≥ 4.5:1; ≥ 3:1 for ≥ 24 px text) in both themes, all controls MUST be keyboard-reachable, and the Qt Accessibility bridge MUST expose panel names and tree items to VoiceOver (macOS), TalkBack (Android) and Orca (Linux). | §11.6 |  |
| **FR-GUI-101** | Everything the GUI shows MUST be obtainable from core data (`StreamIndex`, `SyntaxTree`, `ContainerTree`, `FrameAnalysis`, `MetricsResult`, `Measurements`); the GUI MUST NOT parse or compute codec data itself. | whole chapter; test: every panel value equals the matching `ovt` CLI JSON (chapter 12) |  |
| **FR-GUI-102** | Specification terminology MUST be used in all labels (`SyntaxNode.name`, `BlockKind` names, `RefSlot.name`, untranslated); human explanations go in tooltips and derived columns (chapter 11 §11.3.4, §11.3.9, §11.6). | §11.3.4, §11.3.9, §11.6 (never translated) |  |

Other FR-GUI ids (010–012, 015, 017–020, 023, 025–026, 028–029, 031–032) and FR-AND-001…007/009 are realised by the chapters named in chapter 4 (containers, rendering, Android shell) or by work items of chapter 16; they are not restated here.

---

## 11.2 Application shell

### 11.2.1 Windows and the Picture host

- One main `ApplicationWindow` per open **project** (several projects = several windows; File ▸ New Window). Minimum size 1024×640 device-independent pixels (dip).
- Secondary windows (desktop only): **HDR monitor window** (a second `ApplicationWindow` holding only a Picture panel, typically dragged to an HDR display), **Scopes window**, detached panels (§11.2.2). On Android there is exactly one window; detachment is disabled.
- The Picture panel hosts the `VideoSurface` via `PictureHost` (`QQuickRhiItem`, Chapter 10 WI-1.405) in EMBEDDED mode and, in NATIVE mode, creates the platform child surface (`QWindow` with `MetalSurface`/`VulkanSurface`, or the Android `SurfaceView` sibling) sized to the item's scene-mapped rect (`mapToGlobal` → window coordinates × `devicePixelRatio`), updated on every geometry change through `onXChanged/onYChanged/onWidthChanged/onHeightChanged` and `Window.contentItem` transforms.
- Qt RHI backend is fixed per platform at startup in `app/src/main.cpp`: macOS Metal, Linux Vulkan, Android OpenGL ES (`QQuickWindow::setGraphicsApi`). Environment override `OVT_RHI=` is honoured for debugging.

### 11.2.2 Workspace and docking model

**Decision:** OVT implements its **own dock manager in QML on top of `SplitView`** (`app/qml/dock/`), not KDDockWidgets.

Why: (1) one code path for desktop and Android — KDDockWidgets' Qt Quick front-end relies on floating top-level windows for drag previews and detached docks, which Android does not support well; (2) the panel set is fixed and small (18 dock panels: §11.3.1–§11.3.17 and the phase-4 Buffer panel §11.3.20; the Jobs popover §11.3.18 and the Welcome page §11.3.19 are not dock panels; FR-GUI-001 names the minimum set), so a general dock tree is unnecessary; (3) layouts must serialise into the `.ovtproj` JSON (§11.2.6) and be restorable deterministically; (4) no additional third-party QML plugin to build for four ABIs/platforms. Rejected: KDDockWidgets 2.x (GPL-2/3 or commercial; excellent on desktop; QtQuick front-end less mature; Android floating windows); pure `TabBar` layouts (no resizing).

Model (`app/src/dock/dock_model.h`):

```cpp
enum class DockArea : uint8_t { Left, Center, Right, Bottom, Floating /* desktop only */ };
struct DockGroup { DockArea area; QStringList panelIds; int currentIndex; qreal sizeFraction; /* of the area's split axis */ };
struct Layout { QString name; QList<DockGroup> groups; bool leftVisible, rightVisible, bottomVisible;
                qreal leftSize, rightSize, bottomSize; /* share of window width / centre-column height, 4 decimals, clamped 0.05–0.9; defaults 0.18 / 0.24 / 0.28 */ };
// Panel ids are the snake_case names of §11.3: timeline, picture, overlay_legend, syntax, hex, units, container, stream_info, selection_info,
// block_samples, ref_graph, statistics, metrics, hdr, display_caps, diagnostics, bookmarks (the phase-4 Buffer panel adds `buffer`).
// They are a file format (`.ovtproj` layout.groups, layouts.json); unknown ids in a loaded layout are dropped.
```

Rules:
- Areas: Left and Right are vertical stacks of tab groups; Bottom is a horizontal stack; Center holds exactly the Picture panel group (Picture may share its group with other panels as tabs, e.g. Ref graph).
- Panels are tabs inside groups. Drag a tab header to: another group (insert as tab), an area edge (new group), or the window edge (desktop: float as a new `Window`). Drop targets highlight with `#39CCCC` 40 %.
- Splitters: `SplitView` handles 6 dip wide (12 dip touch hit area on Android); double-click a handle resets to the layout default.
- Keyboard: `Ctrl+Shift+[1-9]` focuses panel N in the current layout's tab order; `Ctrl+Tab`/`Ctrl+Shift+Tab` cycles tabs in the focused group; `Ctrl+Shift+F` toggles "Picture only" (hides Left/Right/Bottom); `Ctrl+Shift+L` toggles the Left area, `Ctrl+Shift+R` Right, `Ctrl+Shift+B` Bottom.
- Named layout presets (View ▸ Layout): **Analyze** (default: Left = Container/Units, Right = Syntax/Selection Info/Block Samples, Bottom = Timeline/Hex), **Syntax** (Left hidden, Right = Syntax wide (`rightSize` 0.36), Bottom = Hex tall (`bottomSize` 0.40)), **Quality** (Right = Metrics/Statistics, Bottom = Timeline with metric series), **HDR** (Right = HDR/Display Capabilities, Bottom = Timeline), **Compare** (Picture in Split mode, Right = Selection Info A/B, Bottom = Timeline). In a preset a slash means one group per panel stacked along the area axis (Analyze: Left 0.6/0.4, Right 0.5/0.25/0.25, Bottom Timeline 0.6 | Hex 0.4); areas a preset does not name keep Analyze's content. The current layout is persisted to `layouts.json` in the settings directory (§11.2.5) as `{"layouts_version": 1, "current": <layout>}` (other keys hold custom layouts and are preserved; FR-GUI-002's "settings.json" means this per-user config directory); projects embed the same layout JSON. View ▸ Reset Layout restores the Analyze preset (FR-GUI-002).
- State preservation: the Layout is saved on every change (debounced 500 ms) and restored on open; panel-internal state (scroll position, expanded nodes, filter text) is kept per panel instance in a `QVariantMap` `panelState` persisted with the project.

### 11.2.3 Menus

| Menu | Items (shortcut) |
|---|---|
| **File** | Open… (Ctrl+O) · Open Reference… (Ctrl+Shift+O, sets role `reference`) · Open Recent ▸ · New Window (Ctrl+N) · Save Project (Ctrl+S) · Save Project As… · Close File (Ctrl+W) · Export ▸ {Frame as PNG (SDR/HDR)…, Plane as PGM/Y4M…, Statistics CSV…, Metrics CSV/JSON…, Syntax as JSON…, Container as JSON…, Selected Unit bytes…} · Quit (Ctrl+Q; macOS: app menu) |
| **Edit** | Copy (Ctrl+C: context-dependent — selected node as text, hex selection as bytes, block as JSON) · Copy as JSON (Ctrl+Shift+C) · Find… (Ctrl+F, in focused panel) · Go to… (Ctrl+G: frame / time / byte offset / unit index) · Preferences… (Ctrl+,) |
| **View** | Layout ▸ {Analyze, Syntax, Quality, HDR, Compare, Save current…, Manage…} · Panels ▸ (checkable list of all 19) · Picture only (Ctrl+Shift+F) · Dual view (Ctrl+D, checkable; Arrangement ▸ Horizontal/Vertical) · Full screen (F) · Decode order / Display order (O) · Zoom ▸ {In (+), Out (−), Fit (R), 100 % (Ctrl+1), 200 % (Ctrl+2), 400 % (Ctrl+4)} · Plane ▸ {YUV (1), Y (2), U (3), V (4), Gray (5)} · HDR output (Ctrl+H, checkable) · Theme ▸ {System, Light, Dark} |
| **Overlays** | Modes F1–F12 (§11.4.2) · Toggles: Picture (P), Info labels (I), Grid (G), Motion vectors (M), Heatmap (T), Legend (L), Hex/Dec (H / D) · Heatmap colour map ▸ · Colour-blind-safe palette (checkable) |
| **Navigate** | Next/Previous frame (→/←) · ±10 frames (Shift+→/←) · Next/Previous I/Key frame (Ctrl+→/←) · Next/Previous frame of type ▸ {I, P, B, same temporal id} · First/Last (Home/End) · Next/Previous error (Ctrl+↓ / Ctrl+↑) · Next/Previous bookmark (N / Shift+N) · Play/Pause (Space) · Playback rate ▸ |
| **Analysis** | Analyse current frame (A) · Analyse GOP (Shift+A) · Analyse range… · Cancel all jobs (Ctrl+.) · Metrics job… (Ctrl+M) · HDR scan range… · Clear cache for file |
| **Compare** | Mode ▸ {Single (C cycles), Side by side, Split vertical, Split horizontal, Overlay, Difference, Flicker} · Swap A/B (X) · Difference gain ▸ {1,2,4,8,16,32} · False colour difference · Lock pan/zoom (checkable, default on) |
| **Help** | Keyboard map (?) · Spec references (opens `docs/spec` HTML) · About (versions of Qt, libplacebo, FFmpeg, libaom, libde265, libvmaf; licences) · Report diagnostics bundle… |

### 11.2.4 Toolbar and status bar

Toolbar (left → right): Open · Open Reference · | · Play/Pause · Step ◁ ▷ · frame spin box (editable; shows `display#/decode#` per order mode) · order toggle (Dec/Disp) · | · Zoom − / Fit / + · plane selector (YUV Y U V Gray segmented control) · | · overlay mode combo (F1–F12 names) · toggles P I G M T L · | · Compare mode combo · | · HDR output toggle (lit when NATIVE active; disabled with tooltip reason when `DisplayCaps.hdr_capable == false`) · | · Jobs indicator (spinner + count; click opens Jobs popover).

Status bar (left → right): file name and track (`clip.mp4 · track 1 (hvc1)`); frame summary (`#1203 disp / #1198 dec · B · 48,211 B · QP 27 · T2 · POC 2396`); cursor read-out (`x 1024 y 512 · Y 723 U 512 V 512 · 1000 nit` or `· 0x2D3`); selection summary (`CU 14 32×32 @ (992,480) · Inter L0 · bits 2,913`); HDR/DV label when applicable (`PQ BT.2020 · MaxCLL 1000` or the Dolby label of Chapter 10 §10.3.7); zoom (`400 %`); memory (`RSS 1.2 GB`); job progress bar (hidden when idle).

### 11.2.5 Preferences (settings file)

**Storage (D8; single source for chapters 4 and 5).** Settings are one plain JSON document written by `app/src/settings.cpp` (not `QSettings`, which has no JSON format) in the settings directory `$XDG_CONFIG_HOME/ovt` (default `~/.config/ovt`) on Linux, `~/Library/Application Support/ovt` on macOS, `<filesDir>/ovt` on Android — resolved as `QStandardPaths::GenericConfigLocation + "/ovt"`, `GenericDataLocation + "/ovt"` and `AppDataLocation + "/ovt"` respectively (`AppConfigLocation` would append the product name and, on macOS, use `~/Library/Preferences`): `<settings dir>/settings.json`. Schema `docs/spec/schemas/settings.schema.json` (JSON Schema 2020-12, `settings_version: 1`); unknown keys are preserved on save; the file is written atomically (temp file + rename) and read once at start-up. The same directory holds `layouts.json`, `keymap.json` and `recent_files.json` (FR-GUI-027: `path`, `codec`, `last_opened`, `pinned`; pinned first; capacity 20; the setting `recent_files_count` is the number shown in the File menu). `Platform` exposes the resolved paths; the `OVT_CONFIG_DIR` environment variable overrides the directory (used by the test harness, chapter 14 §14.10.3).

Groups: General (theme, language, recent files count, confirm on close), Decoding (prefer HW, threads, default analysis plugin per codec, cache directory + size limit), Picture (default zoom, chroma upscale, downscale filter, background colour, sample-value threshold), Overlays (default mode, palette, heatmap LUT, label content), HDR (tone map default, gamut map, clip nits, macOS encoding scRGB/system PQ, Android desired headroom), Metrics (default metrics set, threads, libvmaf model), Keyboard (editable map, `keymap.json`), Privacy (corpora download consent; there is no telemetry and no crash reporter, NFR-021).

### 11.2.6 Project file `.ovtproj`

JSON, UTF-8, schema `docs/spec/schemas/ovtproj.schema.json` (JSON Schema 2020-12):

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://openvideotools.org/schemas/ovtproj-1.json",
  "type": "object", "required": ["ovtproj_version", "files"],
  "properties": {
    "ovtproj_version": { "const": 1 },
    "created": { "type": "string", "format": "date-time" }, "modified": { "type": "string", "format": "date-time" },
    "app_version": { "type": "string" },
    "files": { "type": "array", "minItems": 0, "items": {
      "type": "object", "required": ["id", "path", "role"],
      "properties": {
        "id": { "type": "string", "pattern": "^f[0-9]+$" },
        "path": { "type": "string", "description": "relative to the project file when possible; Android: content:// URI" },
        "role": { "enum": ["single", "reference", "distorted"] },
        "track_index": { "type": "integer", "minimum": 0, "default": 0 },
        "source_hash": { "type": "string", "pattern": "^[0-9a-f]{64}$", "description": "StreamIndex.source_hash, hex" },
        "raw_format": { "type": "object", "description": "only for raw YUV: width, height, chroma_format, bit_depth, fps, color (H.273 codes)" }
      } } },
    "active_file": { "type": "string" },
    "layout": { "type": "object", "properties": { "preset": { "type": "string" }, "groups": { "type": "array" }, "leftVisible": {"type":"boolean"}, "rightVisible": {"type":"boolean"}, "bottomVisible": {"type":"boolean"} } },
    "panel_state": { "type": "object", "additionalProperties": { "type": "object" } },
    "view": { "type": "object", "properties": {
      "order": { "enum": ["decode", "display"] }, "frame": { "type": "object", "properties": { "decode_index": {"type":"integer"}, "display_index": {"type":"integer"} } },
      "overlay_mode": { "type": "integer", "minimum": 1, "maximum": 12 }, "overlay_toggles": { "type": "object" }, "plane_mode": { "type": "integer", "minimum": 0, "maximum": 4 },
      "zoom": { "type": "number" }, "pan": { "type": "array", "items": {"type":"number"}, "minItems": 2, "maxItems": 2 },
      "compare": { "type": "object" }, "tone_map": { "type": "string" }, "hdr_output": { "type": "boolean" } } },
    "bookmarks": { "type": "array", "items": { "type": "object", "required": ["id", "file", "decode_index"],
      "properties": { "id": {"type":"string"}, "file": {"type":"string"}, "decode_index": {"type":"integer"}, "name": {"type":"string"}, "color": {"type":"string", "pattern": "^#[0-9A-Fa-f]{6}$"}, "created": {"type":"string","format":"date-time"} } } },
    "comments": { "type": "array", "items": { "type": "object", "required": ["id", "file", "text", "created"],
      "properties": { "id": {"type":"string"}, "file": {"type":"string"}, "text": {"type":"string"}, "author": {"type":"string"}, "created": {"type":"string","format":"date-time"},
        "anchor": { "type": "object", "description": "exactly one of", "properties": {
          "frame": { "type": "object", "properties": { "decode_index": {"type":"integer"} } },
          "block": { "type": "object", "properties": { "decode_index": {"type":"integer"}, "rect": { "type": "array", "items": {"type":"integer"}, "minItems": 4, "maxItems": 4 } } },
          "bytes": { "type": "object", "properties": { "offset": {"type":"integer"}, "length": {"type":"integer"} } },
          "field": { "type": "object", "properties": { "unit_index": {"type":"integer"}, "path": {"type":"string"} } },
          "box": { "type": "object", "properties": { "path": {"type":"string", "description": "moov/trak[1]/mdia/minf/stbl/stsz" } } } } } } } },
    "metrics_jobs": { "type": "array", "items": { "type": "object", "description": "MetricsJobConfig JSON of Chapter 9 incl. reference/distorted file ids and NormalizationConfig" } }
  }
}
```

Rules: paths are stored relative when under the project directory; on load, missing files raise a per-file error state (§11.7) but the project still opens. `source_hash` mismatch → warning "file changed since project was saved" (cache invalid). Autosave to `<name>.ovtproj.autosave` every 60 s when dirty; offered for recovery on next start.

---

## 11.3 Panels

Common rules for every panel: (a) title bar with name, context menu (Detach / Close / Help), and a **state badge** (none, "loading…" spinner, "partial" yellow, "error" red — §11.7); (b) an **empty state** text when no file is open (§11.8); (c) all tables are `TableView`/`TreeView` over C++ models (virtualised; never `Repeater` over > 200 items); (d) numbers follow the Hex/Dec toggle (`H`/`D`) where marked ⓗ; (e) every row that identifies a frame, unit, byte range, block or field is clickable and writes to `SelectionModel` (§11.4.1); (f) `Ctrl+C` copies the focused row as tab-separated text, `Ctrl+Shift+C` as JSON.

### 11.3.1 Timeline

**Purpose.** Primary navigation (per-frame bar chart as the timeline): one bar per frame; filmstrip; range → bitrate.
**Data source.** `StreamIndex.frames[]` (`type`, `size_bytes`, `qp_base`, `temporal_id`, `is_rap`, `pts`, `declared_refs`), `StreamIndex.display_order[]`; metric series from `MetricsResult.frames[]` (Chapter 9) when present; thumbnails from the playback decoder (`ovt_decoder_*`) rendered at 96 px height into a `QImage` cache (`app/src/models/thumbnail_cache.cpp`, LRU 2000, disk-backed in the cache dir).
**Layout.**

```
┌ Timeline ───────────────────────────────────────────────── series [bits ▾] [QP] [PSNR] [VMAF] [Tid]  order [Dec|Disp] ┐
│ 60k ┤ █                 █                                                                                     │
│ 40k ┤ █     ▆   ▆       █     ▆   ▆          ▂▂▂▂▂▂▂ ← QP line (secondary axis, right)                         │
│ 20k ┤ █ ▃ ▃ ▆ ▃ ▆ ▃ ▃ ▃ █ ▃ ▃ ▆ ▃ ▆ ▃ ▃ ▃ ▆ ...                                                              │
│     ┼─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴ │
│     I         GOP▲                I                                   I     ← GOP markers (RAP ticks)          │
│ ┌───┐┌───┐┌───┐┌───┐┌───┐┌───┐┌───┐┌───┐   ← filmstrip (toggle), ref arrows ⌒ above when enabled               │
│ │ 0 ││ 1 ││ 2 ││ 3 ││ 4 ││ 5 ││ 6 ││ 7 │                                                                      │
│ └───┘└───┘└───┘└───┘└───┘└───┘└───┘└───┘                                                                      │
│ [range: 120–240 · 121 frames · 2.02 s · 3.41 MB · 13.5 Mbit/s · avg QP 28.3]        ◀ ▶ zoom ──●──── [fit]     │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Interactions.** Click bar → `Selection.frame`; drag → range selection (a first..last inclusive *position* in the current x order; shown as a translucent band `#39CCCC` 25 %; status line computes count = last − first + 1, bytes = Σ `size_bytes`, mean `qp_base`, and duration Δpts = (max pts − min pts of the range's frames) + d, where d is the pts distance from the max-pts frame to the next frame in presentation order of the whole stream (for the stream's final frame: the distance from the previous frame), converted with the first frame's time base; bitrate = bytes×8/duration; a one-frame range in a one-frame stream has Δpts = 0 and no bitrate); wheel → zoom about cursor (1.25× per notch), Shift+wheel → pan; double-click → fit; right-click → context menu {Analyse range, Metrics on range, Bookmark frame, Export range CSV, Copy stats}; hover → tooltip (`#disp/#dec · type · bytes · QP · Tid · POC · pts`); keyboard ←/→ when focused. Series combo switches bar heights: **bits** (`size_bytes`, default), **QP** (`qp_base`), **PSNR/VMAF/…** (any `MetricsResult` present, per plane selectable), **temporal id** (stepped). Secondary series may be drawn as a line (checkbox per series). Bars coloured by `FrameType` (§11.4.5); hidden frames (AV1 `shown == false`) hatched (part c of WI-1.455, split per §16.2.6; until then part b draws them at 55 % opacity of the type colour); `show_existing_of` frames drawn as outline-only bars pointing to their source. GOP markers: tick + "I"/"K" label at every `is_rap` (part c of WI-1.455, shared marker layer with the filmstrip). Order toggle switches the x axis between decode and display order (bars re-sort; the selection stays on the same frame).
**Filmstrip.** Thumbnails in the current order under the bars at zoom levels where a frame ≥ 48 px wide; reference arrows (toggle) drawn as arcs from each frame to its `declared_refs` in the current order (L0 orange, L1 purple as §11.4.5; long-term refs dashed); hover highlights a frame's arrows. Until the index carries list membership (`StreamIndex.declared_refs` is a plain list of decode indices with no L0/L1 or long-term information; an additive schema field is a separate, owner-approved WI), timeline arrows from `declared_refs` are drawn as L0, solid (#402); the Reference graph (§11.3.11) colours by list from `FrameAnalysis.refs` slot names when an analysis is loaded.
**States.** Empty (no file); loading (index building: bars appear progressively from the left as the job reports progress, with a grey "indexing…" region); partial (metrics series missing for some frames → gaps); error (index failed → red badge, message, "Open Diagnostics").
**Performance.** Custom `QQuickItem` `TimelineChart` with `QSGGeometryNode` (one node for bars, one for lines, one for markers); LOD: when more than one frame per device pixel, each pixel column shows max (bar) and mean (line) of its frames computed in C++ (`O(n)` prefix tables built once per series); handles 10⁶ frames at 60 fps. Thumbnails decode on a low-priority job; never block the chart.

### 11.3.2 Picture

**Purpose.** The canvas: decoded picture with overlays, zoom/pan, planes, compare, HDR, probe, selection rectangle.
**Data source.** `ovt_decoder_next` → `Picture` (playback path) or `ovt_analysis_plane` (stage planes) → `VideoSurface::set_frame`; overlays from `FrameAnalysis` via `OverlayBuilder`; `Pick`/`Measurements` from the surface.
**Layout.**

```
┌ Picture ─────────────────────────────────────────── [YUV|Y|U|V|Gray] [Stage: Recon ▾] [Mode F2 Prediction ▾] [P][I][G][M][T][L] [HDR] ┐
│                                                                                                                                   │
│         ┌──────────────────────────────── picture with overlays ──────────────────────────┐                                       │
│         │  ┼ probe crosshair       ╔════╗ selected block (white/black outline)             │   ┌ Legend (L) ────────┐             │
│         │                          ║    ║                                                 │   │ ■ Intra  ■ L0  ■ L1 │             │
│         │   ┌ ─ ─ selection rect ─ ┐                                                      │   │ ▨ Skip   → MV       │             │
│         │   └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┘                                                      │   └─────────────────────┘             │
│         └────────────────────────────────────────────────────────────────────────────────┘                                       │
│ A: clip.mp4 #1203 · B: ref.mp4 #1203 · Split 50 % ──●──  · gain ×8      ⓘ Dolby Vision: RPU-reshaped PQ rendering — not Dolby-certified │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Zoom/pan rules.** Zoom levels: fit, then powers of two 1/32 … 64 (ViewState.zoom = device px per luma sample). Wheel zooms about the cursor (the image point under the cursor stays fixed: `pan' = ix − (px − w/2)/zoom'`); `+`/`−` zoom about the viewport centre; `R` fit; `Ctrl+1/2/4` = 100/200/400 %; double-click resets to fit. Drag with left button on empty area (or middle button/Space+drag) pans; pan clamps so ≥ 64 device px of picture stay visible. Pan/zoom are shared between A and B when "Lock pan/zoom" is on (default).
**Plane selector.** `1–5` keys (YUV, Y, U, V, Gray = `plane_mode` 0–4) / segmented control → `ViewState.plane_mode`; Stage combo (visible when a `FrameAnalysis` with planes exists) chooses `PlaneStage` (Reconstruction default; Prediction, Residual (shown signed: mid-grey = 0), PreDeblock, PreSaoOrCdef, PreLoopRestoration) — Residual/Coefficients use a diverging LUT option.
**Compare modes.** Compare combo / `C` cycles Single → Side by side → Split V → Split H → Overlay → Difference → Flicker (requires B: a file with role `reference`/`distorted` or "Picture ▸ Set as B from memory" which snapshots the current frame as B). Split divider draggable; Overlay alpha slider; Difference gain combo and false-colour toggle; the status strip shows region PSNR (`Measurements.region_psnr`).
**Dual view (FR-GUI-021, phase 2).** View ▸ Dual view (`Ctrl+D`; arrangement Horizontal/Vertical) replaces the single Picture with two `VideoSurface` instances hosted by two `PictureHost` items in one panel: A left/top, B right/bottom, each with its own `ViewState` and `OverlayConfig` and its own `FrameAnalysis` overlays (B's analysis is requested from B's stream). A **sync set** {frame, zoom, pan, plane mode, overlay mode} is synchronised by default; each item is unlockable from a padlock row in the panel header (`Platform`-independent; persisted in `panel_state.dual.locks`). While locked, input on either surface writes `ViewState`/`SelectionModel` for both; unlocked properties diverge and the status strip shows both values. B's frame follows `SelectionB` (§11.4.1). The compare modes below remain available in single-surface mode only (Compare and Dual view are mutually exclusive; switching to Dual view sets `CompareMode::Single`). NATIVE HDR mode uses the first surface only; the second surface stays EMBEDDED (SDR badge). Delivered by WI-2.489.
**Overlay toggles.** `P` picture on/off (overlays over a flat `#202020`), `I` labels, `G` grid, `M` motion vectors, `T` heatmap, `L` legend; the Mode combo selects the F1–F12 preset (§11.4.2). Each toggle maps to `OverlayLayer` bits (§11.4.2 table).
**HDR toggle.** `Ctrl+H`/toolbar: requests NATIVE mode (Chapter 10 R-ARCH-2). Disabled with tooltip = `DisplayCaps.reason` when not capable. The tone-map/gamut/clip controls live in the HDR panel (§11.3.14).
**Pixel probe.** Hover shows the read-out in the status bar; `Ctrl+click` (or long-press on touch) **pins** the probe (crosshair at the pixel; `OverlayConfig.probe_x/y`; the Selection Info panel shows the full `Pick` incl. nits/xy; `Esc` unpins). Pinned probes survive panning.
**Selection rectangle.** `Shift+drag` draws a rect (luma samples, snapped to cells); the Statistics panel scopes to it when "Scope: selection" is chosen; `Esc` clears; context menu {Statistics for selection, Metrics for region, Export region as PNG/PGM, Copy rect}.
**Block selection.** Left click → `Selection.block_id` at the cycle level (§11.3.9); repeated click on the same point cycles levels; `Tab`/`Shift+Tab` cycle too; right-click → Details context menu (Open in Block Samples, Show in Syntax, Show in Hex, Copy block JSON, Follow MV to reference frame (seeks to `refs[ref].frame` and pins the probe at the displaced position)); hover → hover outline + tooltip (`CU 32×32 · Inter L0 (−12,+3) · QP 27 · 2,913 b`).
**States.** Empty (drop zone hint); loading (first frame decoding: dimmed previous frame + spinner); **analysis pending** (overlays requested but `FrameAnalysis` missing: picture shown, a hatched placeholder band at the top "Block data not analysed — press A", and a progress ring while the job runs); partial (plugin lacks some fields → affected overlays shown disabled in the Mode combo with "n/a (plugin)"); error (decode error → red badge, last decoded frame kept, message).
**Performance.** All drawing in `libovt-render` (FR-BLK-018, NFR-003). The QML layer only handles input and the legend. Input events are coalesced: at most one `render()` per vsync; `pick()` on every mouse move (cheap). Thumbnail/scrub: while the timeline is being dragged, the Picture shows playback-path frames (HW decoder) and defers analysis until the drag ends (200 ms debounce).

### 11.3.3 Overlay Legend

**Purpose.** Explain the colours/shapes of the active overlay mode; configure heatmap range and LUT.
**Data source.** `ovt_render_legend(OverlayConfig)` (Chapter 10 WI-2.439), `OverlayConfig`.
**Layout.** Vertical list `[swatch] name` (swatch kinds: fill, line, arrow, hatch); below, for heatmaps: LUT combo (viridis, cividis, turbo, jet, hot), range mode (auto / fixed with two spin boxes), gradient bar with min/max/unit labels (e.g. `QP 22 … 40`, `bits 64 … 8192 (log)`), alpha slider.
**Interactions.** Clicking a legend entry toggles that layer; hovering highlights blocks of that class on the Picture (sets a `highlight_class` field in `OverlayConfig` — the fill pass dims other classes to 20 %).
**States.** Hidden when no overlays; shows "Analysis pending" when the mode needs block data that is missing.
**Performance.** Trivial.

### 11.3.4 Syntax tree

**Purpose.** Header-level syntax (parameter sets, slice/frame headers, SEI/metadata OBUs) as a tree with spec names, values, descriptors and bit spans, linked to the Hex view.
**Data source.** `StreamIndex.units[].header` (`SyntaxNode` trees) for the selected unit; `StreamIndex.parameter_sets[]`, `sei[]`; unit bytes via `ovt_stream_unit_bytes`.
**Layout.**

```
┌ Syntax ──────────────────────── [Unit: #412 SLICE_TRAIL_R ▾]  [filter: log2_   ×]  [Expand ▸ Collapse ▾]  [Dec|Hex] [Copy JSON] ┐
│ Name                                   Value        Descr   Bits (off:len)  Derived / enum                 Spec           │
│ ▾ slice_segment_header()                                     0:412                                        H.265 7.3.6.1  │
│     first_slice_segment_in_pic_flag    1            u(1)     0:1                                           H.265 7.4.7.1  │
│     slice_pic_parameter_set_id         0            ue(v)    1:1                                                          │
│   ▾ short_term_ref_pic_set(num)                               17:38                                        H.265 7.3.7    │
│       num_negative_pics                2            ue(v)    17:3                                                         │
│       delta_poc_s0_minus1[0]           0            ue(v)    20:1          DeltaPocS0[0] = -1                            │
│     slice_qp_delta                     3            se(v)    88:5          SliceQpY = 29                                 │
│   ⚠ num_entry_point_offsets            4            ue(v)    102:5         exceeds tiles (Warning SYN-H265-0031)         │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Columns: Name (`SyntaxNode.name` + `[array_index]`), Value ⓗ (`uvalue/ivalue/bvalue/svalue`, bytes as `hex…` truncated, `enum_name` appended), Descriptor, Bits (`span.bit_offset:bit_length`), Derived (`derived` text), Spec (`spec_ref`, click opens the spec reference help). Diagnostics render as ⚠/✖ icons with tooltip and tint the row (§11.4.5).
**Interactions.** Unit combo (synced with Unit list and Selection); filter box filters by name/value substring (background thread over the flattened tree, debounce 150 ms, results swapped in as a new proxy model; matches expanded automatically); Expand all / Collapse all / Expand to level N; click row → `Selection.field_path` + `Selection.bit_range` → Hex highlights the bits (EPB-aware mapping §11.3.5); double-click a VCL unit's row in the unit combo → seek to that frame; right-click → {Copy value, Copy row, Copy subtree JSON, Show in Hex, Go to spec ref, Add comment}; keyboard ←/→ collapse/expand, `*` expand subtree, `Ctrl+F` focus filter.
**States.** Empty; loading (unit parse pending: skeleton rows); partial (parser stopped at an error → tree truncated with a red ✖ node "parse error SYN-… at bit N"); error.
**Performance.** `QAbstractItemModel` over the FlatBuffer (no copying; nodes addressed by pointer + child index), `TreeView` virtualised; SPS trees are small (< 5 k nodes) but AV1 frame headers with large arrays (`loop_filter_ref_deltas`, CDF-free) stay < 50 k nodes; filtering off-thread.

### 11.3.5 Hex view

**Purpose.** Bytes of the stream with bit-level highlighting of the selected field, unit/box boundaries, goto and search.
**Data source.** `ovt_file_read(file, offset, len)` through a windowed `HexModel` (`QAbstractTableModel`; rows = ⌈file size / 16⌉; window of 4096 rows paged from the core on demand); `StreamIndex.units[].file_span/prefix_span/epb_positions`; `ContainerTree` box spans; `Selection.byte_range/bit_range`.
**Layout.**

```
┌ Hex ─────────────── [goto: 0x1A3F40 ⏎]  [find bytes: 00 00 01 ⏎]  [◀ prev] [next ▶]  scope [File|Unit|Box]  [ASCII] ┐
│ Offset     00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F   ASCII                                           │
│ 001A3F30   00 00 00 01 26 01 AF 0C  34 2C 85 71 B2 90 00 03   ....&...4,.q....   ← NAL #412 band (blue tint)  │
│ 001A3F40  ▌00 03 00 03▐ 80 11 A5 F0  44 71 02 93 8E 1F 7B C0   ....... Dq....{.    EPB bytes shown dim + strike │
│            ████▓▓▒▒                                                                 field 4:0 highlighted       │
│ bit ribbon: 1A3F40: 0000 0000 | 0000 0011 | 0000 0000 | 0000 0011 | 1000 0001 | ...   [field bits 17..54 boxed] │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Rows of 16 bytes: offset (8 hex digits, 16 for files > 4 GiB), 16 byte cells grouped 8+8, ASCII column. **Bands**: each unit (`prefix_span` + `file_span`) gets an alternating background tint (`#1E88E5` 12 % / 8 %) with the start code/length prefix in a darker tint; box bands in Container scope use the same scheme with box headers darker. **Emulation-prevention bytes** (`epb_positions`) are drawn dimmed with a strikethrough and excluded from bit arithmetic. **Selected field**: whole bytes solid `#FFD700` 45 %; partially covered bytes striped; the **bit ribbon** (bottom strip, monospace) shows the 8 bytes around the field start in binary with the exact bit range boxed and the field name.
**RBSP→raw mapping (normative algorithm).** For a `SyntaxNode.span` in RBSP bits of unit `u`: `rbsp_byte = bit_offset / 8`; `raw = rbsp_byte`; `for p in u.epb_positions (ascending, raw offsets relative to file_span.offset): if p <= raw: raw += 1 else break`; file offset = `u.file_span.offset + raw`; bit within byte unchanged (`bit_offset % 8`). The end bit is mapped the same way; EPBs inside the range are skipped visually. AV1 (`epb_positions` empty): identity. Container fields: `BoxNode.file_span.offset + field.span.bit_offset / 8`.
**Interactions.** Goto (accepts `0x…`, decimal, `unit:N`, `frame:N`); find bytes (hex string with wildcards `??`; search runs in a job over the mmap, results as a list, `F3`/`Shift+F3` next/prev); click byte → `Selection.byte_range` (1 byte) and reverse lookup: the unit/box containing it is selected and the Syntax tree selects the deepest field covering that bit (binary search over a per-unit flattened span list); drag → byte range; scope combo restricts the view to the current unit or box (offsets stay absolute); right-click → {Copy bytes hex, Copy bytes C array, Copy offset, Export selection…, Add comment}; `Ctrl+B` toggles the binary ribbon on/off (`B` is the global Add bookmark, §11.4.3).
**States.** Empty; loading (window page pending → grey cells); error (I/O).
**Performance.** `TableView` with `reuseItems`; cells are `Text` delegates (≤ 50 rows × 33 cells visible); paging in 64 KiB blocks; band lookup via binary search over unit spans; no full-file loads (multi-GB files OK).

### 11.3.6 Unit list

**Purpose.** Table of NAL units / OBUs / raw samples in decode order.
**Data source.** `StreamIndex.units[]`, `frames[]`.
**Layout.** Columns: # · Type (`type_name`, tinted by class: VCL grey, parameter set blue, SEI/metadata green, AUD/filler light) · L/T (`layer_id`/`temporal_id`) · Offset ⓗ · Size ⓗ · Frame (`decode#/display#`, blank for non-VCL) · Sample (`sample_index`) · Flags (RAP, ⚠ diagnostics count). Filter chips: VCL / non-VCL / SEI / errors only / type text.
**Interactions.** Click → `Selection.unit_index` (+ frame if VCL); double-click a VCL unit → seek frame; right-click → {Show in Hex, Show syntax, Export unit bytes (raw / RBSP), Copy row}; `Ctrl+F` filter; sortable columns (stable sort in C++).
**States.** Loading (rows appear incrementally during indexing); error.
**Performance.** `QAbstractTableModel` over the FlatBuffer vector; 10⁶ rows fine; sorting uses an index permutation.

### 11.3.7 Container tree

**Purpose.** Box tree with field table, hex linkage and derived tabs.
**Data source.** `ovt_container_tree` (`ContainerTree`, `BoxNode`, `Field`), `ovt_container_expand` for lazy children; derived views from Chapter 8 (`TrackInfo`, `SampleTable`, `EditList`, `FragmentTimeline`, `EncryptionInfo`, `HdrMetadata`).
**Layout.**

```
┌ Container ───────────────────────────────── tabs: [Boxes] [Tracks] [Samples] [Edit list] [Fragments] [Encryption] [HDR] ┐
│ ▾ ftyp            0          32    major_brand isom                      │ Field                 Value ⓗ    Bits   Spec   │
│ ▾ moov            32      48,120                                         │ version               0                    │
│   ▸ mvhd          40         108                                         │ creation_time         3,742,… (2018-08-…)   │
│   ▾ trak[0]       148     23,900   video hvc1 1920×1080                  │ timescale             90000                 │
│     ▸ tkhd                                                               │ duration              5,400,000 = 60.0 s    │
│     ▾ mdia                                                               │ next_track_ID         3                     │
│       ▾ minf › stbl                                                      │                                             │
│         stsz      …                 sample_count 1,500                   │ [hex ▸]  [raw payload 64 B ▸]                │
│ ▸ moof ×120  (fragmented)                                                │                                             │
│ ▸ mdat            48,152   38.2 MB  (not parsed)                         │                                             │
└──────────────────────────────────────────────────────────────────────────┴─────────────────────────────────────────────┘
```

Boxes tab: tree columns fourcc (+`uuid` short form), offset ⓗ, size ⓗ, summary (`description` + key fields chosen by Chapter 8: e.g. `stsd` codec, `tkhd` dimensions), ⚠ count. Right side: Field table for the selected box (`Field.name`, value with `svalue`/derived date formatting, `span` bits relative to the box, `spec_ref`), nested fields as a sub-tree; buttons "hex" (→ Hex at `file_span.offset`), "payload" (→ Hex scope Box for `payload_span`).
Derived tabs: **Tracks** (id, handler, codec fourcc + profile string, dimensions, timescale, duration, sample count, avg bitrate, language, default sample description; click → `Selection.track_index`, "Open as stream" button for non-active video tracks); **Samples** (virtualised table: index, offset ⓗ, size, dts, cts, duration, sync, chunk, description index; click → `Selection.sample_index` → Hex + Unit list filter; row of the current frame highlighted); **Edit list** (segment, media time, duration, rate, derived "skips N samples"); **Fragments** (moof index, sequence number, base decode time, sample count, bytes, offset; click → tree + hex); **Encryption** (scheme (`cenc/cbcs/…`), `tenc` defaults, `pssh` systems with system ID names and base64 data, per-sample `senc` IVs and subsample ranges for the current sample); **HDR summary** (`colr` nclx codes with names, `mdcv`, `clli`, `dvcC/dvvC` profile/level/`bl_signal_compatibility_id`, `av1C`/`hvcC` HDR-relevant fields; row "Provenance: container" — the HDR panel merges this with SEI/OBU data).
**Interactions.** Expand lazily (`ovt_container_expand` on first expand; spinner on the node while pending); click box → `Selection.byte_range = file_span`, field click → bit range; search box (fourcc or field name); right-click → {Copy JSON, Export box bytes, Show in Hex, Validate (runs Chapter 8 checks, results to Diagnostics)}.
**States.** Loading (root boxes appear as the sniff completes); partial (box undecoded → "fields: not decoded (unknown box)", payload hex available); error (truncated box → ✖ with `Diagnostic`).
**Performance.** Lazy children, `mdat` never read; sample table windowed over `SampleTable` arrays (10⁶ samples OK).

### 11.3.8 Stream Info

**Purpose.** One-page summary of the open stream and its container.
**Data source.** `StreamIndex` (frames, parameter sets), `ContainerTree.tracks`, `ovt_ColorDescription` of the stream, `HdrMetadata`.
**Layout.** Two-column key/value groups: **Stream** (codec, profile/tier/level with spec names e.g. `Main 10 @ L5.1 Main tier`, chroma format, bit depth, coded size, crop size, frame rate (from container or VUI/timing info; both shown if they differ), frame count by type `I 60 · P 600 · B 840`, hidden frames (AV1), RAP count and mean GOP length, temporal layers count, duration, total bytes, mean/peak bitrate (1 s window), min/mean/max frame bytes, mean `qp_base` by type); **Colour** (H.273 codes with names, range, siting, HDR format label: `HDR10`, `HDR10+`, `HLG`, `Dolby Vision P8.1`, `SDR`); **Container** (format, brands, tracks count, fragmented yes/no, encrypted yes/no, edit list present, duration); **Analysis** (default plugin id/version for this codec, cache status, analysed frames count). A "Copy as text/JSON" button.
**Interactions.** Values that reference something are links (profile → SPS node in Syntax; HDR → HDR panel; bitrate → Timeline).
**States.** Loading (values fill in as index progresses); partial (no VUI → "frame rate: container only").
**Performance.** Computed once per index in C++ (`StreamSummary` struct), updated at index completion.

### 11.3.9 Selection Info

**Purpose.** Everything about the clicked block, with spec names, at the current level of the block hierarchy; also the pinned pixel probe.
**Data source.** `FrameAnalysis` (`grid` vectors at the cell, `blocks[]` chain from leaf to root, `Block.ext` decoded through FlatBuffers reflection with the schema binary `ovt_model.bfbs` so every extension field appears automatically with its schema name, `mvs[]` filtered by `block_id`, `refs[]`), `Pick`.
**Layout.**

```
┌ Selection Info ──────────────────────────────────────────────── level: [CTU ▸ CU ▸ PU ▸ TU]  (Tab cycles)  [Dec|Hex] [Copy JSON] ┐
│ Breadcrumb: CTU 37 (64×64 @ 1024,448) › CU 3 (32×32 @ 1056,480) › PU 0 (32×32)                                               │
│ ── Block (ovt.fb.Block) ──────────────────────  ── Cell grid @ (1060,484) cell (265,121) ────  ── Motion vectors ─────────────  │
│ kind            CU               pred        Inter      qp              27                     list ref      mv (1/4)  mvd  pred │
│ rect            1056,480 32×32   skip        false      intra_mode_y    —                      L0  L0[0]=#1198 −12,+3   −2,0 amvp[0]│
│ partition_type  PART_2Nx2N       merge       false      mv0             −48,+12 (1/16)         L1  L1[0]=#1210 +4,−1   0,0  merge[2]│
│ bits_x8         23,304 (2,913 b) tx_log2     3×3        ref0/ref1       0 / 1                  flags: bi-pred                   │
│ ── HevcCu (ext) ──────────────────────────────────────────────────────────────────────────────────────────────────────────────── │
│ cu_transquant_bypass_flag 0   cu_skip_flag 0   pred_mode_flag 0   part_mode 0 (PART_2Nx2N)   rqt_root_cbf 1   cu_qp_delta_abs 2 … │
│ ── Pixel probe (pinned) ── x 1060 y 484 · Y 723 U 512 V 512 (0x2D3 0x200 0x200) · RGB lin 4.93 4.93 4.93 · 1000 nit · PQ 0.752 · xy 0.3127 0.3290 │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── ┘
```

**Cycle semantics per codec** (`Tab`, repeated click). The first click at a position selects the **coarsest** level (`level = 0`: MB/CTU/SB); each further click at the same position (± 2 device px) or `Tab` descends one level and wraps to the coarsest after the leaf; `Shift+Tab` ascends. H.264 `MB → SubMB` (partitions; MBs without sub-partitions have one level) `→ (transform 4×4/8×8 as TU level when `AvcMb.transform_size_8x8_flag` present)`; HEVC `CTU → CU → PU → TU`; AV1 `SB → Block → TXB` (no PU level; the "PU" step is skipped). The level persists across clicks on other blocks; the breadcrumb is clickable. The Picture outlines the block at the current level (`OverlayConfig.selected_block`).
**Interactions.** Click MV row → `Selection.mv_index` (arrow highlighted) and "Follow" button seeks to the reference frame with the probe displaced by the MV; click a ref → selects that frame in the Ref graph; right-click field → {Copy value, Show in Syntax (for fields with a `BitSpan` in the ext table, e.g. CU-level syntax when the plugin records spans), Explain (opens spec ref)}.
**States.** "No selection" hint; "Analysis pending" with an Analyse button; partial (field vectors empty → rows "n/a (plugin ovt-plugin-ffmpeg does not export intra modes)").
**Performance.** Reflection decoding of one block ≤ 1 ms; the panel re-renders only on selection change.

### 11.3.10 Block Samples

**Purpose.** Numeric sample grids of the selected block per pipeline stage.
**Data source.** `FrameAnalysis.planes[]` (`PlaneRef` per stage/plane) read via `ovt_analysis_plane(blob_id)`; the selected `Block.rect`; chroma rect = luma rect scaled by the chroma format.
**Layout.** Tabs per stage available (`Prediction`, `Residual`, `Reconstruction`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`, `Coefficients`); inside, plane tabs Y/U/V; a grid of cells (`TableView`, monospace) with row/column headers (sample coordinates), values ⓗ; cell background: grey ramp for samples, diverging blue–white–red for signed stages (Residual, Coefficients) centred at 0; header row shows `mean`, `min`, `max`, `energy (Σx²)`. Buttons: Dec/Hex, "Compare with B" (adds a second grid and a diff grid when a B frame with planes exists), Export CSV, Copy TSV.
**Interactions.** Hover a cell → Picture probe moves to that sample; click → pins probe; wheel scrolls; `Ctrl+wheel` adjusts font size.
**States.** "Select a block"; "Stage not exported by plugin" per tab (disabled tab with tooltip from `ovt_AnalysisCaps.plane_stages`); loading (plane job).
**Performance.** Max block 128×128 (AV1 SB) = 16 k cells → `TableView` virtualised; values read directly from the plane blob with stride arithmetic.

### 11.3.11 Reference / GOP graph

**Purpose.** Decode-vs-display order, reference arrows, DPB contents, temporal layers.
**Data source.** `StreamIndex.frames[]` (`id`, `type`, `poc`, `temporal_id`, `declared_refs`, `dpb_after`, `is_rap`, `shown`), `FrameAnalysis.refs[]` when analysed (actual usage counts `usage_cells`).
**Layout.**

```
┌ Ref graph ────────────────────────────────────── window ±64 frames · rows: [Temporal layers ▾]  [Decode|Display] arrows [declared|used] ┐
│ T3 ·      ·  B  ·  B  ·      ·  B  ·  B  ·                                                                                        │
│ T2 ·   B  ·     ·     ·   B  ·     ·     ·                                                                                        │
│ T1 ·      ·     B     ·      ·     B     ·                                                                                        │
│ T0 I ──────────── P ────────────── P ───────────── I                                                                                │
│    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16   ← x = display order (or decode order), labels POC                            │
│ arrows: ⟶ L0 (orange) ⟶ L1 (purple), dashed = long-term;  thickness ∝ usage_cells when "used"                                       │
│ DPB after #1203: [#1198 L0[0]] [#1210 L1[0]] [#1194 LT] [#1201] [#1202]  (5 / 6 slots)                                              │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Row modes: temporal layers (default), decode-vs-display (two rows with crossing lines), flat.
**Interactions.** Click node → seek; hover → highlight its in/out arrows and tooltip; wheel → scroll window; the current frame is outlined; DPB strip chips are clickable; right-click → {Export graph as SVG, Copy frame list}.
**States.** Loading; partial (`dpb_after` empty → strip reads "DPB: not derivable for this codec/plugin").
**Performance.** Only the ±window around the current frame is laid out (`QSGGeometryNode` for arrows); arrows as cubic Béziers tessellated at 16 segments.

### 11.3.12 Statistics

**Purpose.** Distributions over a frame, a selection rectangle, a GOP or a range: block sizes, prediction modes, intra modes, QP histogram, TX sizes/types, ref usage, MV scatter.
**Data source.** `FrameAnalysis.grid` and `blocks[]` of the scoped frames (aggregated in C++ `StatsAggregator`, runs as a job over cached `FrameAnalysis`; frames not analysed are analysed first, with progress), `qp_hist[]`, `mvs[]`.
**Layout.** Scope selector [Frame | Selection | GOP | Range | Stream (analysed frames)]; chart selector (Block size, Pred kind, Intra mode Y/C, QP histogram, TX size, TX type, Ref index usage, Partition type, MV scatter, MV magnitude histogram, Bits per class); normalisation radio **count / area (cells) / bits**; chart type pie/bar (histograms bar only); the data table beside the chart (class, count, %, area %, bits %); Export CSV / Copy.
**Charts** use **Qt Graphs 2D** (`BarSeries`, `PieSeries`, `ScatterSeries`; GPL — allowed for the GPL GUI); MV scatter plots `mv_x/16` vs `mv_y/16` with density colouring (binned 1-sample cells, viridis).
**Interactions.** Click a bar/slice → Picture highlights blocks of that class (`highlight_class`); hover tooltip with exact values; scope follows Selection.
**States.** Analysis pending (button); partial (fields missing → classes "n/a").
**Performance.** Aggregation O(cells) per frame (518 k at 4K ≈ 2 ms); range scopes run as a cancellable job; charts ≤ 256 classes.

### 11.3.13 Metrics

**Purpose.** Configure and run metric jobs (Chapter 9), browse per-frame results linked to the timeline, pooled values, heatmaps, BD-rate.
**Data source.** `ovt_metrics_*` API (Chapter 9): `MetricDescriptor` list from plugins, `MetricsResult` FlatBuffers; `FrameScore.map_blob_id` for heatmaps; BD-rate from `ovt_bdrate`.
**Layout.**

```
┌ Metrics ──────────────────────────────────────────── [New job… Ctrl+M] [Runs ▾ run-3: vmaf,psnr,ssim vs ref.mp4] [Export CSV/JSON] ┐
│ ── Job setup dialog (modal) ──────────────────────────────────────────────────────────────────────────────────────────────────── │
│  Reference: [ref.mp4 (role reference) ▾] [Browse…]   Distorted: [clip.mp4 ▾]      Range: [all | timeline range | 0–600]            │
│  Metrics:  [x] PSNR (Y,U,V,YUV)  [x] SSIM  [ ] SSIM (x264)  [x] VMAF v0.6.1 [model ▾]  [ ] VMAF-NEG  [ ] MS-SSIM  [ ] CAMBI  [ ] UVQ │
│  Normalisation: alignment [auto ▾] offset [0]  crop [auto ▾]  scale [reference size · bicubic ▾]  colour [to reference ▾]           │
│                 bit depth [promote lower ▾]   range [as signalled ▾]   HDR domain [per metric default ▾]   threads [8]              │
│  [Run]  estimated 2 min 10 s (1080p, 1500 frames)                                                                                  │
│ ── Results ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────── │
│  plot (per frame, shared x with Timeline) ──────●──── click = seek                [series: vmaf ▾ psnr_y ▾]  [heatmap overlay T]  │
│  table: frame | vmaf | psnr_y | psnr_u | psnr_v | ssim                                              pooled: mean 93.1 · harm 92.4 │
│  BD-rate tab: anchor [set A ▾] vs [set B ▾] metric [vmaf ▾]  → table: metric, BD-rate %, BD-metric, points used, warnings           │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────── ┘
```

**Defaults.** Every normalisation control is initialised from the core's `NormalizationConfig` defaults (chapter 9 §9.2.4: `align.mode = auto`, `crop.auto_apply`, `scale.to = reference`, `scale.kernel = bicubic`, `bit_depth = promote_lower`, `range = as_signalled`); the dialog never carries its own defaults, so GUI and `ovt metrics` produce identical numbers for the same inputs.
**Interactions.** Job dialog validates (same frame count or alignment enabled; HDR metrics require matching transfer or conversion); Run → job in the Jobs model with progress/cancel; results appear per frame as they arrive (plot grows); click a plot point or table row → `Selection.frame`; "heatmap overlay" toggle sends the map (`map_blob_id`, `map_block_size`) to the Picture as `HeatmapSource::MetricMap` with range = metric range (e.g. PSNR 20–60 dB; configurable in the Legend); pooled values row; **comparison project** (FR-VQ-018): a run set holds one reference and up to 8 distorted files, run as one cancellable job set with a progress bar per file, shown as RD points (bit rate from `StreamIndex.frames[].size_bytes` over the container duration vs pooled metric) in the RD plot of the BD-rate tab; BD-rate tab needs ≥ 4 rate points per set (sets are groups of runs over files tagged with a bitrate/QP; "Add run to set" context action; BD-rate between any two selected files via `ovt_metrics_bd`); **first-mismatch finder** (FR-PLN-009): toolbar action "Find first mismatch" runs the core compare job (chapter 9 §9.3.3) and reports the result as diagnostic `VQ-DIFF-0001 {frame, plane, x, y, value_a, value_b}` in this panel and in Diagnostics; double-click seeks to the frame and pins the probe at (x, y). Export per run uses the core exporters only: CSV = chapter 9 §9.9.1 layout (`ovt_metrics_export_csv`), JSON = OVT metrics report (chapter 9 §9.9.2, `ovt_metrics_export_json`) or libvmaf-compatible JSON (`ovt_metrics_export_vmaf_json`, CLI `--format vmaf-json`).
**States.** No reference ("Open a reference (Ctrl+Shift+O) to run full-reference metrics; no-reference metrics (UVQ) can run now"); running (progress, partial results); cancelled (results kept, badge); error (engine message with code).
**Performance.** Plot via `TimelineChart` reuse (same LOD code); table virtualised; heatmap conversion to cell grid done in C++ (`map_block_size` → cells) once per frame.

### 11.3.14 HDR panel

**Purpose.** All HDR metadata with provenance, per-frame measurements, scopes, and the tone-mapping controls.
**Data source.** `HdrMetadata` (container, Chapter 8), SEI units (`StreamIndex.sei[]` headers: mastering display colour volume, content light level, alternative transfer characteristics, ST 2094-40, DV RPU summary), AV1 metadata OBUs (`METADATA_TYPE_HDR_CLL/HDR_MDCV/ITUT_T35`), `ovt_ColorDescription`, `VideoSurface::measure()`.
**Layout.** Tabs: **Metadata** (table: item, value, provenance (`container colr/mdcv/clli`, `SEI #unit`, `OBU #unit`, `DV RPU`), consistency ⚠ when container and bitstream disagree — e.g. `colr` says PQ, SPS VUI says BT.709 → `Diagnostic DEC-HDR-0001` (chapter 7 §7.9.4)), **Measurements** (current frame: MaxCLL, MaxCLL 99.99 %, FALL, min; running over the scanned range: MaxCLL, MaxFALL, with a "Scan range…" button → job calling `measure()` per frame; comparison row vs signalled `max_cll/max_fall` with ✔/⚠ — the **declared-vs-measured verdict**: ✔ when measured ≤ declared × 1.05, ⚠ `VQ-HDR-0002` (chapter 9 §9.11.4) when measured exceeds declared by more than 5 %, computed by the core exactly as `ovt hdr --measure --compare-declared` (chapter 12) so GUI and CLI agree), **Scopes** (waveform with axis mode [log nits | PQ code] and channel [Y | RGB parade]; vectorscope with gamut triangles 709/P3/2020 toggles and gain ×1/×2; CIE xy with locus and triangles; histogram in PQ code values with band markers; all drawn from `ScopeTexture`s through a small `QQuickRhiItem` plus QML axis labels), **Display** (tone map combo Auto/BT.2390/BT.2446a/Spline/Clip/None, clip nits, gamut map combo, target peak override, false colour toggle (F12), gamut-clip overlay toggle, dither toggle, macOS encoding option, Dolby label and profile).
**Interactions.** Metadata rows link to Syntax/Container; scopes hover shows the value at the cursor (nits at a waveform row; CbCr at a vectorscope point); false-colour legend (Chapter 10 Table 10.7-1) appears on the Picture when enabled.
**States.** SDR content → Metadata reads "SDR (BT.709)" and Measurements still work (203-nit reference); measurement `valid == false` → values greyed with "measuring…".
**Performance.** `measure()` only while the panel or false colour is visible (Chapter 10 §10.7.9).

### 11.3.15 Display Capabilities

**Purpose.** Show `DisplayCaps` (Chapter 10 §10.4.5) for the screen hosting the Picture and for the HDR monitor window.
**Layout.** Key/value table exactly as listed in Chapter 10 §10.4.5 "Panel contents"; buttons Re-probe, Show test pattern (toggle), Copy report.
**States.** Re-probing spinner; "unknown" values shown as `—` with the reason.

### 11.3.16 Diagnostics / Log

**Purpose.** All `Diagnostic`s (families of D2: `CNT-*`, `SYN-*`, `DEC-*`, `ANA-*`, `VQ-*`, `RND-*`, `GUI-*`, …) and the log stream, with jump-to.
**Data source.** `*.diagnostics[]` vectors from every FlatBuffer, `ovt_set_logger` callback (ring buffer 10 000 lines).
**Layout.** Tabs **Errors** (table: severity icon, code, message, location (`unit #412 bit 102` / `box moov/trak[1]/stsz` / `frame #1203 block 37` / `file offset`), spec ref; filter by severity/module/text; count badge on the tab turns red when Errors > 0) and **Log** (level filter, module filter, text search, pause/follow, Save…).
**Interactions.** Double-click an error → navigates: unit/bit → Syntax + Hex; box → Container + Hex; frame/block → Picture selection; `VQ-DIFF-0001` → seek + pinned probe at (x, y); click spec ref → help. Right-click → Copy, Copy all as text, Report bundle (zips log + diagnostics JSON + stream summary, never the media).
**States.** Empty ("No diagnostics"); "N more…" when > 10 000 (oldest dropped).
**Performance.** Diagnostics model merges vectors lazily; log appends in batches every 100 ms.

### 11.3.17 Bookmarks & Comments

**Purpose.** Named frame bookmarks and anchored comments, stored in the project.
**Data source.** `.ovtproj` `bookmarks[]`, `comments[]` (§11.2.6).
**Layout.** Two lists: Bookmarks (colour dot, name, `disp#/dec#`, type, time; shown as markers on the Timeline) and Comments (anchor kind icon, excerpt, author, date). Buttons Add bookmark (`B`), Add comment (`Shift+B` → inline editor anchored to the current selection: frame, block, byte range, field or box), Export JSON/CSV, Import.
**Interactions.** Click → navigate to anchor (frame/block/bytes/field/box via `SelectionModel`); `N`/`Shift+N` next/previous bookmark; rename inline; drag to reorder; right-click → Delete, Change colour.
**States.** Empty hints.

### 11.3.18 Jobs (popover, not a dock panel)

List of running/queued/finished jobs (`JobsModel` over `ovt_Job`): name, progress %, ETA, Cancel button, "Cancel all"; finished jobs keep errors for 5 minutes. Opened from the toolbar indicator; also `Ctrl+J`.

### 11.3.19 Welcome / Start page (replaces Picture when no file is open)

See §11.8.

### 11.3.20 Buffer (HRD) panel (phase 4, FR-GUI-032)

**Purpose.** Show the CPB fullness curve of the HRD model (FR-DEC-013) synchronised with the current frame.

**Data source.** `ovt_index_hrd_trace(stream, &opts, &buf)` → `HrdTrace` (chapter 7 §7.8; one `HrdPoint` per access unit in decode order with `init_arrival_time_90k`, `final_arrival_time_90k`, `removal_time_90k`, fullness in bits, plus `cpb_size_bits`, `bit_rate_bps` and the `DEC-HRD-0010` overflow / `DEC-HRD-0011` underflow diagnostics). Computed once per stream as a job (§11.7); `HrdModel` caches it.

**Layout.** Header row: `sched_sel_idx` combo, NAL/VCL HRD toggle (only the types the stream signals), CBR/VBR and low-delay badges. Body: a Qt Graphs 2D line chart, x = time (seconds, from `removal_time_90k`), y = CPB fullness in bits with a dashed horizontal line at `cpb_size_bits`; a vertical cursor at the current frame's access unit; underflow/overflow events as red markers (`#E53935`) with the diagnostic text as tooltip. Footer: fullness at the cursor in bits and as % of `CpbSize`.

**Interactions.** Click on the curve → `Selection.frame` of the nearest access unit; double-click on a red marker → seek to that access unit and select its diagnostic in Diagnostics. Streams without HRD parameters show the empty state "No HRD parameters in this stream".

**States.** Computing (progress band), empty (no HRD), error (diagnostic row). Delivered by WI-4.470.

---

## 11.4 Interaction model

### 11.4.1 Selection and cursor synchronisation

Single source of truth: the `SelectionModel` singleton (`app/src/models/selection_model.h`), exposed to QML as `Selection`.

```cpp
struct SelectionState {
  QString file_id;                       // .ovtproj file id ("f0"); A side
  int track_index = 0;
  ovt::fb::FrameId frame{-1, -1};        // current frame (both indices always filled from StreamIndex)
  bool display_order = true;             // navigation/order mode (O)
  uint32_t block_id = 0xFFFFFFFFu;       // leaf block at the clicked cell (FrameAnalysis.blocks index)
  int level = -1;                        // index into the codec's cycle chain (0 = coarsest, set by the first click); -1 = none/leaf
  int mv_index = -1;                     // FrameAnalysis.mvs index
  int unit_index = -1;                   // StreamIndex.units index
  int sample_index = -1;                 // container sample
  ovt::fb::ByteSpan byte_range{0, 0};    // file bytes (length 0 = none)
  ovt::fb::BitSpan bit_range{0, 0};      // within unit_index: NAL header + RBSP (H.26x) or whole OBU from obu_header (AV1), see ch. 6 §6.2.7 (length 0 = none)
  QString field_path;                    // "slice_segment_header/short_term_ref_pic_set/num_negative_pics" or "moov/trak[1]/tkhd/duration"
  QPoint probe{-1, -1};                  // pinned pixel probe (luma samples)
  QRect selection_rect;                  // luma samples; null = none
  QString origin;                        // panel id that made the change (for echo suppression)
};
```

API: `setFrame(FrameId, origin)`, `setBlock(block_id, level, origin)`, `setMv`, `setUnit`, `setSample`, `setBytes`, `setField(unit_index, path, bit_range, origin)`, `setProbe`, `setRect`, `clearBlock()`, `clear()`; one signal `changed(SelectionChange mask)` with bits {Frame, Block, Mv, Unit, Sample, Bytes, Field, Probe, Rect, Order}. Derived fields are filled by the model itself (never by panels): setting `frame` fills `unit_index` = first VCL unit of the frame and `sample_index` from it; setting `unit_index` fills `frame` (if VCL), `byte_range = file_span`, `sample_index`; setting `field` fills `byte_range` by the RBSP→raw algorithm (§11.3.5); setting `sample_index` fills `byte_range` and `frame`; setting `block` clears `mv_index`; setting `frame` to a different frame clears `block_id`, `mv_index`, `probe` **unless** "lock pixel selection" is on (probe kept). Changes are applied synchronously; panels react in slots connected to `changed` and MUST ignore changes whose `origin` is themselves.

**Who updates on what:**

| Change ↓ / Panel → | Timeline | Picture | Syntax | Hex | Units | Container | Sel. Info | Block Pres. | Ref graph | Stats | Metrics | HDR | Bookmarks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Frame | cursor | new picture; overlays (fetch/analyse) | unit combo → first VCL unit | scroll to unit | row | Samples row | cleared / probe | cleared | current node, DPB | scope Frame | plot cursor | measurements | marker |
| Block / level | — | outline | (if ext has spans) node | (idem) bits | — | — | fields | grids | — | highlight class off | — | — | — |
| MV | — | arrow highlight | — | — | — | — | MV row | — | ref frame highlight | — | — | — | — |
| Unit | (VCL) frame | (VCL) frame | tree for unit | band + scroll | row | Samples row | — | — | — | — | — | (SEI) row | — |
| Bytes | — | — | deepest field covering | highlight | containing unit | containing box | — | — | — | — | — | — | — |
| Field | — | — | row | bits | — | (container field) row | — | — | — | — | — | — | — |
| Sample | frame | frame | — | bytes | units of sample | Samples row | — | — | — | — | — | — | — |
| Probe / Rect | — | crosshair / rect | — | — | — | — | probe values | hover cell | — | scope Selection | — | — | — |
| Order toggle | re-sort axis | — | — | — | — | — | — | — | row mode | — | plot x | — | labels |

Compare (B side): a second, read-only `SelectionState` `SelectionB` is derived, used by Picture B and Selection Info's A/B columns. Rule, first match wins: (1) if a manual frame offset is set (FR-PLN-004; Compare toolbar, persisted per project), `SelectionB.frame = SelectionA.frame + offset`, and when that index is outside B's frame range `SelectionB` is empty (Picture B shows the "no frame" placeholder, A/B columns show `—`); (2) else, when metrics alignment is known, the aligned index; (3) else the same `display_index`, again empty when B is shorter.

### 11.4.2 Overlay modes (F1–F12) and letter toggles

A **mode** is a preset of `OverlayConfig` (layers, heatmap source, label content, stage). Letter toggles modify the active preset (changes persist per mode in `panel_state`).

| Key | Mode | `layers` | `heatmap` | `label` | Stage |
|---|---|---|---|---|---|
| F1 | Coding flow / partitions | GridCTU, GridCU, GridPartition, Labels | None | BlockSize | Recon |
| F2 | Prediction | GridCU, GridPU, PredFill, SkipHatch, IntraDirs, MvL0, MvL1, Labels | None | PredMode | Recon |
| F3 | Motion vectors | GridPU, MvL0, MvL1, MvBi, Labels | None | MvValues | Recon |
| F4 | Transform / residual | GridCU, GridTU, Labels | TxSize | TxType | Residual (if available, else Recon) |
| F5 | QP map | GridCU, Heatmap, Labels | QP | QP | Recon |
| F6 | Bits heatmap | GridCU, Heatmap, Labels | BitsX8 | Bits | Recon |
| F7 | Reference indices | GridPU, RefIdxFill, MvL0, MvL1, Labels | None | RefIdx | Recon |
| F8 | Loop filter | GridCU, DeblockEdges, GridPartition | None | None | PreDeblock |
| F9 | In-loop filter params (SAO / CDEF / LR) | GridCTU, Heatmap | FilterIdx | None | PreSaoOrCdef |
| F10 | Metrics heatmap | GridCU, Heatmap | MetricMap | None | Recon |
| F11 | Plain picture | none | None | None | Recon |
| F12 | HDR false colour | none (+ `ViewState.false_color = true`) | None | None | Recon |

Letter toggles: `P` picture (adds/removes the picture under overlays), `I` Labels bit, `G` all Grid* bits of the mode, `M` all Mv* bits, `T` Heatmap bit, `L` legend panel visibility, `H`/`D` hex/decimal for all ⓗ columns and label values, `K` colour-blind palette, `E` deblock edges, `S` skip hatch, `F` full screen, `R` fit. Modes 1–10 set the Picture state badge to "Analysis pending" when no `FrameAnalysis` exists and trigger analysis automatically if Preferences ▸ Decoding ▸ "Auto-analyse on overlay mode" is on (default on for desktop, off on Android to save battery).

### 11.4.3 Full keyboard map (desktop; macOS uses ⌘ for Ctrl)

**This table is the single normative keyboard map of the specification (D8).** Chapter 3 UX-2, chapter 4 FR-GUI-008/FR-PLN-001/FR-PLN-004/FR-SYN-014 acceptance lines and the chapter 16 demo scripts cite it and must not define bindings of their own. Precedence: a panel-local binding (e.g. Hex `F3`) applies only while that panel has focus; single-letter global shortcuts are suppressed while a text field has focus; `F1`–`F12` and `Ctrl+…` are window-global.

| Key | Action | Key | Action |
|---|---|---|---|
| `Ctrl+O` / `Ctrl+Shift+O` | Open / Open reference | `Space` | Play / pause |
| `Ctrl+S` / `Ctrl+Shift+S` | Save project / as | `→` / `←` | Next / previous frame (current order) |
| `Ctrl+W` / `Ctrl+Q` | Close file / Quit | `Shift+→` / `Shift+←` | ±10 frames |
| `Ctrl+N` | New window | `Ctrl+→` / `Ctrl+←` | Next / previous RAP (I/Key) |
| `Ctrl+,` | Preferences | `Alt+→` / `Alt+←` | Next / previous frame of the same type |
| `Ctrl+F` | Find in focused panel | `.` / `,` | Next / previous frame in **decode** order regardless of mode |
| `Ctrl+G` | Go to (frame/time/offset/unit) | `Home` / `End` | First / last frame |
| `Ctrl+C` / `Ctrl+Shift+C` | Copy / Copy as JSON | `O` | Toggle decode/display order |
| `Ctrl+E` | Export menu | `A` / `Shift+A` | Analyse frame / GOP |
| `Ctrl+M` | New metrics job | `Ctrl+.` | Cancel all jobs |
| `Ctrl+H` | Toggle HDR output | `Ctrl+J` | Jobs popover |
| `Ctrl+Shift+F` | Picture only | `F` | Full screen |
| `Ctrl+Shift+L/R/B` | Toggle left/right/bottom areas | `Esc` | Clear block/probe/rect; close popups; leave full screen |
| `Ctrl+Shift+1..9` | Focus panel N | `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous tab in group |
| `F1`–`F12` | Overlay modes (§11.4.2) | `P I G M T L H D K E S` | Toggles (§11.4.2) |
| `1 2 3 4 5` | Plane YUV / Y / U / V / Gray (`plane_mode` 0–4) | `+` / `−` / `R` | Zoom in / out / fit |
| `Ctrl+1/2/4` | 100 / 200 / 400 % | `Tab` / `Shift+Tab` | Cycle block level down / up |
| `C` / `Shift+C` | Cycle compare mode forward / back | `X` | Swap A/B |
| `[` / `]` | Difference gain − / + | `V` | Toggle false colour (= F12) |
| `B` / `Shift+B` | Add bookmark / comment | `N` / `Shift+N` | Next / previous bookmark |
| `Ctrl+↓` / `Ctrl+↑` | Next / previous error (Diagnostics) | `?` | Keyboard map |
| `Ctrl+B` (Hex focused) | Toggle binary ribbon | `Ctrl+D` | Toggle Dual view (§11.3.2) |
| `Enter` (in trees/tables) | Activate row (navigate) | `Ctrl+Enter` | Activate in new window (desktop) |
| `Shift+arrows` (Picture, block selected) | Move selection to the neighbouring block at the current level | `Ctrl+Shift+arrows` | Move pinned probe by one sample |
| `PageUp` / `PageDown` | ±1 GOP | `Shift+PageUp/Down` | ±1 second |
| `Ctrl+0` | Reset layout to preset | `Ctrl+Shift+T` | Toggle theme |

The map is user-editable (`keymap.json` in the settings directory of §11.2.5: `{ "action.navigate.next": ["Right", "J"] }`; an action id is `action.` plus the menu id of §11.2.3 (`action.navigate.next`), and keys without a menu item follow the same pattern (`action.picture.levelDown`)); conflicts are judged on the whole file (`{ "action.navigate.next": ["Right", "L"] }` alone is rejected because `L` is still the legend toggle, while the same file that also moves the legend away loads) and only between actions that share a scope (window-level, or the same panel); they are flagged in Preferences ▸ Keyboard with `GUI-KEY-0001`. Single-letter shortcuts are **window-level** but suppressed while a text field has focus.

### 11.4.4 Mouse and touch gestures

| Gesture (mouse) | Touch equivalent | Where | Action |
|---|---|---|---|
| Wheel | Pinch (two fingers; centroid = cursor) | Picture, Timeline, Ref graph, scopes | Zoom about cursor/centroid (Picture: ×2 per wheel notch = 15° = `QWheelEvent::angleDelta().y() == 120`; smooth-scrolling devices accumulate `angleDelta` and apply 2^(Δ/120) so one notch's worth is still ×2; Timeline ×1.25 per notch) |
| Shift+Wheel | Two-finger horizontal pan | Timeline, Hex (horizontal) | Pan/scroll horizontally |
| Left drag on empty area / Middle drag / Space+drag | One-finger drag (when no block under finger, or two-finger drag always) | Picture | Pan |
| Left click | Tap | Picture | Select block at cycle level; repeated → cycle |
| Shift+drag | Long-press then drag | Picture | Selection rectangle |
| Ctrl+click | Long-press (600 ms) | Picture | Pin/unpin pixel probe |
| Right-click | Long-press (600 ms) on a block / two-finger tap | all | Context menu ("Details") |
| Double-click | Double-tap | Picture | Reset zoom to fit |
| Double-click | Double-tap | Timeline / Ref graph / Units / Diagnostics | Navigate (seek / jump) |
| Drag | Drag | Timeline | Range selection; dragging the divider in Split compare moves it |
| Hover | — (touch has no hover; the status read-out follows the last tap) | all | Tooltips, hover outline, probe read-out |
| Drag tab header | Long-press tab then drag | Dock | Rearrange panels |
| Drag file onto window | Share-to / Open-with intent (Android) | Shell | Open file (second file while one is open → asks role: A/B/reference) |

Hit targets ≥ 24×24 dip on desktop, ≥ 48×48 dip on touch (§11.5). Gestures never trigger core work synchronously; pans/zooms update `ViewState` only.

### 11.4.5 Colour language (all values from `ovt::render::kOverlayColors`, Chapter 10 §10.5.2, or defined here)

| Use | Light theme | Dark theme | Notes |
|---|---|---|---|
| Frame type I / Key | `#E53935` | same | timeline bars, filmstrip borders, ref-graph nodes; AV1 IntraOnly = I colour hatched; Switch = `#9E9E9E` |
| Frame type P / Inter | `#1E88E5` | same | |
| Frame type B | `#43A047` | same | |
| Hidden frame (AV1 `shown=false`) | hatch over type colour | same | |
| Intra / Inter L0 / Inter L1 / Bi / Skip | `#2ECC40` / `#FF851B` / `#B10DC9` / `#00E5FF` / hatch `#AAAAAA` | same | legend, Selection Info chips, ref arrows |
| Severity Info / Warning / Error / Fatal | `#607D8B` / `#FFB300` / `#E53935` / `#B71C1C` | same | icons ℹ ⚠ ✖ ⛔; row tint 10 % |
| Selection (block outline, current row) | `#FFFFFF`+`#000000` on Picture; `#1565C0` 25 % rows | `#1E88E5` 30 % rows | |
| Hover | `#FFD700` | same | |
| Range selection / selection rect | `#39CCCC` 25 % | same | |
| Hex: unit band / prefix / EPB / field / partial-byte | `#1E88E5` 12 % / 24 % / strikethrough grey / `#FFD700` 45 % / striped | same | |
| Role A / B | `#1E88E5` / `#FB8C00` | same | file chips, compare divider labels |
| Analysis pending | hatch `#FFB300` 40 % | same | |
| Heatmap default LUT | viridis | viridis | colour-blind safe; the Okabe-Ito overlay palette is used when `K` is on |
| UI chrome | Qt Quick Controls `Material`-free custom style `OvtStyle` (Fusion-like) with system accent | | text contrast ≥ 4.5:1 (checked by `app/tests/a11y/test_contrast.cpp`) |

---

## 11.5 Android / ChromeOS adaptations

### 11.5.1 Shell structure

`android/app/`: Kotlin `MainActivity` (`AppCompatActivity`), layout `FrameLayout { QtQuickView (match_parent), SurfaceView hdrSurface (positioned by code) }`. The Qt side is the same QML as desktop (`app/qml/`) compiled into the APK with `androiddeployqt` (ADR-012); platform differences are expressed through a `Platform` QML singleton (`Platform.isAndroid`, `Platform.widthDp`, `Platform.hasHover`, `Platform.windowingMode`). Native glue in `android/app/src/main/cpp/` (JNI) and `app/src/platform/android_*.cpp`. ABIs: `arm64-v8a`, `x86_64` (ChromeBooks). `minSdk 28`, `targetSdk 36`.

Manifest essentials: `android:resizeableActivity="true"`, `android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|density|keyboard|keyboardHidden|navigation|uiMode"` (no Activity restart on resize/rotation), `<meta-data android:name="WindowManagerPreference:FreeformWindowSize" android:value="maximize"/>`, `<meta-data android:name="WindowManagerPreference:FreeformWindowOrientation" android:value="landscape"/>` (ChromeOS defaults), `android:colorMode="hdr"`, `<uses-feature android:name="android.hardware.touchscreen" android:required="false"/>`, `<uses-feature android:name="android.software.leanback" android:required="false"/>`, intent filters for `video/*`, `application/octet-stream` with extensions `.mp4 .mov .m4v .ivf .obu .h264 .264 .h265 .hevc .y4m .yuv .ovtproj`, `ACTION_VIEW` + `ACTION_SEND` (share-to).

### 11.5.2 Layouts by width (smallest-width breakpoints; `Platform.widthDp`)

| Width | Layout | Panels |
|---|---|---|
| ≥ 1200 dp (Chromebook maximised, 12"+ tablet landscape) | **Three-pane** = desktop layout presets unchanged | all; Left area default collapsed to icons until hovered/tapped |
| 840–1199 dp (≥ 10" tablets landscape, split-screen) | **Two-pane**: Picture (60 %) + Right tabbed area; Timeline bottom (compact, 120 dp); Left area becomes a modal drawer (swipe from left edge / hamburger) | Right tabs: Syntax, Selection Info, Statistics, Metrics, HDR, Container, Units, Hex (scrollable tab bar) |
| 600–839 dp (10" portrait) | **Two-pane vertical**: Picture top (50 %) + tabbed area below; Timeline as a 96 dp strip between them | same tabs |
| < 600 dp (phones — unsupported but MUST not crash) | **Single pane** with bottom navigation (Picture, Timeline, Info, More); panels full-screen one at a time | functional minimum |

Dual view and the Block Samples by width class: at ≥ 1200 dp both behave as on desktop; at 840–1199 dp Dual view is forced to the Vertical arrangement inside the Picture region and the Block Samples becomes a tab of the Right tabbed area; below 840 dp Dual view is unavailable (the View ▸ Dual view action is disabled with the tooltip "needs a wider window", and an active Dual view falls back to single surface A, keeping `SelectionB` for Selection Info) and the Block Samples opens as a full-height tab of the tabbed area.

Layout switches on `widthDp` change without losing state (§11.5.7). Dock drag-and-drop is disabled below 1200 dp (tabs reorderable by long-press instead). Floating panels never exist on Android.

### 11.5.3 Free-form windows, keyboard, mouse, hover

- Resizable windows (ChromeOS, Android 15 QPR1+/16 desktop windowing): handled by the breakpoint table; `onConfigurationChanged` → QML re-layout ≤ 1 frame; no `Activity` recreation.
- Keyboard: the full desktop map (§11.4.3) applies; `Ctrl` is used as on Linux. Hardware keyboard presence (`Configuration.keyboard == KEYBOARD_QWERTY`) hides on-screen helper buttons (zoom ±, Analyse) that exist for pure-touch devices.
- Mouse: hover (`Platform.hasHover` from `InputDevice` sources containing `SOURCE_MOUSE`) enables tooltips and hover outlines; right-click = context menu; wheel = zoom/scroll as desktop; `Ctrl+wheel` zoom is the same.
- Touch: table of §11.4.4; two-finger tap = right-click; long-press = right-click/pin.
- Stylus: treated as mouse with hover (S Pen / USI).

### 11.5.4 Touch targets and density

All interactive controls ≥ 48×48 dp hit area (Material guideline); visual size may be smaller (≥ 32 dp) with the hit area expanded (`TapHandler.margin`). Splitter handles 12 dp hit, 2 dp visible. Table rows ≥ 40 dp on touch, 24 dp with a mouse (`Platform.hasHover`). Text ≥ 12 sp. Qt 6 high-DPI scaling is always on; the app sets `QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough)` before the `QGuiApplication` is created so Android density and desktop fractional scales pass through unrounded.

### 11.5.5 HDR `SurfaceView` sibling positioning rules

1. The QML `PicturePanel` emits `pictureRectChanged(x, y, w, h)` in window pixels (`mapToItem(null)` × `Screen.devicePixelRatio`) on every geometry/visibility change; the shell sets `hdrSurface.layoutParams` margins/size accordingly on the UI thread (`post`ed, coalesced per frame).
2. `hdrSurface.setZOrderMediaOverlay(true)`: it composites above `QtQuickView`'s surface. Because QML items cannot draw above it, **overlays are drawn by `libovt-render`** (Chapter 10 R-ARCH-1) and the legend/probe read-outs live outside the picture rect.
3. **Popup rule.** When any QML `Popup`/`Menu`/`Dialog` opens (tracked by a global `Overlay.overlay` child-count watcher) or the Picture panel is not fully visible (tab not current, drawer open, window not focused), the shell calls `nativeSetVisible(false)` → `hdrSurface.visibility = GONE` and the Picture shows the EMBEDDED SDR texture (Chapter 10 R-ARCH-3). On close, the sibling is shown again. The brightness change during a popup is accepted behaviour and documented in Help.
4. Scroll/resize: during a `SplitView` drag the sibling follows per frame; the `SurfaceView` resize causes `surfaceChanged` → `VideoSurface::resize` (no re-init).
5. Multi-window / picture-in-picture: PiP is not supported (`supportsPictureInPicture=false`); in split-screen the rules above apply unchanged.
6. On `onPause` the sibling is hidden and `VideoSurface` keeps its state; on `surfaceDestroyed` → `retarget()` to offscreen; on `surfaceCreated` → `retarget()` back.

### 11.5.6 File access (Storage Access Framework)

Open via `ACTION_OPEN_DOCUMENT` (`Intent.CATEGORY_OPENABLE`, MIME `*/*` with `EXTRA_MIME_TYPES` {`video/*`, `application/octet-stream`}); `takePersistableUriPermission(uri, FLAG_GRANT_READ_URI_PERMISSION)` so project files can reopen the URI; `ParcelFileDescriptor pfd = contentResolver.openFileDescriptor(uri, "r")` → `ovt_file_open_fd(pfd.detachFd(), size, &file)` (§0.6.1; core uses `pread`). Directory access for sidecar reference files via `ACTION_OPEN_DOCUMENT_TREE` (optional). Files on removable/cloud providers that do not support random access (`pfd` is a pipe) → diagnostic `GUI-AND-0001` ("provider does not support seeking; copy the file locally", §11.7 registry) with a "Copy to app storage" action (streams to `getExternalFilesDir`). Cache directory = `context.cacheDir/ovt` (size limit 2 GiB default on Android, chapter 14 §14.9.3 / D5). Recent files list stores URIs + display names (`OpenableColumns.DISPLAY_NAME`). Export uses `ACTION_CREATE_DOCUMENT`.

### 11.5.7 State preservation

Core handles (`ovt_File`, `ovt_Container`, `ovt_Stream`, caches) are owned by a process-lifetime `OvtSession` singleton (not by the Activity); `MainActivity` re-binds on recreation. `onSaveInstanceState` stores: project path/URI, `SelectionState`, `ViewState`, layout preset, panel state (`QVariantMap` → JSON ≤ 100 KB). Rotation/resize never restarts the Activity (manifest), but when the system does recreate it (process death), the saved state restores the same frame and selection within 2 s of relaunch. Jobs continue across configuration changes; cancelled on `onDestroy` with `isFinishing`.

---

## 11.6 Accessibility and i18n

1. Every interactive QML item sets `Accessible.role`, `Accessible.name` (`qsTr`), and `Accessible.description` where the purpose is not obvious; tables expose rows/columns via `TableView`'s accessibility; the Picture exposes the current selection summary as its `Accessible.name` (so screen readers read "CU 14 32×32 Inter L0").
2. Keyboard focus order follows the visual order per area (Left → Center → Right → Bottom); focus is visible (2 dp outline `#1E88E5`); `Tab` never gets trapped (the Picture's `Tab` cycle is active only when a block is selected; otherwise `Tab` moves focus — documented in the keyboard map).
3. Contrast ≥ 4.5:1 for text and ≥ 3:1 for UI glyphs (§0.7.7, NFR-023); verified by `app/tests/a11y/test_contrast.cpp` over the theme palettes.
4. No information by colour alone: skip uses hatching, L0/L1 arrows differ by shaft pattern (L1 shafts are dashed 4/2 px, chapter 10 §10.5.4; heads are identical), severities have icons, frame types have letters in tooltips and on bars when ≥ 12 px wide.
5. Colour-blind-safe sets: viridis/cividis heatmaps; Okabe-Ito overlay palette (`K`).
6. Font scaling: all sizes in `sp`/points via `OvtStyle.fontScale` (0.8–1.6), honours system font size on Android and macOS accessibility zoom.
7. Reduced motion: when the OS asks (`QAccessible`/Android `ANIMATOR_DURATION_SCALE == 0`), transitions are instant; Flicker compare still works (explicit user action).
8. i18n: all strings `qsTr` with disambiguation contexts; `.ts` catalogues under `app/i18n/`; plurals via `%n`; numbers/dates through `QLocale` **except** code values, offsets, hex and spec identifiers (always C locale, no grouping unless the user enables "group digits" in Preferences); layouts use anchors/`Layout` (no fixed widths for text); RTL mirroring is not required in v1 but `LayoutMirroring` MUST not be broken by hard-coded `x` offsets. Spec names (`SyntaxNode.name`) are never translated.

---

## 11.7 Error and progress UX

- **Jobs model.** Every core call longer than 50 ms runs as a job (`ovt_Job`) with name, progress (0–1 or indeterminate), cancel. The toolbar indicator shows the count; the status bar shows the foreground job's progress; the Jobs popover lists all. Jobs that block a panel show the panel's "loading…" badge and a cancel link in the panel's empty area.
- **Cancellation.** `Esc` in a panel cancels its own pending job; `Ctrl+.` cancels all; closing a file cancels its jobs and waits ≤ 2 s (then detaches; the job finishes in the background and drops its result).
- **"Analysis pending" affordance.** Any panel that needs `FrameAnalysis` shows, when it is missing: a hatched band (colour language) with "Block data for frame #N is not analysed — [Analyse (A)] [Analyse GOP]" and, while running, a progress ring with ETA computed from the plugin's measured frames/s. The Picture keeps showing the playback-path image underneath.
- **Partial data.** Panels show what exists and mark gaps: yellow badge "partial" with a tooltip listing missing sources (e.g. "plugin does not export: intra_mode_c, cbf"); table cells read `n/a`; charts omit classes; metric plots leave gaps. Partial is never an error.
- **Errors.** Red badge + one-line message with the stable code (`SYN-H265-0007`, `RND-0040`, `GUI-AND-0001`) and a "Details" link to Diagnostics; panels keep their last good content. Fatal stream errors (cannot index) put the file chip in error state; the project stays open. Decoder plugin crash protection (ADR-013 phase 1 is in-process): a plugin returning `OVT_E_PLUGIN` disables block analysis for that stream with the message "Analysis plugin failed (code); playback continues".
- **Progress wording.** `"Indexing… 42 % · 1,203 / 2,870 frames · 00:12 left"`, `"Analysing GOP 37 (frames 1180–1239)… 18 / 60"`, `"VMAF 0.6.1 · 318 / 1500 frames · 47 fps"`.
- **Confirmations** only for destructive actions (clear cache, discard unsaved project, delete bookmark with comments); never for navigation.

**GUI diagnostic registry (`GUI-*`, D2).** Codes raised by `ovt-studio` itself (everything else comes from the core and keeps its family). Messages are `qsTr` templates.

| Code | Severity | Message template | When raised | Spec ref |
|---|---|---|---|---|
| `GUI-AND-0001` | Error | provider does not support seeking; copy the file locally | SAF `ParcelFileDescriptor` is a pipe/socket (`fstat` not regular) | §11.5.6 |
| `GUI-PROJ-0001` | Warning | file changed since project was saved (`{path}`) | `.ovtproj` `source_hash` ≠ `StreamIndex.source_hash` | §11.2.6 |
| `GUI-PROJ-0002` | Error | project file invalid: `{json_pointer}` `{reason}` | `.ovtproj` fails schema validation | §11.2.6 |
| `GUI-PROJ-0003` | Error | file not found: `{path}` | project file entry missing on load | §11.2.6 |
| `GUI-KEY-0001` | Warning | shortcut `{key}` is bound to `{action_a}` and `{action_b}` | `keymap.json` conflict | §11.4.3 |
| `GUI-SET-0001` | Warning | settings file unreadable, defaults used (`{reason}`) | `settings.json` parse/schema failure | §11.2.5 |
| `GUI-RAW-0001` | Error | raw format does not match the file size (`{expected}` ≠ `{actual}`; `actual` = file size, `expected` = `max(1, floor(actual / frame_bytes)) × frame_bytes`; raised whenever `actual` is not a multiple of the frame size) | raw YUV dialog | §11.2.6 |
| `GUI-JOB-0001` | Warning | job `{name}` did not stop within 2 s; detached | close-file cancellation timeout | §11.7 |

---

## 11.8 First-run and empty states

- **Welcome page** (shown in the Center area when no file is open): big "Open file… (Ctrl+O)" and "Open project…", drag-and-drop zone ("Drop a video, elementary stream or .ovtproj"), Recent files (name, path/URI, codec, last opened) with pin/remove, "Sample streams" (at most 5 entries tagged `fast` with a URL from the corpus manifest, smallest first; the manifest is read from `OVT_CORPORA_MANIFEST`, else `<application dir>/../share/ovt/corpora/manifest.json`, and the list is empty when absent; a consent dialog showing size, SPDX id and licence text and the URL is shown every time and emits `sampleConsented(id)`; the download itself is a later WI; the Open buttons trigger `file.open` / `file.openProject` and are disabled until the file-open WI enables them), and a three-line "How it works" (Timeline → Picture → Syntax/Hex) with the keyboard map link.
- **First run**: a one-time, dismissible banner "ovt-studio sends no data anywhere: no telemetry, no crash reporter." On Android an additional banner explains SAF file access and the cache location.
- **Panel empty states** (one sentence each, `qsTr`): Timeline "Open a file to see its frames."; Picture (file open, nothing decoded yet) "Decoding first frame…"; Syntax "Select a unit in the Unit list or a frame."; Hex "Select bytes, a unit, a box or a syntax field."; Container "This file has no container (elementary stream)."; Selection Info "Click a block in the Picture (analysis required: press A)."; Block Samples "Select a block."; Ref graph "Index in progress…"; Statistics "No analysed frames in scope — press A."; Metrics "Open a reference (Ctrl+Shift+O) or run a no-reference metric."; HDR "SDR content — measurements use 203 cd/m² reference white."; Display Capabilities (never empty); Diagnostics "No diagnostics."; Bookmarks "Press B to bookmark the current frame."
- **Second file opened while one is open**: dialog "Open as: [Replace] [Reference (B), current becomes distorted] [Distorted (A), current becomes reference] [New window]". Replace keeps one file with role `single`.

---

## 11.9 GUI test plan

Normative test mechanics live in chapter 14: §14.10.1 (QML model tests), **§14.10.2 (screenshot harness — the only screenshot definition)**, §14.10.3 (test IPC and pytest interaction scripts), §14.10.4 (Android instrumentation), §14.9.3 (performance budgets). This table only lists what the GUI chapter requires those mechanisms to cover.

| Layer | Tool | Location | What |
|---|---|---|---|
| Models | Catch2 (C++) | `app/tests/models/` | `SelectionModel` derivation rules and echo suppression (every row of the §11.4.1 table); `HexModel` paging and RBSP→raw mapping (EPB cases: 0, 1, many, EPB inside the field); `SyntaxTreeModel` over fixture FlatBuffers (row counts, filter results); `TimelineChart` LOD aggregation; `StatsAggregator` normalisations; `.ovtproj` round trip + schema validation (`nlohmann::json-schema-validator`); keymap conflicts |
| QML units | Qt Quick Test (`qmltestrunner`) | `app/tests/qml/tst_*.qml` | panels instantiate with mock models; toggles map to `OverlayConfig` bits; dock model moves/serialises; breakpoints switch layouts at 600/840/1200 dp (`Platform.widthDp` mocked) |
| Interaction scripts | the in-app test IPC of chapter 14 §14.10.3 (`ovt-studio --test-ipc <socket>`, JSON lines: `open`, `click <objectName> [row]`, `key`, `get`, `screenshot`, `wait-idle`) driven by pytest | `app/tests/scripts/test_*.py` | in addition to the five required scripts of §14.10.3: `test_open_seek_select.py`, `test_syntax_to_hex.py` (click field → hex highlight offsets match the FFmpeg `trace_headers` golden), `test_container_to_hex.py`, `test_keyboard_only.py` (opens a file, seeks, selects a block, exports CSV with the keyboard alone), `test_compare_modes.py`, `test_dual_view.py` (FR-GUI-021 acceptance: pan A by (100, 50) → B pans identically; unlock pan → it does not), `test_metrics_job.py`, `test_desktop_idioms.py` (also run on ChromeOS), `test_android_breakpoints.py` |
| Screenshots | chapter 14 §14.10.2 harness (`ovt_studio_screenshots`, offscreen, 96 DPI, 1600×1000, bundled `Inter-Variable.ttf`, both themes, SSIM/tile tolerance as defined there) | `app/tests/screenshots/` per §14.10.2 | the §14.10.2 scene list plus: every panel in each state (empty/loading/partial/error) and each layout preset. Chrome scenes run with the stub renderer (`OVT_RENDER_STUB=1`, deterministic checker + text) so chrome is tested independently of the GPU renderer; renderer output has its own goldens (chapter 10 §10.9.1) |
| Performance | `tests/perf/gui/bench_gui_open_first_frame.cpp`, `bench_gui_scrub.cpp`, `bench_gui_cold_start.cpp` (chapter 14 §14.9.3 harness, D14) | self-hosted runners | open-to-first-frame ≤ 500 ms (M1, NFR-002); cold start ≤ 1.5 s M1 / ≤ 3 s Pixel Tablet (NFR-008); scrub 300 frames with F3 overlays: UI frame time p95 ≤ 16 ms on desktop and ≤ 33 ms on the Pixel Tablet (D5), no job on the GUI thread > 50 ms (`QElapsedTimer` instrumentation under `OVT_GUI_TRACE=1`, a runtime environment variable, not a CMake option) |
| Android | UI Automator / Espresso (chapter 14 §14.10.4) | `android/app/src/androidTest/` | SAF open via a test `DocumentsProvider`; rotation/resize keeps `Selection.frame`; `SurfaceView` bounds equal the picture rect; popup hides the sibling; breakpoints on emulator profiles (Pixel Tablet, Chromebook x86_64 1920×1080 free-form 1200×800 window) |
| Accessibility | `app/tests/a11y/test_a11y.cpp`, `app/tests/a11y/test_contrast.cpp` | | every focusable item has `Accessible.name`; contrast check over both themes; keyboard-only script passes |

CI (the `gui` job of `.github/workflows/ci.yml`, chapter 13 §13.7.1; chapter 13 owns the workflow files): models + QML units + interaction scripts + screenshots run on Linux (offscreen) per PR; macOS runs scripts natively; Android instrumentation on the emulator nightly; perf and real-device HDR on self-hosted runners nightly (chapter 14 §14.9.2: `android-hdr` device for HDR, Pixel Tablet is SDR).

---

## 11.10 Work items

All GUI WIs produce files under `app/` (or `android/`) and add tests to `app/tests/` as described in §11.9 (`app/tests/{models,qml,scripts,screenshots,a11y}/`; performance benchmarks under `tests/perf/gui/`). Every WI carries the §0.1 template lines (D15); "Interface" names the §/types it implements. "Fixture" = FlatBuffers/corpus fixtures from Chapters 6–8 test suites.

### Phase 1 — shell, docking, timeline, picture, syntax, hex, container, stream info, HDR panel skeleton

### WI-1.450 — Application skeleton, RHI selection, `Platform` singleton, settings
**Goal:** `ovt-studio` starts to an empty window with menus, toolbar and status bar on macOS and Linux.
**Overlap:** extends the `ovt_studio` target of WI-0.019 in place; do not recreate `app/CMakeLists.txt`. See chapter 16 §16.3.
**Depends on:** WI-0.001 (CMake presets), WI-0.018 (Qt 6.8.3 install), WI-0.019 (`ovt_studio` skeleton target, extended by this WI).
**Produces:** `app/CMakeLists.txt`, `app/src/main.cpp`, `app/qml/Main.qml`, `app/qml/controls/OvtStyle.qml`, `app/src/platform/platform.{h,cpp}`, `app/src/settings.{h,cpp}`, `docs/spec/schemas/settings.schema.json`, `app/tests/qml/tst_main.qml`, `app/tests/models/test_settings.cpp`.
**Interface:** §11.2.1, §11.2.3–11.2.5; `Platform` properties (`Platform::configDir()` resolves the §11.2.5 directories); `Settings` (JSON document in `Platform::configDir()`, `OVT_CONFIG_DIR` override).
**Steps:** 1. CMake target with `qt_add_qml_module`. 2. Backend selection per platform + `OVT_RHI`; `setHighDpiScaleFactorRoundingPolicy(PassThrough)` before the application object. 3. Menus/toolbar/status bar with disabled actions wired to a central `Actions` object. 4. Settings JSON (atomic write, schema validation, `GUI-SET-0001`). 5. Theme light/dark tokens.
**Acceptance tests:** 1. Given a built `ovt-studio`, When run with `--version`, Then it prints versions and exits 0. 2. Given `QT_QPA_PLATFORM=offscreen`, When `tst_main` runs, Then the window has all 8 menus and the status bar. 3. Given `OVT_CONFIG_DIR` pointing at an empty directory, When the theme is changed and the app restarted, Then `settings.json` validates against the schema and the theme is restored; given a corrupt file, When the app starts, Then defaults load and `GUI-SET-0001` is raised.
**Size:** M.
**Notes / pitfalls:** `QSettings` is not used anywhere in `app/` (D8); `Qt::AA_EnableHighDpiScaling` is a no-op in Qt 6 and must not appear.

### WI-1.451 — Dock manager (`SplitView`-based) with presets and serialisation
**Goal:** The dock model of §11.2.2 with drag/drop, tabs, presets, JSON persistence.
**Depends on:** WI-1.450.
**Produces:** `app/src/dock/dock_model.{h,cpp}`, `app/qml/dock/{DockArea,DockGroup,DockTabBar,DockDropIndicator}.qml`, `app/tests/models/test_dock_model.cpp`, `app/tests/qml/tst_dock.qml`.
**Interface:** `DockArea`, `DockGroup`, `Layout`; preset table.
**Steps:** 1. Model + JSON. 2. Areas with `SplitView`. 3. Tab drag (QML `DragHandler`) with drop indicators. 4. Presets. 5. Shortcuts `Ctrl+Shift+…`. 6. Floating windows (desktop) behind `Platform.isAndroid == false`.
**Acceptance tests:** 1. Given preset Analyze, When serialised and restored, Then the JSON is identical and the groups are equal. 2. Given preset Analyze, When a tab is dragged to the right edge, Then a new Right group exists with that panel.
**Size:** L.
**Notes / pitfalls:** floating windows must be compiled out, not just hidden, on Android (`Platform.isAndroid`); serialise `sizeFraction` with 4 decimals so round trips are byte-identical.

### WI-1.452 — `SelectionModel` and the derivation rules
**Goal:** §11.4.1 singleton with all derived fields and echo suppression.
**Depends on:** WI-1.450, WI-0.083 (generated FlatBuffers headers).
**Produces:** `app/src/models/selection_model.{h,cpp}`, `app/tests/models/test_selection_model.cpp`.
**Interface:** `SelectionState`, setters, `changed(mask)`; a small `SelectionIndex` interface (`unit`, `resolve_frame`, `first_vcl_unit`, `sample`, `unit_at_byte`) through which the model reads the index with app-side mirrors of `FrameId`/`ByteSpan`/`BitSpan` (the app links the generated FlatBuffers headers only from WI-1.455, which supplies the adapter over `fb::StreamIndex`; the adapter returns the sample span from the container sample table when present, else the union of the sample's units including prefixes); `setChainLength(fn(block_id) → n)` supplied by the Picture panel from the `FrameAnalysis` ("next" on the same leaf gives `(level + 1) % n`, on a new leaf 0; without a provider `n = 1`).
**Steps:** 1. Struct + Q_PROPERTYs. 2. Derivations (frame→unit/sample; unit→frame/bytes; field→bytes via EPB map; sample→bytes/frame) over `SelectionIndex`. 3. Clear rules + probe lock. 4. Tests for every table row.
**Acceptance tests:** 1. Given the `syn_h264_epb_dense.264` fixture (chapter 14 §14.3.3, EPBs inside SPS/PPS/slice-header fields) and its golden `tests/golden/epb/syn_h264_epb_dense.json` (field name → file byte offset and bit, produced by the FFmpeg `trace_headers` normaliser of chapter 14 §14.4.1), When `setField` is called for every golden field, Then `byte_range` equals the golden offset (until that golden exists — `trace_headers.py --epb-offsets` is produced by the WI that needs it, WI-1.458 — this WI ships the equivalent test against an independent emulation-prevention encoder over zero-run RBSPs). 2. Given a selected block, MV and probe with the lock off, When `setFrame` is called, Then `block_id`, `mv_index`, `probe` reset. 3. Given an analysed HEVC frame, When `setBlock(block_id, level=0)` is called and then two `setBlock` calls at the same leaf with `level = -1` (next), Then `level` reads 0, 1, 2 and wraps to 0 after the chain length.
**Size:** M.
**Notes / pitfalls:** derivations must never call the core synchronously beyond `StreamIndex` lookups; echo suppression keys on `origin`, not on value equality.

### WI-1.453 — Jobs model, progress UX and diagnostics model
**Goal:** `JobsModel`, `DiagnosticsModel`, status-bar progress, Jobs popover, Diagnostics/Log panel.
**Depends on:** WI-1.450, WI-1.512 (`ovt_Job` API).
**Produces:** `app/src/models/{jobs_model,diagnostics_model,log_sink}.{h,cpp}`, `app/qml/panels/DiagnosticsPanel.qml`, `app/qml/JobsPopover.qml`.
**Interface:** §11.7, §11.3.16; `DiagnosticsModel::NavigationSink`, a small abstract interface (`setUnit(unit_index, BitSpan)`, `setBox(path)`, `setFrameBlock(...)`, `setFrameProbe(FrameId, QPoint)`) that `SelectionModel` (WI-1.452, or a one-line follow-up in whichever lands second) implements; the location of a diagnostic is an app-side `DiagnosticLocation` filled by the producer of the entry (the `Diagnostic` table has `span` only).
**Steps:** 1. Job wrapper (name, progress, cancel) over the exported C `ovt_Job` API with the model's own `QThreadPool` (`ovt::jobs::Pool` is not reachable from the app). 2. Status bar + popover. 3. Diagnostics merge + jump-to via Selection. 4. Log ring buffer.
**Acceptance tests:** 1. Given a running job, When it reports progress, Then the status bar shows `%` within 100 ms. 2. Given a `Diagnostic` with a `BitSpan`, When double-clicked, Then the `NavigationSink` receives `setUnit(unit_index, bit_range)` (asserted on a fake sink; on `Selection.unit_index`/`bit_range` once `SelectionModel` implements the sink). 3. Given a `VQ-DIFF-0001` diagnostic, When double-clicked, Then the sink receives `setFrameProbe` with its fields (then `Selection.frame` and `Selection.probe`).
**Size:** M.
**Notes / pitfalls:** `GUI-*` codes come from the registry in §11.7; core codes are shown verbatim and never re-numbered.

### WI-1.454 — File open flow, project model and `.ovtproj`
**Goal:** Open/Recent/roles/second-file dialog, project save/load with schema validation and autosave.
**Depends on:** WI-1.452, WI-1.217 (`ovt_container_open` / `TrackInfo`), WI-1.121 (`ovt_stream_build_index`).
**Produces:** `app/src/project.{h,cpp}`, `docs/spec/schemas/ovtproj.schema.json`, `app/qml/dialogs/{OpenAsDialog,RawFormatDialog}.qml`, `app/tests/models/test_project.cpp`.
**Interface:** §11.2.6, §11.8 second-file dialog.
**Steps:** 1. Open → container → track choice (first video track default) → index job (`Project` records the file and emits `fileOpened(id)`; the container → track → index chain attaches to that signal once WI-1.217 and WI-1.121 land; `track_index` and `source_hash` are stored and validated). 2. Raw YUV dialog. 3. Roles (§11.8). 4. Project JSON + validation + relative paths; `recent_files.json` (§11.2.5). 5. Autosave/recovery. File chooser dialogs (`QtQuick.Dialogs`) are a follow-up after the CI image is checked; this WI covers the command line, drag-and-drop, Open-as and Raw format dialogs.
**Acceptance tests:** 1. Given a saved project with two files and bookmarks, When reopened, Then files, roles, layout, view and bookmarks are restored (test compares states). 2. Given a saved project whose file path no longer exists, When it is opened, Then the project opens with the file in error state (`GUI-PROJ-0003`). 3. Given a raw `.yuv` file, When the Raw format dialog changes width/height/bit depth, Then the preview frame updates within 200 ms and a size mismatch shows `GUI-RAW-0001`. 4. Given no running instance, When `ovt-studio clip.mp4` is run, Then the file opens; given that window, When a second file is dropped on it, Then the Open-as dialog of §11.8 appears.
**Size:** M.
**Notes / pitfalls:** store `content://` URIs verbatim on Android; relative paths only when the file is under the project directory.

### WI-1.455 — Timeline panel
**Goal:** §11.3.1 chart, series, range selection, GOP markers, filmstrip with reference arrows.
**Depends on:** WI-1.452, WI-1.453.
**Produces:** `app/src/items/timeline_chart.{h,cpp}` (`QQuickItem` + SG nodes), `app/src/models/{stream_index_model,thumbnail_cache}.{h,cpp}`, `app/qml/panels/TimelinePanel.qml`, tests.
**Interface:** `enum class Series : uint8_t { Bits, Qp, TemporalId, Metric }` with `struct SeriesRef { Series kind; QString metric_id; int plane; }`; `rangeChanged(first, last)`.
**Steps:** 1. Index model. 2. Bars + LOD. 3. Order toggle. 4. Range + stats line. 5. GOP markers. 6. Thumbnails job + filmstrip. 7. Ref arrows.
**Acceptance tests:** 1. Given a 10⁶-frame synthetic index, When zoomed to fit, Then paint ≤ 16 ms and bars equal per-pixel maxima (unit test on the aggregator). 2. Given `syn_h264_gop60_1080p`, When the timeline is dragged over frames 120–240, Then the status line bitrate equals Σbytes×8/Δpts with Δpts per §11.3.1 (range extent plus the last frame's presentation distance) in seconds from the track time base, exact to 1 tick.
**Size:** L.
**Notes / pitfalls:** LOD prefix tables are per series and rebuilt only when the index grows; thumbnails never block the chart.

### WI-1.456 — Picture panel (EMBEDDED mode): zoom/pan, planes, probe, selection rect
**Goal:** §11.3.2 without compare/overlay modes (added in WI-1.458, WI-2.481).
**Depends on:** WI-1.405 (`PictureHost`), WI-1.452.
**Produces:** `app/qml/panels/PicturePanel.qml`, `app/src/picture_controller.{h,cpp}` (ViewState ownership, input → ViewState/Selection), tests `tst_picture.qml`.
**Interface:** `ViewState`; gesture table §11.4.4.
**Steps:** 1. Playback decoder job per frame change (HW first). 2. Zoom/pan math and clamping. 3. Plane selector + stage combo. 4. Probe hover/pin. 5. Selection rect. 6. Status bar read-out.
**Acceptance tests:** 1. Given a frame at zoom 1, When the wheel turns at device (400,300) so zoom becomes 2, Then the image point under the cursor is unchanged (`pick()` before/after equal). 2. Given a frame shown, When the picture is `Ctrl+click`ed, Then `Selection.probe` is set and the crosshair drawn (screenshot). 3. Given the Picture focused, When keys `1`–`5` are pressed, Then `ViewState.plane_mode` reads 0–4.
**Size:** L.
**Notes / pitfalls:** one `render()` per vsync — coalesce wheel events; the wheel notch is `angleDelta == 120` (§11.4.4).

### WI-1.457 — Syntax tree panel
**Goal:** §11.3.4.
**Depends on:** WI-1.452, WI-1.130 (`ovt_stream_unit_syntax`), WI-1.108, WI-1.112, WI-1.116 (parsers).
**Produces:** `app/src/models/syntax_tree_model.{h,cpp}`, `app/src/models/syntax_filter_proxy.{h,cpp}`, `app/qml/panels/SyntaxPanel.qml`, tests.
**Interface:** `SyntaxNode` FlatBuffer; `Selection.setField`.
**Steps:** 1. Model over FlatBuffer. 2. Columns + hex/dec. 3. Filter off-thread. 4. Expand/collapse. 5. Diagnostics icons. 6. Context menu + JSON copy (via FlatBuffers reflection → JSON).
**Acceptance tests:** 1. Given the SPS of `syn_h264_epb_dense`, When it is selected, Then row count equals the node count and `name` texts match. 2. Given that SPS shown, When the filter "log2" is typed, Then only matching rows (and ancestors) remain within 200 ms for a 50 k-node tree. 3. Given the same SPS, When the pointer hovers a `ue(v)` node, Then the tooltip shows its `spec_ref`, its descriptor `ue(v)` and the one-sentence glossary definition from `docs/spec/02-glossary.md` (FR-SYN-017), and no tooltip text is longer than that sentence plus the two fields.
**Size:** M.
**Notes / pitfalls:** labels are `SyntaxNode.name` untranslated (FR-GUI-102); never materialise more than visible + 200 rows (FR-GUI-013).

### WI-1.458 — Hex view panel
**Goal:** §11.3.5 incl. bands, bit ribbon, goto, byte search, EPB mapping.
**Depends on:** WI-1.452, WI-1.101 (`ovt_file_read` C API), WI-1.234 (byte service).
**Produces:** `app/src/models/hex_model.{h,cpp}`, `app/qml/panels/HexPanel.qml`, `app/qml/controls/BitRibbon.qml`, tests `test_hex_model.cpp`.
**Interface:** `Selection.byte_range/bit_range`; search job.
**Steps:** 1. Paged model. 2. Bands from unit/box spans. 3. Field highlight incl. partial bytes + ribbon. 4. Goto parser. 5. Search job with wildcards. 6. Reverse lookup (byte → unit/box/field).
**Acceptance tests:** 1. Given a field spanning bits 17–54 of a unit with an EPB at raw offset 3, When it is selected, Then highlighted file offsets equal the oracle. 2. Given an Annex B file open, When `00 00 01` is searched, Then results equal the start-code positions of the index. 3. Given Hex focused, When `Ctrl+B` is pressed, Then the binary ribbon toggles and `B` still adds a bookmark.
**Size:** M.
**Notes / pitfalls:** EPB mapping is the normative algorithm of §11.3.5; search runs over the mmap in a job.

### WI-1.459 — Unit list panel
**Goal:** §11.3.6.
**Depends on:** WI-1.455 (index model).
**Produces:** `app/src/models/unit_list_model.{h,cpp}`, `app/qml/panels/UnitListPanel.qml`, `app/tests/models/test_unit_list_model.cpp`.
**Interface:** §11.3.6 columns; `Selection.setUnit`.
**Steps:** 1. Proxy model over the index model. 2. Type filter. 3. Double-click navigation.
**Acceptance tests:** 1. Given `syn_sei_h264_ga94_cc` open, When the filter "SEI" is typed, Then only SEI units are listed. 2. Given the unit list, When a VCL unit is double-clicked, Then `Selection.frame` changes to its frame.
**Size:** S.
**Notes / pitfalls:** unit types are shown with their spec names (`nal_unit_type` names, OBU type names).

### WI-1.460 — Container tree panel with derived tabs
**Goal:** §11.3.7.
**Depends on:** WI-1.452, WI-1.218, WI-1.219, WI-1.220, WI-1.221, WI-1.223 (container derived views).
**Produces:** `app/src/models/{container_tree_model,field_table_model,sample_table_model,edit_list_model,fragment_model,encryption_model}.{h,cpp}`, `app/qml/panels/ContainerPanel.qml`, `app/qml/panels/container/{Tracks,Samples,EditList,Fragments,Encryption,Hdr}Tab.qml`, `app/tests/models/test_container_tree_model.cpp`.
**Interface:** §11.3.7; `ovt_container_expand`; `Selection.setBytes/setSample/setField`.
**Steps:** 1. Lazy tree with `ovt_container_expand`. 2. Field table. 3. Tabs Tracks/Samples/Edit list/Fragments/Encryption/HDR. 4. Hex links. 5. Validate action.
**Acceptance tests:** 1. Given `syn_mp4_frag_cmaf` (chapter 14 §14.3.3 recipe 21), When the Container panel opens, Then Fragments lists every `moof` with correct base decode times. 2. Given the Container tree, When `tkhd.duration` is clicked, Then Hex highlights the field's bytes.
**Size:** L.
**Notes / pitfalls:** expansion is lazy (FR-GUI-013 analogue for boxes); never walk the whole `mdat`.

### WI-1.461 — Stream Info panel
**Goal:** §11.3.8 summary.
**Depends on:** WI-1.455, WI-1.460.
**Produces:** `app/src/models/stream_summary.{h,cpp}`, `app/qml/panels/StreamInfoPanel.qml`, `app/tests/models/test_stream_summary.cpp`.
**Interface:** §11.3.8 rows; reads `StreamIndex`, `ContainerTree`, `HdrMetadata` only.
**Steps:** 1. Summary model. 2. Panel. 3. Copy as JSON.
**Acceptance tests:** 1. Given the `syn_h265_main10_pq_hdr.mp4` fixture, When the Stream Info panel opens, Then the panel shows `Main 10 @ L5.1`, `4:2:0 10-bit`, `HDR10`, frame counts matching `ovt index --format json`.
**Size:** S.
**Notes / pitfalls:** every derived value (bitrate, duration) is computed by the core summary API, not in QML.

### WI-1.462 — HDR panel skeleton (Metadata + Display tabs) and Display Capabilities panel
**Goal:** §11.3.14 Metadata and Display tabs; §11.3.15.
**Depends on:** WI-1.456, WI-1.407, WI-1.411.
**Produces:** `app/src/models/hdr_metadata_model.{h,cpp}`, `app/qml/panels/{HdrPanel,DisplayCapsPanel}.qml`, `app/tests/qml/tst_HdrMetadataModel.qml`.
**Interface:** §11.3.14 Metadata/Measurements/Display tabs, §11.3.15; `DisplayCaps` (chapter 10 §10.4.5).
**Steps:** 1. Merge container/SEI/OBU metadata with provenance + consistency diagnostics. 2. Tone-map/gamut/clip controls bound to `ViewState`. 3. Dolby label. 4. DisplayCaps table + re-probe + test pattern.
**Acceptance tests:** 1. Given `syn_h265_main10_pq_hdr_mismatch.mp4` (`colr` and VUI disagree), When the HDR panel's Metadata tab opens, Then it shows an ⚠ row with the core's `DEC-HDR-0001` diagnostic. 2. Given an SDR display, When the HDR panel opens, Then the HDR output toggle is disabled with the reason tooltip. 3. Given a stream declaring MaxCLL 1000, When a range scan measures 1200, Then the verdict row shows ⚠ `VQ-HDR-0002` (chapter 9 §9.11.4) equal to `ovt hdr --measure --compare-declared` (chapter 12 §12.2.11).
**Size:** M.
**Notes / pitfalls:** the consistency and declared-vs-measured verdicts come from the core (chapter 8/9/12 diagnostics); the model only displays them.

### WI-1.463 — HDR output toggle and NATIVE-mode host (desktop)
**Goal:** Picture panel creates/positions the child `QWindow` and calls `retarget()`.
**Overlap:** phase-1 scope is the macOS NATIVE host only; the Linux host is added by WI-4.450. See chapter 16 §16.3.
**Depends on:** WI-1.406 (macOS native target), WI-1.456. (The Linux host arrives in phase 4 with the Linux HDR work item and is not a dependency.)
**Produces:** `app/src/native_surface_host.{h,cpp}`, `app/src/platform/mac_native_host.mm` (the `CAMetalLayer` target itself is the chapter 10 WI-1.406 file in `render/src/platform/`), `app/tests/models/test_native_surface_host.cpp`.
**Interface:** `NativeSurfaceHost { retarget(), setGeometry(QRect), setVisible(bool) }`; `VideoSurface::retarget` (chapter 10 §10.2.2).
**Steps:** 1. Child window lifecycle. 2. Geometry sync. 3. Popup rule (desktop: `Popup.popupType = Popup.Window` where supported, else hide sibling). 4. HDR monitor window.
**Acceptance tests:** 1. Given an XDR Mac, When `Ctrl+H`, Then `active_encoding() == ScRgbLinear16F` and the child window rect equals the item rect ±1 px during a 100-step splitter drag.
**Size:** M.
**Notes / pitfalls:** geometry must be mapped through `Window.contentItem` and `devicePixelRatio` on every change (§11.2.1); the popup rule (§11.5.5 item 3) applies on desktop too where `Popup.Window` is unavailable.

### WI-1.464 — Welcome page, empty states, first-run banners
**Goal:** §11.8.
**Depends on:** WI-1.454.
**Produces:** `app/qml/WelcomePage.qml`, `app/qml/controls/EmptyState.qml`, strings.
**Interface:** §11.8 texts; `Platform.isFirstRun`.
**Steps:** 1. Welcome page with recent list and consent dialog. 2. `EmptyState` control used by all panels. 3. First-run banners.
**Acceptance tests:** 1. Given a fresh profile, When the app starts, Then the welcome page and privacy banner appear (scene `welcome` of the chapter 14 §14.10.2 harness, both themes).
**Size:** S.
**Notes / pitfalls:** corpora downloads need the explicit consent dialog (licence shown) every time a new manifest entry is fetched.

### WI-1.465 — Interaction script runner, screenshot harness and CI wiring
**Goal:** the in-app test IPC server of chapter 14 §14.10.3, the stub renderer switch, and the GUI CI workflow.
**Overlap:** this WI delivers the in-app `--test-ipc` server (`app/src/testipc/`), `OVT_RENDER_STUB=1` and the `gui` job of `.github/workflows/ci.yml`; the interaction scripts, the Python client and the screenshot harness are WI-0.568/WI-1.576 (no `script_runner`). See chapter 16 §16.3.
**Depends on:** WI-1.456, WI-0.568 (screenshot diff tool and GUI scaffolding).
**Produces:** `app/src/testipc/{test_ipc_server,command_dispatch}.{h,cpp}`, `app/src/render_stub.cpp` (`OVT_RENDER_STUB=1`), the `gui` job added to `.github/workflows/ci.yml` (file owned by chapter 13 WI-0.031; this WI contributes the job definition).
**Interface:** chapter 14 §14.10.3 command set (`open`, `click`, `select-syntax-node`, `key`, `get`, `screenshot`, `wait-idle`) and its request/response JSON shape (`{"id","cmd","args"}` → `{"id","ok","value"|"error"}`; issue #353), JSON lines over a Unix domain socket (`QLocalServer`), enabled only with `--test-ipc <socket>`.
**Steps:** 1. Socket server on the GUI thread (`QLocalServer`). 2. Command dispatch to `objectName` lookups (`QQuickItem::mapToScene` for click coordinates). 3. `wait-idle` = no running jobs and no pending render. 4. Stub renderer. 5. Workflow job wiring (`ctest -L gui`).
**Acceptance tests:** 1. Given CI on Linux offscreen, When the GUI job runs, Then the five required scripts of §14.10.3 and the §11.9 scripts pass and screenshots match within the §14.10.2 tolerance. 2. Given no `--test-ipc` flag, When the app starts, Then no socket exists.
**Size:** M.
**Notes / pitfalls:** the IPC is GPL app code and must not be compiled into release builds (`OVT_BUILD_TESTS`); `QTest::mouseClick` on QML items needs scene coordinates, never item-local ones.

### WI-1.466 — Keyboard map, Go to dialog, Preferences dialog
**Goal:** §11.4.3 map (editable), `Ctrl+G`, Preferences groups.
**Depends on:** WI-1.450.
**Produces:** `app/src/keymap.{h,cpp}`, `app/qml/dialogs/{GotoDialog,PreferencesDialog,KeymapDialog}.qml`, `app/tests/models/test_keymap.cpp`, `app/tests/qml/tst_Shortcuts.qml`.
**Interface:** §11.4.3 map (actions `action.<group>.<name>`), `keymap.json`, `GUI-KEY-0001`; Go to grammar of FR-GUI-006 (`d123`, `123`/`p123`, `poc45`, `12.5s`, `00:01:30.250`, `@0x1A2B3C`, `@1715004`, plus `unit:N`).
**Steps:** 1. Action registry and default map. 2. JSON overrides with conflict detection. 3. Go to parser. 4. Preferences groups bound to `Settings`. 5. Keymap dialog.
**Acceptance tests:** 1. Given `keymap.json` rebinding `action.navigate.next` to `J`, When `J` is pressed, Then a frame advances and the Keyboard map dialog shows it; given a binding already used in the same scope (`L` while the legend toggle keeps it), When the file loads, Then `GUI-KEY-0001` and the conflicting entry is rejected. 2. Given a file open, When `Ctrl+G` "@0x1A3F40" is entered, Then Hex scrolls there and the containing unit is selected; given no frame with POC 9999, When `poc9999` is entered, Then "no frame" and the current frame is unchanged. 3. Given `tst_Shortcuts.qml`, When it sends each key of the §11.4.3 table, Then every row triggers its action exactly once.
**Size:** S.
**Notes / pitfalls:** the §11.4.3 table is the only source of defaults; chapter 3/4 bindings are not consulted.

### Phase 2 — selection info, overlay modes, block samples, ref graph, statistics

### WI-2.480 — Analysis orchestration and `FrameAnalysisStore`
**Goal:** GUI-side LRU of `FrameAnalysis` per frame with GOP-local analysis jobs and the "analysis pending" affordance.
**Depends on:** WI-2.150, WI-2.151, WI-2.152 (plugin host and analysis session), WI-1.453.
**Produces:** `app/src/models/frame_analysis_store.{h,cpp}`, `app/qml/controls/AnalysisPendingBand.qml`, `app/tests/models/test_frame_analysis_store.cpp`.
**Interface:** `FrameAnalysisStore { request(FrameId, stream), get(FrameId) -> const FrameAnalysis*, cancelAll() }`; signal `arrived(FrameId)`.
**Steps:** 1. Request/cancel per frame; coalesce scrub requests (200 ms). 2. LRU 64 grids (§0.7.3). 3. Pending band + ETA. 4. Auto-analyse preference.
**Acceptance tests:** 1. Given a file open, When 30 frames are sought in rapid succession, Then at most 2 analysis jobs are started (last two). 2. Given an unanalysed frame, When `A` is pressed, Then the band shows progress and disappears when the FlatBuffer arrives.
**Size:** M.
**Notes / pitfalls:** the store is per stream (A and B have separate stores in Dual view, WI-2.489); the LRU cap counts grids, not frames.

### WI-2.481 — Overlay modes F1–F12, toggles, Legend panel
**Goal:** §11.4.2 presets → `OverlayConfig`; §11.3.3 Legend.
**Depends on:** WI-2.480, WI-2.431, WI-2.432, WI-2.433, WI-2.434, WI-2.435, WI-2.436, WI-2.439.
**Produces:** `app/src/overlay_modes.{h,cpp}`, `app/qml/panels/LegendPanel.qml`, `app/tests/models/test_overlay_modes.cpp`.
**Interface:** §11.4.2 table → `OverlayConfig` (chapter 10 §10.2.3); `ovt_render_legend`.
**Steps:** 1. Preset table. 2. Letter toggles with per-mode persistence. 3. Legend panel bound to `ovt_render_legend`. 4. Highlight-class hover.
**Acceptance tests:** 1. Given an analysed frame, When `F3` then `M` are pressed, Then `layers` has no `Mv*` bits, and When `M` is pressed again, Then they are restored. 2. Given an analysed HEVC or AV1 frame, When `F5` is pressed, Then the legend shows the QP gradient 0–51 for HEVC and 0–255 for AV1.
**Size:** M.
**Notes / pitfalls:** presets are data, not code paths; a mode the plugin cannot serve shows "n/a (plugin)" instead of an empty picture.

### WI-2.482 — Selection Info panel with reflection-based extension display and level cycling
**Goal:** §11.3.9.
**Depends on:** WI-2.480, WI-0.083 (`ovt_model.bfbs` resource).
**Produces:** `app/src/models/selection_info_model.{h,cpp}`, `app/src/fb_reflection.{h,cpp}`, `app/qml/panels/SelectionInfoPanel.qml`, `app/tests/models/test_selection_info_model.cpp`.
**Interface:** §11.3.9 sections; `SelectionModel.level` cycle rule (first click → level 0).
**Steps:** 1. Cycle chains per codec. 2. Cell/Block/ext/MV sections. 3. Reflection decode of `ext` by `ext_type`. 4. Probe section. 5. Follow-MV action.
**Acceptance tests:** 1. Given an analysed frame of `syn_h265_gop60_1080p`, When a CU is selected, Then every `HevcCu` schema field appears with its name and value. 2. Given no selection, When the same point is clicked three times, Then `level` reads 0, 1, 2 and the breadcrumb header reads CTU, CU, PU in turn; a fourth click reads TU and a fifth wraps to CTU.
**Size:** M.
**Notes / pitfalls:** reflection uses the `ovt_model.bfbs` resource of WI-0.083, so new `ext` tables appear without GUI changes.

### WI-2.483 — Block Samples panel
**Goal:** §11.3.10.
**Depends on:** WI-2.482, WI-2.523 (`ovt_analysis_plane`), WI-2.158, WI-2.166 (plane export by the plugins).
**Produces:** `app/src/models/block_grid_model.{h,cpp}`, `app/qml/panels/BlockSamplesPanel.qml`, `app/tests/models/test_block_grid_model.cpp`.
**Interface:** §11.3.10; `ovt_analysis_plane` (`PlaneStage`).
**Steps:** 1. Grid model over plane blobs. 2. Stage/plane combos. 3. Hex/dec and signed display. 4. Compare-with-B column.
**Acceptance tests:** 1. Given a 32×32 CU selected, When the Residual stage is chosen, Then the grid shows 1024 signed values equal to the plane blob at the rect (test reads the blob directly). 2. Given that grid, When the hex toggle is set, Then values render as `0x…`.
**Size:** M.
**Notes / pitfalls:** residual values are signed; mid-grey = 0 only in the Picture, the grid shows raw signed integers.

### WI-2.484 — Reference / GOP graph panel
**Goal:** §11.3.11.
**Depends on:** WI-1.455.
**Produces:** `app/src/items/ref_graph.{h,cpp}`, `app/qml/panels/RefGraphPanel.qml`, `app/tests/models/test_ref_graph_layout.cpp`.
**Interface:** §11.3.11; `StreamIndex.frames[].declared_refs`, `temporal_id`.
**Steps:** 1. Layout by temporal id. 2. Arrows. 3. DPB view. 4. Click navigation.
**Acceptance tests:** 1. Given `syn_h265_bpyramid` (chapter 14 §14.3.3 recipe 43), When the Reference graph panel opens, Then rows by temporal id match `temporal_id` and arrows equal `declared_refs`. 2. Given the same fixture, When a node is clicked, Then `Selection.frame` changes to that node's frame.
**Size:** M.
**Notes / pitfalls:** `declared_refs` come from the index (headers), not from decoded data; mark frames whose refs are unknown.

### WI-2.485 — Statistics panel (Qt Graphs 2D)
**Goal:** §11.3.12.
**Depends on:** WI-2.480.
**Produces:** `app/src/models/stats_aggregator.{h,cpp}`, `app/qml/panels/StatisticsPanel.qml`, `app/tests/models/test_stats_aggregator.cpp`.
**Interface:** §11.3.12; scope {Frame, Range, Selection}; CSV via the core exporter of WI-0.089.
**Steps:** 1. Aggregator over `FrameAnalysis` grids. 2. Charts (Qt Graphs 2D). 3. Scope handling and incremental analysis. 4. Export.
**Acceptance tests:** 1. Given an analysed frame of `syn_h265_gop60_1080p`, When the Statistics panel shows it, Then "Pred kind by area" percentages equal the per-cell counts/total (unit test). 2. Given 10 frames of which 4 are unanalysed, When the scope is set to Range over them, Then analysis jobs run and the chart updates incrementally.
**Size:** M.
**Notes / pitfalls:** percentages are by area (cells), not by block count, unless the chart says so.

### WI-2.486 — Compare modes UI and B-side selection
**Goal:** Compare combo, divider drag, gain, A/B swap, `SelectionB`, memory snapshot ("Set as B").
**Depends on:** WI-1.410, WI-1.456.
**Produces:** `app/src/compare_controller.{h,cpp}`, QML in `app/qml/panels/PicturePanel.qml`, `app/tests/scripts/test_compare_modes.py`.
**Interface:** `CompareMode` (chapter 10 §10.2.4); `SelectionB` (§11.4.1).
**Steps:** 1. Mode cycling and combo. 2. Divider/alpha/gain controls. 3. `SelectionB` derivation including the manual-offset rule (§11.4.1). 4. Memory snapshot. 5. "Find first mismatch" toolbar action (FR-PLN-009): run the core compare job (chapter 9 §9.3.3) as a cancellable job, post `VQ-DIFF-0001`, and jump to the frame with the probe pinned at (x, y).
**Acceptance tests:** 1. Given A and B open, When `C` is pressed 6 times, Then modes cycle in the documented order (script). 2. Given Split mode, When the divider is dragged to 25 %, Then `CompareMode.split_position == 0.25 ± 0.01`. 3. Given `src_1080p_8bit` as A and `syn_metrics_1080p_bitflip_f37` as B (chapter 14 §14.3.3 recipe 52), When "Find first mismatch" runs, Then `VQ-DIFF-0001` reports frame 37, plane Y, (611, 203) with the values recorded in `tests/corpora/compare_expected.json`, `Selection.frame.display_index == 37` and the probe is pinned at (611, 203). 4. Given a manual offset of +3 and B shorter than A by 2 frames, When A's last frame is selected, Then `SelectionB` is empty and Selection Info's B column shows `—`.
**Size:** M.
**Notes / pitfalls:** compare modes are single-surface only; entering Dual view (WI-2.489) resets the mode to `Single`.

### WI-2.487 — Bookmarks & Comments panel
**Goal:** §11.3.17 with anchors and timeline markers.
**Depends on:** WI-1.454, WI-1.455.
**Produces:** `app/src/models/bookmarks_model.{h,cpp}`, `app/qml/panels/BookmarksPanel.qml`, `app/tests/models/test_bookmarks_model.cpp`.
**Interface:** §11.3.17; `.ovtproj` `bookmarks[]`/`comments[]`.
**Steps:** 1. Models over the project. 2. Anchors → `SelectionModel`. 3. Timeline markers. 4. Import/export.
**Acceptance tests:** 1. Given a comment anchored to a field, When clicked, Then Syntax and Hex select it. 2. Given three bookmarks, When `N` is pressed three times, Then bookmarks cycle in frame order.
**Size:** S.
**Notes / pitfalls:** anchors reference `decode_index`, which is stable across order toggles.

### WI-2.488 — Export menu (PNG/PGM/Y4M/CSV/JSON/unit bytes) and Copy-as-JSON everywhere
**Goal:** File ▸ Export items wired to Chapter 10 exports and core exporters; `Ctrl+Shift+C` in all panels.
**Depends on:** WI-1.412 (PNG export), WI-0.088 (JSON exporter), WI-0.089 (YUView CSV exporter), WI-1.508 (`ovt extract` unit bytes), WI-2.485.
**Produces:** `app/src/export_actions.{h,cpp}`, `app/qml/dialogs/ExportDialog.qml`, `app/tests/models/test_export_actions.cpp`.
**Interface:** File ▸ Export items (§11.2.3); core exporters (`ovt_render_export_png`, `ovt_metrics_export_*`, JSON/CSV exporters of WI-0.088/WI-0.089).
**Steps:** 1. Export dialog. 2. Per-item wiring. 3. Copy-as-JSON in every panel.
**Acceptance tests:** 1. Given an HDR frame shown, When Export ▸ Frame as HDR PNG runs, Then the file validates (cICP present). 2. Given the Statistics panel, When CSV export runs, Then columns equal the panel table.
**Size:** S.
**Notes / pitfalls:** the GUI never formats numbers for export itself; all exports call core exporters so CLI and GUI files are byte-identical.

### WI-2.489 — Dual view (two `VideoSurface` instances, synchronised and unlockable)
**Goal:** FR-GUI-021: two `PictureHost`/`VideoSurface` instances for slots A and B in one panel, horizontal or vertical, with frame, zoom, pan, plane mode and overlay mode synchronised by default and individually unlockable; B shows its own `FrameAnalysis` overlays.
**Depends on:** WI-2.486 (compare controller, `SelectionB`), WI-2.481 (overlay modes), WI-1.405 (`PictureHost`).
**Produces:** `app/src/dual_view_controller.{h,cpp}`, `app/qml/panels/DualViewPanel.qml`, `app/qml/controls/SyncLockRow.qml`, `app/tests/models/test_dual_view_controller.cpp`, `app/tests/scripts/test_dual_view.py`.
**Interface:** §11.3.2 "Dual view"; `DualViewController { arrangement, locks{frame,zoom,pan,plane,overlay}, viewStateA/B, overlayA/B }`; `panel_state.dual`.
**Steps:** 1. Second `PictureHost` + `VideoSurface` (EMBEDDED) and second `FrameAnalysisStore`. 2. Sync set propagation with per-property locks. 3. Padlock row and arrangement toggle (`Ctrl+D`). 4. Mutual exclusion with compare modes. 5. Persistence in `panel_state`.
**Acceptance tests:** 1. Given `syn_metrics_1080p_pair` (chapter 14 §14.3.3) open in Dual view, When the left surface is panned by (100, 50) device px, Then the right surface's `ViewState.pan` changes identically; When "unlock pan" is enabled and the left surface is panned again, Then the right `pan` is unchanged (FR-GUI-021 acceptance). 2. Given Dual view with overlay unlocked, When `F3` is set on A and `F5` on B, Then the two surfaces show different overlays from their own `FrameAnalysis`. 3. Given Split compare active, When Dual view is enabled, Then `CompareMode == Single` and the Compare combo is disabled.
**Size:** M.
**Notes / pitfalls:** two surfaces double GPU memory — B uses the same decoder cache limits (§0.7.3); NATIVE HDR applies to A only; keep one `render()` per vsync for both surfaces.

### Phase 3 — metrics panel

### WI-3.450 — Metrics job dialog and results model
**Goal:** §11.3.13 job setup + per-frame table/pooled values.
**Depends on:** WI-3.316 (map store and C API for the GUI), WI-3.300, WI-1.455.
**Produces:** `app/src/models/metrics_model.{h,cpp}`, `app/qml/panels/MetricsPanel.qml`, `app/qml/dialogs/MetricsJobDialog.qml`, `app/tests/qml/tst_MetricsModel.qml`, `app/tests/scripts/test_metrics_job.py`.
**Interface:** §11.3.13; `ovt_metrics_job_*` and `ovt_metrics_export_*` (chapter 9 §9.10); defaults from `NormalizationConfig` (chapter 9 §9.2.4).
**Steps:** 1. Job dialog bound to `MetricDescriptor` list and core defaults. 2. Job wrapper with per-frame delivery. 3. Table/pooled models. 4. Comparison project (run sets, per-file progress). 5. First-mismatch action (`VQ-DIFF-0001`). 6. Export via core exporters.
**Acceptance tests:** 1. Given the `syn_metrics_1080p_pair` fixtures, When PSNR+VMAF run, Then the table values equal `ovt metrics -r … -d … -m psnr,vmaf --format json` for every frame (exact). 2. Given a running metrics job, When it is cancelled mid-run, Then partial results stay with the badge. 3. Given factory settings, When the dialog opens, Then every normalisation control equals the core default (`scale.kernel == bicubic`, `bit_depth == promote_lower`).
**Size:** L.
**Notes / pitfalls:** no metric math, pooling or normalisation in `app/` — the GUI only drives the chapter 9 API.

### WI-3.451 — Metrics plot linked to the timeline and heatmap overlay
**Goal:** Plot series via `TimelineChart`, click-to-seek, `HeatmapSource::MetricMap` plumbing with range control in the Legend.
**Depends on:** WI-3.450, WI-2.481.
**Produces:** `app/src/metric_map_bridge.{h,cpp}`, QML in `app/qml/panels/MetricsPanel.qml`, `app/tests/models/test_metric_map_bridge.cpp`.
**Interface:** `HeatmapSource::MetricMap` (chapter 10 §10.2.3); `ovt_metrics_map` (chapter 9 §9.10); `TimelineChart` `Series::Metric`.
**Steps:** 1. Series registration with the timeline. 2. Click-to-seek. 3. Map bridge (blob → cells). 4. Legend range control.
**Acceptance tests:** 1. Given SSIM maps, When `F10`, Then the Picture heatmap cell values equal the map blob resampled to cells (unit test on the bridge). 2. Given a metrics plot, When it is clicked at frame 77, Then `Selection.frame.display_index == 77`. 3. Given a finished metrics run, When the frame-size series is enabled (FR-VQ-016), Then the plot shows frame size on a secondary axis and a cumulative mean line.
**Size:** M.
**Notes / pitfalls:** map blobs are per frame and per metric; resample in C++ once per frame change.

### WI-3.452 — BD-rate tab and run sets
**Goal:** Sets of runs, anchor selection, BD-rate table via core.
**Depends on:** WI-3.450, WI-3.313 (BD-rate API).
**Produces:** `app/src/models/bdrate_model.{h,cpp}`, `app/qml/panels/metrics/BdRateTab.qml`, `app/tests/models/test_bdrate_model.cpp`.
**Interface:** §11.3.13 BD-rate tab and RD plot; `ovt_metrics_bd` (chapter 9 §9.8.4) with the `ovt bdrate` options of chapter 12 (`--pool`, `--interpolator`, `--range`).
**Steps:** 1. Run sets. 2. RD plot. 3. BD-rate table with warnings (`VQ-BD-*`). 4. Export.
**Acceptance tests:** 1. Given two sets of 4 runs each, When the BD-rate tab computes, Then BD-rate equals `ovt bdrate` output exactly; given a set of 3 points, When it is computed, Then the `VQ-BD-0002` warning row appears and `--interpolator cubic` is disabled.
**Size:** S.
**Notes / pitfalls:** the RD bit rate is `Σ size_bytes × 8 / container duration`, computed by the core summary API.

### Phase 4 — Buffer panel

### WI-4.470 — HRD buffer graph panel (Buffer panel)
**Goal:** §11.3.20 Buffer (HRD) panel realising FR-GUI-032: CPB fullness curve, cursor synchronised to the current frame, fullness in bits and % of `CpbSize`, underflow/overflow marks. Added under the D28 procedure (chapter 16 §16.2, sub-range 4.470–4.479); phase 4, milestone M4.3.
**Depends on:** WI-4.112 (HRD model and `HrdTrace`), WI-1.455 (timeline cursor), WI-2.483 (panel pattern for per-frame grids and Qt Graphs use).
**Produces:** `app/src/models/HrdModel.{h,cpp}`, `app/qml/panels/BufferPanel.qml`, `app/tests/models/test_hrd_model.cpp`, `app/tests/qml/tst_BufferPanel.qml`, registration of the panel in the View menu and in the dock presets (§11.2.2).
**Interface:** `ovt_index_hrd_trace(ovt_Stream*, const ovt_HrdOptions*, ovt_Buffer*)` → `HrdTrace`/`HrdPoint` (chapter 7 §7.8, FR-DEC-013; schema chapter 6 §6.3); diagnostics `DEC-HRD-0010` (overflow) and `DEC-HRD-0011` (underflow); `Selection.frame` (§11.4.1); the `ovt hrd --format json` output of chapter 12 as the oracle.
**Steps:** 1. `HrdModel` runs `ovt_index_hrd_trace` as a job and exposes points, `cpb_size_bits` and events. 2. Chart with the CPB-size line, cursor and red event markers. 3. Header controls (`sched_sel_idx`, NAL/VCL) re-run the trace. 4. Cursor/click synchronisation with `Selection.frame`. 5. Empty state for streams without HRD parameters.
**Acceptance tests:** 1. Given `syn_h264_hrd_cbr` (chapter 14 §14.3.3 recipe 65) at frame 100, When the Buffer panel is open, Then the cursor is at frame 100 and the shown fullness in bits equals `ovt hrd --format json | jq .frames[100].fullness_bits` and the percentage equals that value / `cpb_size_bits` × 100 rounded to 0.1 (FR-GUI-032 acceptance). 2. Given `jctvc_HRD_A_Fujitsu_3`, When the panel opens, Then the curve has one point per access unit and no underflow/overflow marker appears. 3. Given the patched copy of `jctvc_HRD_A_Fujitsu_3` with halved `bit_rate_value_minus1` (chapter 14 §14.11 phase 4), When the panel opens, Then a red underflow marker appears at the first access unit for which `ovt hrd` reports `DEC-HRD-0011`. 4. Given a stream without HRD parameters (`syn_h264_gop60_1080p`, chapter 14 §14.3.3 recipe 26), When the panel opens, Then it shows "No HRD parameters in this stream" and no job error.
**Size:** M.
**Notes / pitfalls:** the trace is integer 90 kHz arithmetic in the core; the panel only converts to seconds for the x axis and never recomputes fullness; one trace per (`sched_sel_idx`, NAL/VCL) pair is cached.

### Phase A — Android shell, layouts, SAF

### WI-A.450 — Gradle project with `QtQuickView` embedding
**Goal:** The desktop QML runs inside the Kotlin Activity of WI-0.050 with a process-lifetime `OvtSession` and the §11.5.1 manifest entries.
**Overlap:** this WI delivers only `OvtSession.kt` and the §11.5.1 manifest entries on top of the Gradle project and `MainActivity.kt` of WI-0.050 (primary); the ChromeOS free-form launch test is WI-0.054. See chapter 16 §16.3.
**Depends on:** WI-1.450, WI-0.050 (Android Gradle project and `MainActivity.kt`).
**Produces:** `android/app/src/main/kotlin/org/ovt/OvtSession.kt`, manifest additions in `android/app/src/main/AndroidManifest.xml` per §11.5.1 (intent filters, `configChanges`, free-form metadata, `colorMode`), `android/app/src/androidTest/java/org/ovt/SessionTest.kt`.
**Interface:** `OvtSession` (singleton holding `ovt_File/ovt_Container/ovt_Stream` handles and caches across Activity recreation, §11.5.7); manifest keys of §11.5.1.
**Steps:** 1. `OvtSession` with JNI binding to the core handles. 2. Manifest entries. 3. Re-bind on `onCreate`. 4. Instrumentation test.
**Acceptance tests:** 1. Given the APK from `./gradlew -p android :app:assembleFossDebug`, When it is installed with `:app:installFossDebug` on the x86_64 API 36 emulator and launched, Then the welcome page shows. 2. Given the app running, When an `ACTION_VIEW` intent with a `video/mp4` URI arrives, Then the file opens in the running Activity without recreation. 3. Given a file open and "Don't keep activities" enabled, When the Activity is recreated, Then `OvtSession` keeps the open handles (same `source_hash`, no re-index).
**Size:** M.
**Notes / pitfalls:** `targetSdk 36`, `minSdk 28` are pinned in chapter 13; do not duplicate Gradle files owned by WI-0.050.

### WI-A.451 — `Platform` singleton on Android and breakpoint layouts
**Goal:** §11.5.2 layouts switching live on resize.
**Depends on:** WI-A.450, WI-1.451.
**Produces:** `app/src/platform/platform_android.cpp`, `app/qml/dock/CompactLayout.qml`, `app/tests/qml/tst_breakpoints.qml`, `app/tests/scripts/test_android_breakpoints.py`.
**Interface:** `Platform.widthDp/hasHover/windowingMode/isAndroid`; §11.5.2 breakpoint table.
**Steps:** 1. `Platform` values from `Configuration`/`InputDevice`. 2. Compact layouts. 3. Drawer and bottom navigation. 4. State hand-over on switch.
**Acceptance tests:** 1. Given a file open, When the window is resized 1300 → 900 → 700 dp, Then layouts switch three-pane → two-pane → vertical without losing `Selection.frame`.
**Size:** M.
**Notes / pitfalls:** breakpoints use smallest-width dp from `Configuration.screenWidthDp` of the window, not the display.

### WI-A.452 — Storage Access Framework open/recent/export
**Goal:** §11.5.6.
**Overlap:** this WI delivers only the GUI flows (recent list, export, `GUI-AND-0001` copy action, Espresso tests) over `FilePicker.kt` and `ovt_jni_open_fd` of WI-0.051; no `Files.kt` and no `file_fd_bridge.cpp`. See chapter 16 §16.3.
**Depends on:** WI-A.450, WI-1.454, WI-0.051 (`FilePicker.kt`, `ovt_jni_open_fd`).
**Produces:** `android/app/src/main/kotlin/org/ovt/RecentFiles.kt`, `android/app/src/main/kotlin/org/ovt/ExportDocument.kt`, `android/app/src/main/kotlin/org/ovt/CopyToAppStorage.kt`, `android/app/src/androidTest/java/org/ovt/SafFlowsTest.kt` (with a test `DocumentsProvider`).
**Interface:** §11.5.6 flows; `GUI-AND-0001`; `ACTION_CREATE_DOCUMENT` export; recent list entries {uri, display name, codec}.
**Steps:** 1. Recent list over persisted URIs. 2. Non-seekable detection (`fstat` on the fd) → `GUI-AND-0001` + copy action streaming to `getExternalFilesDir`. 3. Export via `ACTION_CREATE_DOCUMENT`. 4. Instrumentation tests.
**Acceptance tests:** 1. Given a document URI, When opened, Then `ovt_file_open_fd` succeeds and the recent list shows its display name; after process restart the URI reopens. 2. Given a non-seekable provider, When a document is opened, Then `GUI-AND-0001` is raised with the copy action, and after the copy the file opens.
**Size:** M.
**Notes / pitfalls:** never read the whole file to detect seekability; persisted URI permissions are capped by the platform (128/512), evict oldest.

### WI-A.453 — Touch, hover, right-click and keyboard parity
**Goal:** §11.4.4 touch column and §11.5.3/11.5.4.
**Depends on:** WI-A.451, WI-1.456.
**Produces:** `app/qml/controls/TouchHandlers.qml` (pinch/long-press/two-finger tap), hit-area adjustments, `app/tests/scripts/test_desktop_idioms.py` run on Android/ChromeOS.
**Interface:** §11.4.4 gesture table; §11.5.3–11.5.4 rules.
**Steps:** 1. `PinchHandler`/`TapHandler` set. 2. Hover detection. 3. Hit-area expansion. 4. Parity script.
**Acceptance tests:** 1. Given a frame on the emulator, When a pinch is performed, Then zoom changes about the centroid. 2. Given a ChromeOS device with a mouse, When a block is right-clicked, Then the context menu opens. 3. Given every panel open at 900 dp width, When the automated `test_a11y` check runs, Then all touch targets are ≥ 48 dp.
**Size:** M.
**Notes / pitfalls:** stylus hover arrives as `SOURCE_STYLUS`, treat like mouse hover.

### WI-A.454 — HDR `SurfaceView` sibling integration in the Picture panel
**Goal:** §11.5.5 rules incl. the popup rule.
**Depends on:** WI-A.400, WI-A.401, WI-1.456.
**Produces:** `app/src/platform/android_native_host.cpp`, `android/app/src/main/kotlin/org/ovt/HdrSurfacePositioner.kt`, `android/app/src/androidTest/java/org/ovt/SiblingSurfaceTest.kt`.
**Interface:** `NativeSurfaceHost` (WI-1.463) Android backend; `pictureRectChanged(x, y, w, h)`; `nativeSetVisible(bool)`.
**Steps:** 1. Geometry relay (coalesced per frame). 2. Popup watcher. 3. Lifecycle (`surfaceCreated/Destroyed` → `retarget`). 4. Tests.
**Acceptance tests:** 1. Given NATIVE HDR mode, When a popup opens over the picture, Then the sibling is `GONE` and the embedded texture is visible, and When it closes, Then the sibling returns. 2. Given NATIVE HDR mode, When the device rotates, Then `Selection.frame` and zoom persist and the sibling re-attaches.
**Size:** M.
**Notes / pitfalls:** HDR assertions run on the `android-hdr` device of chapter 14 §14.9.2; the Pixel Tablet (SDR) exercises only geometry and visibility.

### WI-A.455 — State preservation and lifecycle
**Goal:** §11.5.7.
**Depends on:** WI-A.450, WI-1.454.
**Produces:** `app/src/platform/android_state.cpp` (state ↔ JSON), `onSaveInstanceState` glue in `MainActivity.kt` (owned by WI-0.050, extended in place), `android/app/src/androidTest/java/org/ovt/StateRestoreTest.kt`.
**Interface:** §11.5.7 saved-state keys (project path/URI, `SelectionState`, `ViewState`, layout preset, panel state ≤ 100 KB).
**Steps:** 1. State serialiser. 2. Save/restore hooks. 3. Job continuity across configuration changes. 4. Test.
**Acceptance tests:** 1. Given "Don't keep activities" enabled, When backgrounded and resumed, Then the same frame/selection show within 2 s.
**Size:** S.
**Notes / pitfalls:** `OvtSession.kt` itself is produced by WI-A.450; this WI only uses it.

### WI-A.456 — Play / F-Droid packaging hooks for the GUI (icons, adaptive icon, splash, about/licences)
**Goal:** Store-ready presentation; licence texts (GPL-3, LGPL, AOM notice) in About.
**Depends on:** WI-A.450, WI-0.603 (`THIRD_PARTY_NOTICES.md` generator), WI-0.606 (branding single source), WI-0.056 (signing and F-Droid metadata).
**Produces:** `android/app/src/main/res/` (icons, splash), `app/qml/dialogs/AboutDialog.qml` (shared with desktop).
**Interface:** About dialog contents (§11.2.3 Help); `THIRD_PARTY_NOTICES.md` as a Qt resource.
**Steps:** 1. Adaptive icon and splash. 2. About dialog reading the generated notices. 3. Store listing assets.
**Acceptance tests:** 1. Given the release AAB, When `bundletool validate` runs, Then validation passes and About lists every third-party licence from `THIRD_PARTY_NOTICES.md`.
**Size:** S.
**Notes / pitfalls:** F-Droid builds must not include Play-only dependencies.

### WI-A.457 — Android instrumentation CI (emulator) and perf run
**Goal:** Nightly instrumentation suite on Pixel Tablet and Chromebook x86_64 emulator profiles; `bench_gui_scrub` on the Pixel Tablet runner.
**Overlap:** superseded by WI-1.577 (Android instrumentation workflow `android-nightly.yml`); the `bench_gui_scrub` device run is registered through the Android runner of WI-0.560. This WI only adds the GUI test classes of WI-A.450–A.455 to that workflow. See chapter 16 §16.3.
**Depends on:** WI-A.452, WI-A.453, WI-A.454, WI-A.455, WI-1.577.
**Produces:** job additions in `.github/workflows/android-nightly.yml` (instrumentation classes, `bench_gui_scrub` on `pixel-tablet`).
**Interface:** chapter 14 §14.10.4 and §14.9.3 (Android budgets; the scrub budget is read from `tests/perf/budgets.json`, D19).
**Steps:** 1. Register test classes. 2. Add the bench step with the §14.9.3 budget file. 3. Artifact upload.
**Acceptance tests:** 1. Given the nightly workflow, When it runs, Then all instrumentation tests pass on both profiles and `bench_gui_scrub` on `pixel-tablet` is within the scrub budget of chapter 14 §14.9.3 (`tests/perf/budgets.json`, D19).
**Size:** S.
**Notes / pitfalls:** no separate `android-gui.yml`; one Android nightly workflow (chapter 13).

---

### Glossary additions (chapter 11)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **Interaction script** — a pytest script (`app/tests/scripts/test_*.py`) that drives the real application through the test IPC of chapter 14 §14.10.3.
- **Dual view** — the FR-GUI-021 mode with two `VideoSurface` instances for slots A and B whose frame, zoom, pan, plane mode and overlay mode are synchronised by default and individually unlockable (§11.3.2, WI-2.489).
- **Sync set / sync lock** — the five properties Dual view synchronises and the per-property padlock that unlocks one of them.
- **`OvtSession`** — the process-lifetime Android singleton owning the core handles across Activity recreation (§11.5.7, WI-A.450).
