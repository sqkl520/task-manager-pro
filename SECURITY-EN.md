[简体中文](SECURITY.md) | English

# Security Policy

## Reporting a vulnerability

**Use GitHub's private vulnerability reporting** —
[open a draft advisory](https://github.com/sqkl520/TaskManagerPro/security/advisories/new).
It stays private between you and the maintainer until a fix ships.

If that link does not work on your account, open a
[regular issue](https://github.com/sqkl520/TaskManagerPro/issues/new) that says
only "security report, please enable a private channel" and **nothing else** —
no details, no reproduction steps, no affected paths. The maintainer will open a
private channel and reply there.

Do not put exploit details in a public issue, a pull request, or a commit
message.

## What to expect

| | |
|---|---|
| Acknowledgement | within **7 days** |
| Initial assessment (is it real, how severe) | within **14 days** |
| Fix, or a written decision not to fix | within **90 days** for confirmed issues |

These are targets, not a contract. This is a single-maintainer project. If a
window passes with no reply, ping the advisory thread — silence usually means
the notification was missed, not that the report was ignored.

## Scope

Task Manager Pro is a Win32 program that runs elevated. It reads information
about every process on the machine, and starts an ETW session when the Network
page is enabled. Things that count as security issues here:

### Privilege and permission boundaries

- **Executing something it should not, at a higher privilege level.** The
  program starts and ends processes and reads other processes' memory usage and
  command lines. Any path that turns "look at" into "control" is a real problem
- **Wrong handle lifetime management.** Caching a handle to a process that has
  exited and then using it after its PID is reused is a genuine vulnerability
  class here, not merely a resource leak
- **Mishandled ETW session permissions.** The Network page needs administrative
  rights. Insufficient rights should degrade cleanly rather than crash or
  silently report wrong data

### Parsing external data

- **Memory-safety bugs driven by paths, command lines, or process names.** Those
  strings come from places this program does not control, and both their length
  and content can be hostile. Buffer handling errors count
- **String injection into the interface.** Process names and command lines are
  rendered in the list. If a process can make the UI show content that does not
  belong to it — a fabricated row, a fabricated state — that is a problem

### Data leaving the machine

- This program **reads local data only and makes no network requests.** Any
  finding that it sends data outward is serious
- The probe tools (`tools/`) emit local process names, PIDs, paths, and command
  lines. If a change causes that output to be committed or uploaded, that is a
  problem

### Build and dependencies

- **A poisoned build chain.** The project has no third-party dependencies, only
  MSVC and the Windows SDK. Any change that introduces an external download in
  `build.bat` needs justification

## Explicitly out of scope

- **Requiring administrator rights.** That is a design decision, not a defect.
  The Network page's ETW session demands elevation, and the stock Task Manager
  works the same way
- **Killing processes can destabilize the system.** That is the feature, not a
  vulnerability. Ending a critical process will of course cause problems
- **A displayed number differs from the stock Task Manager.** That is a bug —
  file a regular issue, not a security report
