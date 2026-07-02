CALL sync.explain("office", ["A", "B"], ["north", "north"])
YIELD step, letter, active_state_keys
RETURN step, letter, active_state_keys
ORDER BY step;

// Expected:
// step: 0, letter: "", active_state_keys: ["A", "B"]
// step: 1, letter: "north", active_state_keys: ["B", "C"]
// step: 2, letter: "north", active_state_keys: ["C"]
