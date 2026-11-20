[简体中文](README.md) | English

# Task Manager Pro

A rewrite of the Windows 10 Task Manager. On the machines where the stock one
freezes — many cores, many threads, many processes — this one stays responsive.

![Stock Task Manager compared with Task Manager Pro](docs/images/compare.png)

---

## What it solves

The stock Windows 10 Task Manager goes unresponsive for long stretches once the
process and thread counts get large. Not slow — **unresponsive for twenty-odd
seconds**, with dropped frames while dragging the window.

The root cause is the data path it chose for itself. A `\Thread(*)` PDH wildcard
query costs nearly a second on an 8000-thread machine, plus WMI thread queries
(seconds) and per-process signature verification (seconds). That path is bound
to the UI thread with no backpressure, so when one collection exceeds the refresh
budget the interface simply stops.

This implementation replaces the whole path: one `NtQuerySystemInformation` call
yields the full snapshot; every expensive field (icon, publisher, command line,
signature) is lazily loaded and persistently cached; the UI thread never calls a
blocking API.

### Measured (same machine, same load)

Test machine: Xeon 服务器级 CPU (10C/20T) / 32 GB / Windows 10 22H2 22H2,
roughly 490 processes and 8,450 threads during measurement.

| Metric | Stock | Task Manager Pro |
|---|---|---|
| Longest unresponsive stretch after launch | **23.95 s** | **0 s** |
| Steady-state CPU (share of whole system) | ~6.4% | **0.12%** |
| Working set | 47 MB+ | **13.1 MB** |
| Thread count | 19 | **4** |
| Full snapshot cost | hangs, no data | **9.1 ms** (9% of a 1 Hz budget) |
| Scroll paint p95 | — | **0.87 ms** (5% of a 60 fps budget) |

Full methodology and per-path costs are in
[BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md) and
[DIAGNOSIS.md](docs/DIAGNOSIS.md).

---

## Quick start

### Requirements

Visual Studio 2022 or Build Tools, with the C++ desktop workload and the Windows
SDK. Nothing else — no package manager, no external dependencies.

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

### Build

```bash
build.bat
```

The script locates MSVC through `vswhere`, so you do not need to open an
"x64 Native Tools" prompt first.

### Verify the collection layer first

```bash
build\bench_snapshot.exe
```

Run this **before** anything else. It measures how the cost of a single
`NtQuerySystemInformation` call scales with process count. If it passes in tens
of milliseconds, the rest of the architecture holds. Over 200 ms means the
collection layer has a problem — do not go on to look at the UI.

### Run

```bash
build\TaskManagerPro.exe
```

The status bar shows live self-monitoring numbers: collection cost, paint cost,
rows actually rendered, and the snapshot generation number. While scrolling,
"paint" should stay under 4 ms and "visible rows" should equal only what fits on
screen — if it equals the total process count, virtualization is not working.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ UI thread                                                     │
│   WM_TIMER(250ms) → compare generation number only; no change │
│                     means do nothing at all                   │
│   WM_PAINT        → iterate visible rows [firstRow, lastRow]  │
│                     double-buffered BitBlt, GDI DrawTextW     │
├────────── double buffer + atomic generation publication ─────┤
│ Collector thread (THREAD_PRIORITY_BELOW_NORMAL)               │
│   Single full NtQuerySystemInformation(SystemProcessInformation)│
│   Buffer reused across rounds; grows only on                │
│   STATUS_INFO_LENGTH_MISMATCH                                 │
│   CPU% = diff of (KernelTime+UserTime) across rounds,         │
│   no additional system call                                   │
├──────────────────────────────────────────────────────────────┤
│ LazyLoader (standalone component, not wired into the main UI) │
│   path / command line / icon / signature / account name,      │
│   on demand plus persistent cache                             │
└──────────────────────────────────────────────────────────────┘
```

### Four hard rules

1. **The UI thread never calls** `NtQuerySystemInformation` / `OpenProcess` /
   `LookupAccountSid` / `WinVerifyTrust` / any PDH `Collect`
2. **One system call yields the whole snapshot** — never "enumerate N PIDs, then
   call 5 APIs on each"
3. **Never poll** `\Thread(*)` or `\Process(*)` PDH wildcards
4. **Every expensive column is lazy-loaded and cached**

---

## Layout

```
src/            Implementation (29 .cpp files, split by responsibility — see below)
include/        Matching headers
tools/          One-off probes and verification utilities (not part of the build output)
docs/           Design documents, implementation specs, evidence
build.bat       The only build entry point
```

Main-window files, split by responsibility:

| File | Responsibility |
|---|---|
| `tmwindow_paint.cpp` | Painting |
| `tmwindow_list.cpp` | Process list: grouping, sorting, row construction |
| `tmwindow_events.cpp` | Message handling |
| `tmwindow_details.cpp` | Details page |
| `tmwindow_toolbar.cpp` | Toolbar |
| `tmwindow_menus.cpp` | Context menus |
| `tmwindow_icons.cpp` | Asynchronous icon resolution |
| `collector.cpp` | Snapshot collection thread |
| `classifier.cpp` | Process grouping and naming rules |

---

## Documentation

- [Why the stock Task Manager freezes](docs/DIAGNOSIS.md) — measured cost per data path
- [Benchmarks](docs/BENCHMARK_RESULTS.md) — full measurement data
- [Technology choices](docs/TECH_STACK.md) — why C++ / Win32
- [UI reconstruction](docs/UI_SPEC.md) — where the interface specs came from
- [Documentation index](docs/README.md) — everything else

---

## On "reproducing the stock UI"

The interface was written from scratch against **observable specifications** of
the Windows 10 Task Manager: the declarative layout resources (`UIFILE`), the MUI
string tables, and pixel measurements taken from screenshots.

**No original source code was copied, decompiled, or linked.** What was examined
is the part of the shipped binary that can be legitimately observed — layout
resources, strings, and the rendered result at runtime.

---

## Known limitations

- Pixel-level verification was done at 100% DPI only. The high-DPI logic is
  correct (it scales by rp), but has not been verified pixel by pixel
- The network page requires administrator rights (the ETW session demands it)
- A few stock menu items were never captured in a screenshot; those are
  implemented from their caption and conventional behavior, and tagged
  `[inference]` in the specs

---

## License

[MIT](LICENSE)
