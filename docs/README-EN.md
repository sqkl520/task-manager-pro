[简体中文](README.md) | English

# Documentation

Task Manager Pro's documentation is in four parts. Pick the one that matches why
you are here.

## I want to know why this exists

| Document | What it covers |
|---|---|
| [DIAGNOSIS.md](DIAGNOSIS.md) | Why the stock Task Manager freezes on machines with many cores and threads. Includes measured cost per data path |
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Side-by-side measurements: unresponsive time, CPU, memory, thread count, snapshot cost |

Those two explain the problem and why a different implementation fixes it.

## I want to know how it was built

| Document | What it covers |
|---|---|
| [TECH_STACK.md](TECH_STACK.md) | Technology choices: why C++ / Win32 / Direct2D rather than alternatives |
| [UI_SPEC.md](UI_SPEC.md) | Which observable specs the UI was reconstructed from, and what "no source code was copied" means concretely |

## I am going to change the code

| Document | What it covers |
|---|---|
| [specs/](specs/) | Per-module implementation specs: process classification rules, menu strings, per-page layout, performance page geometry |
| [evidence/](evidence/) | The first-hand data behind those specs: MUI extraction, UIFILE decompilation, pixel measurements, path benchmarks |
| [reference/](reference/) | Reference screenshots of the stock Task Manager for pixel-level comparison |

Every conclusion in `specs/` carries a confidence label (**[confirmed]** /
**[strong inference]** / **[inference]**), and the corresponding raw data is in
`evidence/`. Check the label before changing code — treating an `[inference]` as
`[confirmed]` is a mistake this project has made repeatedly.

## Glossary

| Term | Meaning |
|---|---|
| **rp** | DirectUI layout unit, `1rp = dpi/96` pixels. Equals px at 100% DPI |
| **UIFILE 30001 / 30024** | Declarative layout files in the Task Manager resources; decompiling them yields exact geometry for every element |
| **duib** | The compiled binary form of a `UIFILE` |
| **group** | The three segments of the Processes page: Apps / Background processes / Windows processes. Group headers are suppressed when sorting by a numeric column |
