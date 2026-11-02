---
title: Backups
app: backup
section: apps
order: 44
keywords: backup, restore, time machine, recover files
---
# Backups

Automatic backups of your home folder.

## Getting started

1. In the app's **Settings** choose the **destination**: an external disk (ideally ext4) or a
   folder. Backups are stored in `Axia DE-Copias`.
2. Choose the **frequency**: hourly, daily, weekly or manual.
3. **Back up now** makes the first one.

## How it works

- Each backup is a full snapshot of your home folder at that moment, but **only takes the space of
  what changed**: files identical to the previous backup are not stored again.
- **Kept:** every backup from the last 24 hours, one per day for the last month and one per week
  before that. If the disk fills up, the oldest ones are deleted.
- **It doesn't slow the desktop down:** it runs at the lowest processor and disk priority, and
  pauses by itself when the computer is on battery (this can be turned off).
- If it is interrupted (the disk is unplugged, the computer shuts down), the next one continues
  where it left off.
- **Exclusions:** folders you don't want backed up. Cache and Trash are never copied.
- It runs as your user: no administrator password needed.

## Restoring

In **Backups** choose the date and browse as in Files. You can **restore** a file, a folder or
**everything**. If the file already exists, both are kept (with “(restored)” in the name), unless
you choose to replace.

When it finishes, a [notification](help:notifications) tells you how it went. Backup encryption is
not available yet: if you need it, use an encrypted disk.
