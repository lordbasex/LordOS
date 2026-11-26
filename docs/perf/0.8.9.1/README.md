# 0.8.9.1 memory work: raw data

How these were measured: [../MEMORY-HEAPTRACK.md](../MEMORY-HEAPTRACK.md).

PC: Intel i9-10900, 128 GB, RX 580, 3 displays at 1920×1080, Debian 13, kernel 6.12, THP `always`, Workspaces,
7 widgets on one display. Each CSV is one fresh session, sampled every 10 s after 3 minutes of settling
(`tools/bench/session-sample.sh`).

| File | Build | Scene |
|---|---|---|
| `pc-ws02-widgets-terminal.csv` | ws02 | widgets + Terminal with Claude Code at rest |
| `pc-ws03-widgets.csv` | ws03 | widgets only (the Terminal was opened after the sample) |
| `pc-ws04-widgets-terminal.csv` | ws04 | widgets + Terminal with Claude Code |
| `pc-ws05-widgets-terminal.csv` | ws05 | widgets + Terminal with Claude Code |
| `pc-ws06-widgets-terminal.csv` | ws06 | widgets + Terminal with Claude Code |
| `pc-ws07-widgets-terminal.csv` | ws07 | widgets + Terminal with Claude Code |
| `pc-ws08-widgets-terminal.csv` | ws08 | widgets + Terminal with Claude Code |
| `heaptrack-ws06-live.txt` | ws06 under heaptrack | heap alive at rest, by line of code |
