---
title: Ranking: what the user is doing
status: stable
updated: 2026-11-11
---

A name's score comes from the fuzzy matching (the same as Jolt's), and then from what Seek knows about the user:

$$\text{score} = 8\,(m + r) - \min(d, 40) + \sum b$$

- $m$, the matching: contiguous letters, word starts and the start of the name count for more; letters far apart count for less.
- $r$, what the name is: 6,000 [rank_exact_program] for a program whose name is exactly what was typed, 4,000 [rank_exact_name] for any name that is exactly it, 2,000 [rank_program] for a program with the word whole in its name; otherwise 0.
- The 8 is [rank_match_scale]. $d$ is the name's depth, the folders above it, counted up to 40 [rank_depth_max]: of two equal names, the shallower one wins.
- $\sum b$, the points in the table below, for what the user did.

Close names (a typo) aren't scored this way: they come after every real match, ordered by how close they are.

Then what Seek knows about the user moves the best 2,000 [ranked_results] up or down:

| what Seek knows | points | shown as |
|---|---|---|
| changed (or opened by a program) in the last hour, day, or week | +3,000 [rank_changed_hour], +1,500 [rank_changed_day], +600 [rank_changed_week] | "changed 2 h ago", "opened 5 min ago" |
| picked in Jolt's Ctrl+P for this very text | up to +5,000 [rank_jolt_picked_max] | "picked in Jolt for this" |
| a file opened from Jolt | up to +2,000 [rank_jolt_file_max] | "opened in Jolt" |
| in a folder the user opens in Jolt, or up to two levels below it | up to +1,200 [rank_jolt_folder_max], by how much it is used | "in a folder used in Jolt" |
| inside the folder given as `near` | +2,500 [rank_near]; otherwise +150 [rank_near_level] for each folder level shared with it | "inside seek" |
| in a cache, a package store, a log or AppData | −2,000 [rank_noise] | – |

**Every result carries its reasons** ("exact name, changed 2 h ago, in a folder used in Jolt"), so the agent can tell a confident answer from a guess.

**When nothing matches well:**

- Letters scattered along a long name match by chance. They are counted apart and shown only when nothing better is there, and the answer says so.
- Close names, with a typo, are tried only when no name has the letters in order. The answer says how close each one is, as a percentage of the letters typed.

`near` is a boost, not a filter: `path` filters.
