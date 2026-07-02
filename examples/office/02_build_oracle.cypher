CALL sync.build_pair_oracle("office")
YIELD status, pairs, pair_edges, mergeable_pairs, generation
RETURN status, pairs, pair_edges, mergeable_pairs, generation;

// Expected:
// status: "OK"
// pairs: 6
// pair_edges: 12
// mergeable_pairs: 6
// generation: 0
