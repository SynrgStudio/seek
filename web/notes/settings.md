---
title: Settings and command line
status: stable
updated: 2026-11-11
---

`%APPDATA%\Seek\seek.txt`, read when the background process starts:

```
exclude = C:\Users\me\Private      a folder
exclude = *.key                     files with that extension
exclude = secrets                   any folder or file with that name
log = no                            don't write log.txt
cache = 256                         MB of folders kept in memory; 0 for none (512 by default)
jolt = no                           don't use Jolt's index or what Jolt knows
```

Seek's own files are in `%LOCALAPPDATA%\Seek`:

- its copies of the index;
- `activity.txt`, with what it saw change;
- `log.txt`, with every call.

The command line:

```
seek --cli find "{\"query\":\"report .pdf\"}"      run a tool in this process
seek --call grep "{\"pattern\":\"TODO\",\"path\":\"C:\\\\code\"}"   through the background process
seek --reindex                                      walk the drives again now
seek --status                                       how the background process is: indexes, watchers, folders in memory
seek --stats                                        what agents asked, by day: calls, times, what found nothing
seek --stop                                         stop the background process
seek --version
```
