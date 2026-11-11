---
title: Numbers others published
status: stable
updated: 2026-11-11
---

Measured by others, on other machines and other data. Their sources are in each fact's `source`.

- **[fff's README](https://github.com/dmtrKovalenko/fff):**
  - On whole agent tasks in a big repository: 24.7 s [fff_agent_shell_s] with shell search and 14.3 s [fff_agent_fff_s] with fff. The tokens were almost the same: 85,400 [fff_agent_shell_tokens] against 82,500 [fff_agent_fff_tokens].
  - In a held-out run, one model was slower with fff: 9.0 s [fff_heldout_fff_s] against 8.1 s [fff_heldout_shell_s].
  - On a 500,000-file [fff_chromium_files] Chromium checkout, each ripgrep run took 3 to 9 s [fff_chromium_ripgrep_s_min] [fff_chromium_ripgrep_s_max], and each fff query under 10 ms [fff_chromium_query_ms_max].
- **[Everything's FAQ](https://www.voidtools.com/faq/):** a million files take about a minute [everything_index_min_per_million] to index, and about 100 MB [everything_mb_per_million] of memory.
- **[ripgrep's README](https://github.com/BurntSushi/ripgrep#quick-examples-comparing-tools)**, searching the Linux kernel's source:

  | tool | time |
  |---|---|
  | ripgrep | 0.082 s [ripgrep_kernel_s] |
  | git grep -P | 0.273 s [ripgrep_kernel_git_grep_s] |
  | The Silver Searcher | 0.443 s [ripgrep_kernel_ag_s] |
  | ugrep | 0.639 s [ripgrep_kernel_ugrep_s] |
  | ack | 2.9 s [ripgrep_kernel_ack_s] |

- **[Claude Code's issue tracker](https://github.com/anthropics/claude-code/issues):** users report its Glob tool timing out at 20 s [claude_glob_timeout_s] in large repositories, where ripgrep alone takes under a second, because it doesn't skip ignored folders.

**Read together:**

- A search tool that keeps its index between calls is orders of magnitude faster per search than one that starts over.
- But a search is only part of an agent's turn, so the biggest wins are where the old way took minutes or flooded the context.
