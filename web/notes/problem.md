---
title: The problem: what a search costs an agent
status: stable
updated: 2026-11-11
---

The search itself is rarely the cost. The cost is the time, the tokens, the round trips and the guessing.

- **Time.** An agent that needs a file whose place it doesn't know runs `dir /s` or `Get-ChildItem -Recurse`.
  - Over one user profile that took 85 s [find_dir_s] and 98 s [find_get_childitem_s].
  - Listing a kind of file over three drives took 567 s [count_dir_s].
  - The agent waits, and so does the person.
- **Tokens.** Every line a command prints is read by the model. Listing the 8,200 [blend_files] `.blend` files on one PC is 196,438 tokens [count_everything_list_tokens] with Everything. That is more than many models' whole context, to answer "how many and where". Seek's summary of the same is 378 tokens [count_tokens].
- **Round trips.** A tool that knows nothing answers in pieces: list, filter, list again, open. Each step is a model turn of several seconds.
- **Guessing.** "The file I was working on" means something to the user and nothing to a shell. The agent asks, or guesses wrong.

The tools agents have don't fit:

- **ripgrep** is the fastest way to search text without an index, but each run starts over. On a 500,000-file [fff_chromium_files] Chromium checkout, fff's authors measured 3 to 9 s [fff_chromium_ripgrep_s_min] [fff_chromium_ripgrep_s_max] per ripgrep run.
- **Claude Code's Glob** doesn't skip ignored folders. Users report it timing out at 20 s [claude_glob_timeout_s] in large repositories, where ripgrep alone takes under a second.
- **Everything** finds names instantly. But it needs a service or administrator rights, it doesn't forgive a typo, and it answers with the whole list.
