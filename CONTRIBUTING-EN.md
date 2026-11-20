[简体中文](CONTRIBUTING.md) | English

# Contributing

This project has no package manager, no dependency install, and no test
framework. Building it is one `build.bat`.

## Requirements

- **Windows 10 or newer** (the code calls Win32 and `ntdll.dll` directly; there
  is no cross-platform layer)
- **Visual Studio 2022 or Build Tools**, with the C++ desktop workload and the
  Windows SDK
- Nothing else

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

## Building

```bash
build.bat
```

The script finds MSVC through `vswhere`, so you do not need an
"x64 Native Tools" prompt. Output lands in `build\`.

The build produces three executables:

| Artifact | Purpose |
|---|---|
| `build\TaskManagerPro.exe` | The application |
| `build\bench_snapshot.exe` | Collection-layer benchmark; run after touching collection code |
| `build\bench_ui.exe` | Table-control benchmark; run after touching painting code |

## Before you open a pull request

### 1. Run the benchmarks

```bash
build\bench_snapshot.exe
build\bench_ui.exe
```

Both must pass, and the numbers must not regress noticeably.

`bench_snapshot.exe` measures how the cost of a single
`NtQuerySystemInformation` call scales with process count. The whole architecture
rests on this being cheap; if it regresses, a prettier interface does not help.

`bench_ui.exe` measures the virtualized table's scroll paint cost.

### 2. Read the specs before changing the UI

Every dimension, color, and caption in the interface has a source, recorded in
`docs/specs/` and `docs/evidence/`. Changing a value means the old one was wrong
— **explain in a code comment why**, citing the evidence you checked.

This project has been burned repeatedly by tuning numbers by feel. Conclusions
tagged `[inference]` are fair game if you have real evidence, but do not replace
one `[inference]` value with another `[inference]` value.

### 3. Do not commit build output

`.gitignore` already excludes `build/`, `*.obj`, `*.exe`, `*.pdb`. If something
else lands in the staging area, fix `.gitignore` rather than using `git add -f`.

### 4. Commit messages

Format `type(scope): description`, saying **why** rather than only what.

```
fix(list): suppress group headers when sorting by a numeric column

The stock Task Manager clears grouping whenever SortColumn is non-zero, so the
"Apps (N)" headers disappear. We kept showing them, which made row order look
wrong against the original.
```

`type` is one of seven: `feat` `fix` `docs` `refactor` `test` `chore` `perf`.

## Constraints specific to this repository

### Do not copy the original source code

The interface is built from **observable specifications**: layout resources
(`UIFILE`), MUI string tables, and pixel measurements from screenshots. All three
are legitimately observable.

Do not submit Task Manager source code found online, decompiled C pseudocode, or
anything "adapted from" either. Reading a disassembly to **extract a value**
(whether a constant is 28 or 30) is fine; using it to reconstruct implementation
logic is not.

### No absolute paths

Scripts, comments, and docs must not contain machine-specific paths such as
`D:\Projects\...`. `build.bat` locates itself with `%~dp0`; `tools\*.bat` use
`%~dp0..`. If code needs to know where the project lives, derive it from the
executable path rather than hard-coding it.

### Real process data does not go in the repository

The probe tools emit local process names, PIDs, paths, and command lines. That is
real data, and their output files are in `.gitignore`. If you want to commit a
sample, fabricate it.

## Layout conventions

```
src/             Implementation, split by responsibility
include/         Matching headers
tools/           One-off probes and verification utilities
docs/            Design documents, implementation specs, evidence
docs/specs/      Per-module specs, cited directly from code comments
docs/evidence/   First-hand data behind those specs
docs/reference/  Reference screenshots of the stock Task Manager
```

When adding a source file:

1. Put it in `src/`, headers in `include/`
2. Add it to the link line in `build.bat`
3. If it represents a new category of work, consider whether a new file is
   really needed — the existing files are already split fairly finely

## Reporting problems

Use the issue templates. When filling in the environment, **include CPU core
count and process/thread counts** — this project exists because the stock Task
Manager fails on large machines, so scale matters far more here than in most
projects.

If something "differs from the original", attach a screenshot of the original.
That is the strongest evidence this project can get.
