# Warehouse example

Run the numbered Cypher files in order. The model has five orientation-aware
states, four controller actions, and four abstract sensor outputs. Preparation
validates all 20 transition cells and all 20 observation cells before writing
the pair oracle.

The ambiguous bay hypotheses synchronize at `dock:north` under
`["to_corridor", "go_west"]`. The first action alone emits different landmark
outputs, so `["to_corridor"]` is also a homing disambiguation word.

File 05 demonstrates both monitors: `validate_update` checks motion-only
localization, while `validate_observed_update` conditions on committed Mealy
outputs first. Its execution example moves from `WAIT` without localization
to `CONTINUE` when a report arrives for the same prefix, then validates both
outputs after the second action. Independent alternative histories demonstrate
an impossible output and `REPLAN` from a strict localizer subset.

Keep the original support, word, and generation with each plan. Append one
output per committed action; repeat the same prefix when only localization
changes. Adopting a new plan resets its completed-step count and output list.
The caller decides when a physical action and its sampled output are committed.

Files 00 through 05 demonstrate a visual snapshot oracle. File 06 reprepares
the same model in compact incremental mode and atomically changes one
observation cell while preserving the oracle epoch.
The update advances the model from generation 1 to 2, makes old plans stale,
and allows a fresh plan to be monitored immediately without another preparation.
The longer [README walkthrough](../../README.md#worked-warehouse-example)
includes an extra `mark_dirty` step, so its final update reaches generation 3.
