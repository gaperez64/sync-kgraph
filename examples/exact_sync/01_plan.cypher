// Both queries below have the same semantic results.
// At budget 3 both models return RESOURCE_BOUND with an empty word.
// At budget 4 the positive model returns PLAN / SUBSET_BFS / ["b", "c"].
// At budget 4 the negative model returns NO_PLAN with an empty word.
UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
UNWIND [3, 4] AS budget
CALL sync.plan_sync_uncached(model, ["q0", "q1", "q2"], budget)
YIELD status, outcome, method, word, final_state_key, final_support_size, expansions, generation
RETURN model, budget, status, outcome, method, word, final_state_key,
       final_support_size, expansions, generation
ORDER BY model, budget;

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
CALL sync.prepare_model(model, false, false)
YIELD status, generation
RETURN model, status, generation;

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
UNWIND [3, 4] AS budget
CALL sync.plan_sync(model, ["q0", "q1", "q2"], budget)
YIELD status, outcome, method, word, final_state_key, final_support_size, expansions, generation
RETURN model, budget, status, outcome, method, word, final_state_key,
       final_support_size, expansions, generation
ORDER BY model, budget;
