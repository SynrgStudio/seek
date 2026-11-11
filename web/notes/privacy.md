---
title: Privacy
status: stable
updated: 2026-11-11
---

Seek only reads. Some places are never listed, searched or remembered:

- SSH, GPG, AWS, Azure, Kubernetes and Docker credentials
- Windows' credential and key stores
- browser profiles: Chrome, Edge, Brave, Thorium, Vivaldi, Opera and Firefox
- password managers: Bitwarden, 1Password, KeePass
- Telegram and Signal data
- the Recycle Bin, `System Volume Information`, and `.kdbx`, `.pfx`, `.p12` and `.ppk` files

More can be added in the settings (`exclude =`).

**The log.** Every agent's call is written to `%LOCALAPPDATA%\Seek\log.txt`: when, which agent, which tool, the arguments, and how long it took. The user can see what agents looked for. `log = no` turns it off. Programs' searches through the pipe (`find.rows` and the like) aren't written there.

**Nothing leaves the PC.** Seek makes no network connections. Its pipe admits only the same user on the same machine, and SYSTEM.
