CALL sync.word_to_target(
  "office",
  ["A", "B"],
  ["C"],
  "REACH_AND_SYNC",
  64
)
YIELD status, word, length, final_state_key, generation
RETURN status, word, length, final_state_key, generation;

// Expected:
// status: "PAIR_GREEDY_TARGETED"
// word: ["north", "north"]
// length: 2
// final_state_key: "C"
// generation: 0
