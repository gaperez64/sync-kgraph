CALL sync.validate_model("office")
YIELD ok, code, message, states, letters, transitions
RETURN ok, code, message, states, letters, transitions;

// Expected:
// ok: true
// code: "OK"
// message: "model is complete and deterministic"
// states: 3
// letters: 2
// transitions: 6
