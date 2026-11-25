# docs/archive manifest

History kept for reference. Nothing was deleted; each file moved with `git mv` (history follows).

| Original path | New path | Date | Reason |
|---|---|---|---|
| docs/bumped-normals-study.md | docs/archive/bumped-normals-study.md | 2026-11-25 | Study finished; work is in main (see docs/reflection-speckle.md and the unified-reflections design for what is current). |
| docs/fork-vs-upstream.md | docs/archive/fork-vs-upstream.md | 2026-11-25 | Snapshot of how the fork differed from upstream at one point in time; superseded by the 1.1.2 port and the upstream-rt-merge work. |
| docs/upstream-cherry-picks.md | docs/archive/upstream-cherry-picks.md | 2026-11-25 | Cherry-pick list for an earlier upstream version; superseded by skill `vs-upstream-hand-port`. |

Kept in docs/ on purpose: pipeline-audit.md, ssfx-rain-comparison.md (still referenced as open work).

## Other moves of this restructure (not archive; recorded here so every move is in one place)
| Original path | New path | Date | Reason |
|---|---|---|---|
| .claude/skills/vs-architecture/SKILL.md | docs/reference/architecture.md | 2026-11-25 | Technical reference, not a workflow; frontmatter dropped. |
| .claude/skills/vs-dxvk-input/SKILL.md | docs/reference/dxvk-input.md | 2026-11-25 | same |
| .claude/skills/vs-runtime-paths/SKILL.md | docs/reference/runtime-paths.md | 2026-11-25 | same |
| .claude/skills/vs-gbuffer-dxvk-facts/SKILL.md | docs/reference/gbuffer-dxvk-facts.md | 2026-11-25 | same; push-block section corrected against NvrDxvkExt.h and patch 0004 |
| (memory) pipboy-sleeve-clipping-research | src/effects/PipboyFit/README.md | 2026-11-25 | The effect had no folder; the note was its only doc. Original kept in F:\Claude Brain\archive\2026-11-25\vs\memory\. |
| (memory) vs-puddles, vs-sun-gi, vs-rt-gi-review, ... | docs/reference/lessons-from-archived-notes.md | 2026-11-25 | Still-true lessons and owner decisions; the notes are archived (see F:\Claude Brain\archive\2026-11-25\vs\MANIFEST.md). |
New skills: vs-upstream-hand-port, vs-bench-check. Rewritten skills: vs-build-deploy, vs-effects-modular (guideline shape).
