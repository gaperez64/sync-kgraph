CALL sync.validate_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"],
  1,
  ["corridor_w:east", "corridor_e:west"],
  true
)
YIELD status, decision, reason, expected_hypotheses, unexpected_hypotheses, generation
RETURN status, decision, reason, expected_hypotheses, unexpected_hypotheses, generation;

// Expected decision: "CONTINUE"

// Commit to_corridor -> west_landmark while the localizer is unavailable.
CALL sync.validate_observed_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 1, ["west_landmark"], [], false
)
YIELD decision, expected_hypotheses, observation_compatible
RETURN decision, expected_hypotheses, observation_compatible;

// Expected: "WAIT", ["corridor_w:east"], true
// A later localizer report reuses the same committed step and output prefix.
// Replay from the original support, with one Mealy output for each completed action.
CALL sync.validate_observed_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"],
  1,
  ["west_landmark"],
  ["corridor_w:east"],
  true
)
YIELD status, decision, expected_hypotheses, observation_compatible,
      failed_observation_step, observed_output, expected_outputs
RETURN status, decision, expected_hypotheses, observation_compatible,
       failed_observation_step, observed_output, expected_outputs;

// Expected: "OK", "CONTINUE", ["corridor_w:east"], true, null, null, []

// After committing go_west -> dock, replay the full prefix from the original bays.
CALL sync.validate_observed_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"], 2, ["west_landmark", "dock"], ["dock:north"], true
)
YIELD decision, expected_hypotheses, observation_compatible
RETURN decision, expected_hypotheses, observation_compatible;

// Expected: "CONTINUE", ["dock:north"], true. The word is exhausted.
// Independent alternative history: localization cannot override an impossible output.
CALL sync.validate_observed_update(
  "warehouse", 1,
  ["west_bay:east", "east_bay:west"],
  ["to_corridor", "go_west"],
  1,
  ["symmetric"],
  ["corridor_w:east", "corridor_e:west"],
  true
)
YIELD decision, expected_hypotheses, observation_compatible,
      failed_observation_step, observed_output, expected_outputs
RETURN decision, expected_hypotheses, observation_compatible,
       failed_observation_step, observed_output, expected_outputs;

// Expected: "MODEL_VIOLATION", [], false, 1, "symmetric", ["west_landmark", "east_landmark"]

// Another independent history: localization shrinks the output-conditioned support.
CALL sync.validate_observed_update(
  "warehouse", 1, ["west_bay:east", "east_bay:west", "dock:north"],
  ["go_east"], 1, ["symmetric"], ["west_bay:east"], true
)
YIELD decision
WITH decision, ["west_bay:east"] AS accepted_hypotheses
WHERE decision = "REPLAN"
CALL sync.plan_disambiguate("warehouse", accepted_hypotheses, 1, 64)
YIELD outcome, word, generation
RETURN decision, accepted_hypotheses, outcome, word, generation;

// Expected: "REPLAN", ["west_bay:east"], "ALREADY_SATISFIED", [], 1
// The singleton already satisfies homing. A new PLAN would start with step 0 and outputs [].
