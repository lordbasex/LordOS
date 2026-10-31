**In development for 0.8.7. The latest release, 0.8.6-beta, does not include Cleaner.** This page describes the work on the development branch; it can still change before release.

## The short version

Cleaner shows what is taking up your disk and helps you remove what can safely go. Its rule is simple: **analyse, review, plan, confirm, execute, verify**. A scan only reads. Nothing changes until you have seen what will happen and said yes.

[[shot:0]]

Think of a careful house clearance. Someone walks through and writes a list, touching nothing. You go through the list. Only then does anything leave the house, and each item is checked once more on the way out.

[[diagram:flow]]

## What it does

- **Scan** runs Cleanup, Storage and Duplicates one after the other, then shows a review board of what it really found: one card for each thing it found, with its real sizes and the real files behind it.
- **Cleanup** lists caches, developer tool downloads, browser caches, old temporary files and the Trash, each with its reason, its risk and what will happen to it.
- **Large files** filters by size, age and type.
- **Storage** shows the largest folders as bubbles, a map, a folder list and a summary by type. A folder is drawn with what it is, so videos, music and code are told apart at a glance.
- **Duplicates** finds exact copies in the folders you choose. Nothing is selected for you.
- **Activity** records what ran: counts and sizes, never a path.

[[shot:1]]

## What it will not do

Cleanup rules never cover cookies, sessions, history or passwords, or `~/.cache` as a whole. Cleaner makes no claim to protect your computer and does not look for malware. It has no memory "free up" and no speed test: neither is something it could do honestly. The sizes it shows before acting are estimates, and it says so, because hard links, open files, compression and snapshots change what is really freed.

## How it stays safe

Scanning runs in `lord-clean`, a sandboxed helper that can only read. Plans are built in `lord-server`, not in the app, and the app only holds candidate ids, never the authority to touch a path. Everything is checked again right before acting.

Regenerable cache files beneath fixed folders can be deleted for real, as your own user, and only by this path: `lord.fs` still has no general delete. Anything else goes to the Trash, where you can restore it. Three closed administrator actions clean the system's own files, each confirmed; they are the one part not yet run on a test machine.

## Easy on a modest computer

Cleaner comes in three looks, follows light and dark mode, and works in English, Spanish and Portuguese, right to left too. Its motion runs only while its window is in front and stops entirely with Reduce motion. Moving parts are kept in separate frames so the screen repaints as little as possible; the numbers are in `docs/CLEANER-TESTS.md`. They were taken on a virtual machine, not on a Raspberry Pi.

[[shot:2]]
