// Run 00_reset_and_load.cypher and 01_plan.cypher first.
// Same exact synchronization fallback, confined to the given action alphabet.
CALL sync.plan_sync_allowed("sync_fallback_positive", ["q0", "q1", "q2"], ["a", "b", "c"], 4)
YIELD outcome, method, word, final_state_key, expansions, generation
RETURN outcome, method, word, final_state_key, expansions, generation;

// Removing c makes synchronization impossible. The failed word is empty.
CALL sync.plan_sync_allowed("sync_fallback_positive", ["q0", "q1", "q2"], ["a", "b"], 4)
YIELD outcome, method, word, final_state_key, expansions
RETURN outcome, method, word, final_state_key, expansions;

// The full-alphabet heuristic uses a, so the permitted partition search finds bc.
CALL sync.plan_disambiguate_allowed("sync_fallback_positive", ["q0", "q1", "q2"], 1, ["b", "c"], 2)
YIELD outcome, method, word, worst_support_size, homing
RETURN outcome, method, word, worst_support_size, homing;

// Goal search reaches q3 with bc using three support expansions and no greedy phase.
CALL sync.plan_goal("sync_fallback_positive", ["q0", "q1", "q2"], ["q3"], ["a", "b", "c"], 3)
YIELD outcome, method, word, final_state_keys, final_support_size, expansions
RETURN outcome, method, word, final_state_keys, final_support_size, expansions;

// A goal SET may contain multiple physical states. This objective accepts a.
CALL sync.plan_goal("sync_fallback_positive", ["q0", "q1", "q2"], ["q3", "q4"], ["a", "b", "c"], 1)
YIELD outcome, method, word, final_state_keys, final_support_size
RETURN outcome, method, word, final_state_keys, final_support_size;

// Monitor a restricted plan with the same original support and committed prefix.
CALL sync.validate_observed_update("sync_fallback_positive", 1, ["q0", "q1", "q2"],
  ["b", "c"], 1, ["quiet"], ["q5", "q6"], true)
YIELD decision, expected_hypotheses, generation
RETURN decision, expected_hypotheses, generation;
