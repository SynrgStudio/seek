---
title: Ranking: what the user is doing
status: stable
updated: 2026-11-11
---

A name's score comes from the fuzzy matching (the same as Jolt's).

- Contiguous letters, word starts and the start of the name count for more.
- An exact name counts for more still, and an exact program (`.exe`, `.lnk`) for most.
- The score is multiplied by 8 and lowered by the name's depth, so of two equal names the shallower one wins.

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
