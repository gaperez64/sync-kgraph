// Sync-KGraph auxiliary indexes and constraints.
// Run this once in an existing Memgraph database before loading a model view.

CREATE INDEX ON :SyncModel(model);
CREATE INDEX ON :SyncState(model);
CREATE INDEX ON :SyncState(state_key);
CREATE INDEX ON :SyncLetter(model);
CREATE INDEX ON :SyncPair(model);
CREATE INDEX ON :SyncPair(pair_id);
CREATE INDEX ON :SyncSubset(model);
CREATE INDEX ON :SyncSubset(subset_key);

// The application owns the source schema. Sync-KGraph only requires these
// materialized view objects:
//
// (:SyncModel {model, generation, dirty})
// (:SyncState {model, state_key, state_id, base_id?})
// (:SyncLetter {model, letter, letter_id})
// (:SyncState)-[:SYNC_TRANS {model, letter}]->(:SyncState)
//
// The native module materializes:
// (:SyncPair {model, pair_id, first_key, second_key, distance, has_witness,
//             witness, next_pair, generation})
// (:SyncPair)-[:PAIR_NEXT {model, letter, letter_id}]->(:SyncPair)
// (:SyncPair)-[:PAIR_PRE {model, letter, letter_id}]->(:SyncPair)
// (:SyncSubset {model, mode, target_key, subset_key, word, size,
//               word_length, generation})
