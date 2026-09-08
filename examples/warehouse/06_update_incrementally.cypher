CALL sync.prepare_model("warehouse", false, true)
YIELD status, generation, oracle_epoch, pairs, pair_edges,
      materialized_pair_edges, incremental_enabled
RETURN status, generation, oracle_epoch, pairs, pair_edges,
       materialized_pair_edges, incremental_enabled;

// When files 00 through 05 were run in order, expected row:
// "OK", 1, 2, 15, 60, false, true

CALL sync.update_cells("warehouse", [{
  state_key: "east_bay:west",
  action_key: "to_wall",
  output_key: "east_landmark"
}], 15)
YIELD status, maintenance_mode, generation, oracle_epoch, changed_cells,
      direct_pair_edges, fallback_rebuild
RETURN status, maintenance_mode, generation, oracle_epoch, changed_cells,
       direct_pair_edges, fallback_rebuild;

// Expected row:
// "UPDATED", "INCREMENTAL", 2, 2, 1, 5, false

// Every plan from the previous generation is stale, including words unaffected by this cell.
CALL sync.validate_observed_update(
  "warehouse", 1, ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, ["west_landmark"], [], false
)
YIELD decision, generation, observation_compatible
RETURN decision, generation, observation_compatible;

// Expected: "STALE_GENERATION", 2, null
// The updated generation is already prepared. Adopt a fresh plan and reset its prefix.
WITH ["west_bay:east", "east_bay:west"] AS hypotheses
CALL sync.plan_disambiguate("warehouse", hypotheses, 1, 64)
YIELD outcome, word, generation
WITH hypotheses, word, generation, outcome
WHERE outcome = "PLAN"
CALL sync.validate_observed_update(
  "warehouse", generation, hypotheses, word, 0, [], hypotheses, true
)
YIELD decision, expected_hypotheses, observation_compatible
RETURN word, generation, decision, expected_hypotheses, observation_compatible;

// Expected: ["to_corridor"], 2, "CONTINUE", ["west_bay:east", "east_bay:west"], true
