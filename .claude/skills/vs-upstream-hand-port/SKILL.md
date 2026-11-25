---
name: vs-upstream-hand-port
description: "Workflow: bring fixes from upstream adeesababa/VegasShaders into this fork by hand or by merge - dry-run with git merge-tree, port selected fixes, renumber settings migration steps, keep our defaults, review, record what was skipped. Use when upstream has a new release or the owner asks to take an upstream fix."
---

# Porting upstream commits into the fork

## Task
Take chosen upstream changes (remote `upstream` = github.com/adeesababa/VegasShaders, read only for us) into this fork without losing
our own work (deferred wetness, unified reflections, speckle fixes, single pass).

## Prerequisites
- `git fetch upstream`. Worktree on a side branch (`vs-build-deploy` skill); never commit on main without the owner's word.
- Know what is ours: `docs/deferred-roadmap.md`, `docs/unified-reflections-design.md`, `docs/deferred-wetness-design.md`.
- Upstream's `docs/next/*` files are not in either repo: a number cited only from there is unverified.
- Decision from the owner decides the mode: **hand port** (select fixes) or **full merge** (take upstream's RT as is, keep ours,
  hand-merge the seams). Example of each: hand port = branch `upstream-1.1.2-fixes`, commit f4e43e3 (in main); full merge = branch
  `upstream-rt-merge`, merge commit 68cbb9c (not in main; its record: `git show upstream-rt-merge:docs/upstream-113-merge.md`).

## Steps
1. **Dry run.** `git merge-tree --write-tree main upstream/main` (or per file `git diff main...upstream/main -- <path>`) to see which
   files conflict. Files only we touched stay ours; files only upstream touched are taken.
2. **Pick.** List upstream's changes since the merge base; mark each: take / skip (fork-only gating, version bump, things that
   duplicate ours). Write the skipped ones in the commit message (the 1.1.2 port skipped the DXVK-fork gating, the version bump,
   1.1.1 and 1.1.3).
3. **Port by hand** when the code differs: describe upstream's behaviour and write it fresh in our file; copy upstream's file only
   where we never changed it. (Decompiled game code is never copied.)
4. **Settings migrations.** Upstream and we both add `UpdateOldDefaults()` steps and raise the version. Keep upstream's steps at their
   numbers and **renumber ours after theirs** (the 1.1.3 merge put upstream's 90-94 first and ours 90-92 -> 95-97, version 97); a saved file
   at one of our old numbers must skip steps that already ran. Park upstream's step behind one constant per step when we keep our
   default (`kParkUpstream91/92/94` there).
5. **Defaults.** Keep ours; list every differing default in the merge doc (key, ours, upstream's) for the owner to decide, one key each.
   Edit fragments then `py -3.10 -I tools/defaults_tool.py merge .` and `check .` (skill `vs-effects-modular`). After a merge a
   duplicated default line has failed to load in game once: check for duplicates.
6. **Seams.** Where both touched the same pass (the Reflections compute/composite, the Reflections D3D9 variants) keep one source per
   frame (our G-buffer film or upstream's forward film), make binding numbers not collide, and write the choice in the effect README.
7. **Review.** `lead-architect` agent reviews the diff (approve / with changes / reject); apply the comments.
8. **Record.** One merge doc in `docs/` (what came unchanged, what stays ours, seams, defaults table, proven / believed / not run), the
   affected effect READMEs, and the room memory note for the upstream state.

## Validation
- Build and offline checks: skill `vs-build-deploy`. Defaults `check` clean; settings version equals the last step.
- Lead-architect verdict recorded. If the RT helper protocol changed, `NvrRtHelper.exe` must be replaced with the new build.
- In-game or bench result: skill `vs-bench-check`; mark what was only compiled as "not run in game".

## Troubleshooting
- Merged defaults file fails to load: a key duplicated on both sides; keep one.
- A saved settings file behaves wrongly after the merge: the version numbers collided; compare the step table with each side's.
- Shaders look stale after copying upstream's files: clear `Shaders\VegasShaders\Shaders\Cache\`.

## Links
- `docs/archive/upstream-cherry-picks.md` and `docs/archive/fork-vs-upstream.md`: how earlier ports were recorded.
- Branch `upstream-refl-rt-compare`: `docs/upstream-reflections-rt-compare.md` (exists on that branch only), the comparison behind the full-merge choice.
