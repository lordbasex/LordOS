---
title: Logs
app: logs
section: apps
order: 45
keywords: logs, journal, errors, debug, console, diagnostics
---
# Logs

System and Axia DE logs. Use it to see what happened when something fails.

## Sources

- **My logs:** your systemd journal (your apps and services), read with your permissions.
- **System** and **Kernel:** the system journal. Administrators (sudo group) see it through the
  administration helper, as does anyone in the journal reader group.
- **Axia DE:** your session logs (`~/.cache/axia`, like `session.log`).

## Filter

- **Level:** all, info, warnings or errors.
- **Time:** last 15 minutes, last hour, 24 hours or everything.
- **Process or unit**, and **search** in the message text.

Clicking an entry shows the full message with its fields (time, process, PID, unit).

## Live and export

- **Live** follows new messages as they arrive, without reloading.
- **Export** saves what you see (with the filters) to a text file in your folder, to send it when
  asking for help.
