[简体中文](README.md) | English

# Evidence

First-hand measurements taken on the development machine while reconstructing
Windows 10 Task Manager: UI layout reverse-engineering, MUI string extraction,
system metric sampling, and data-path benchmarks.

Source comments cite these files as the origin of specific numbers. Wherever a
comment says "measured" or "confirmed", the raw data is here.

## Why only part of it is here

| Included | Not included |
|---|---|
| Readable text dumps (`.md` `.txt` `.tsv` `.json` `.xml`) | Screenshots and crops (`.png` `.bmp`) |
| Probe sources used to take the measurements (`.cpp` `.ps1` `.js`) | Probe build output (`.exe`) |
| Individual files under 2 MB | Single dumps over 2 MB |

Screenshots are excluded for **size**, not secrecy: the reference screenshots
and zoomed crops of the original Task Manager run to several hundred MB
combined, and they are regenerated on every comparison pass. They stay in the
workspace on the development machine.

Files over 2 MB are excluded for the same reason. The largest is
`tm/disasm.txt`, a 17.8 MB disassembly dump that is also encoding-corrupted and
unreadable.

A comment citing a file that is not here refers to one of the two categories
above. The reference is kept deliberately: it records *which measurement
produced this number*, not *click here to open it*.

## Layout

```
docs/evidence/
├── exec/          Specs and measurement tables from review rounds (see docs/specs/)
├── main/          Collection-path benchmarks and API matrices
├── roast2/        Per-page visual specs
├── tm/            The Task Manager binary: MUI, disassembly, registry exports
└── crops/         Pixel measurements of icons and colors (text only)
```

## Confidence labels

The original documents tag every conclusion with one of three levels:

| Label | Meaning |
|---|---|
| **[confirmed]** | Measured on this machine, or stated outright in authoritative documentation |
| **[strong inference]** | Derived from several consistent measurements; logically closed, but never directly observed |
| **[inference]** | Induced from observed behavior, without first-hand evidence |

Carry the label with the conclusion. Treating an `[inference]` as `[confirmed]`
is a mistake this project has made repeatedly.

## Reproducing

All measurements were taken on Windows 10 Pro 22H2 (22H2) /
Xeon 服务器级 CPU (10C/20T) / 32 GB. Absolute numbers will differ on other
hardware, but the **order-of-magnitude relationships** — which data path is
cheap and which is catastrophic — are this project's core finding and do not
change with the machine.

## About the probe sources here

The `.cpp` and `.ps1` files under `docs/evidence/` are **archived as they were
during measurement**, not tools you can run today. Two caveats:

- **Paths**: the originals contain absolute paths from the development machine.
  Most have been changed to locate themselves relative to the script; a few keep
  the original and note it in a header comment — that is the record of *where it
  ran*
- **Compilability**: some probes depend on the Windows SDK of that era and will
  not build against a current one (for example `gpuinfo_probe14.cpp` relies on
  `d3dkmthk.h` layouts that have since changed). They record **how that
  measurement was taken**, not "run this now"

To take a new measurement, write a fresh probe following the archived approach
rather than compiling these files directly.
