---
title: Remote desktop
section: system
order: 63
keywords: remote, remote access, browser, screen sharing, lan
---
# Remote desktop

From another computer on your network you can use Axia DE in a web browser, without installing
anything.

1. Turn it on in [Settings → Sharing](settings:sharing) → “Allow access from the network”. It
   ships **off**.
2. On the other computer open the address shown in Settings (`https://<IP>:5673`).
3. The browser warns that the certificate is self-signed: compare the **fingerprint** with the one
   shown in Settings.
4. Log in with your user and password.

- With **your own user** you see and control the same session that is on the monitor.
- With **another user**, that person gets their own desktop, at the same time.
- In Settings you choose who can log in (everyone or only the `lordos-remote` group) and see who is
  connected, with a button to disconnect them.

It works before anyone has logged in on the computer. Failed attempts wait longer each time.
Protected windows show black.
