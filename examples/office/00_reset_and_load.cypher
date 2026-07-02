// Fully materialized office automaton example.
// This reset only touches Sync-KGraph view objects for this example model.

MATCH (n)
WHERE (n:SyncModel OR n:SyncState OR n:SyncLetter OR n:SyncPair OR n:SyncSubset)
  AND n.model = "office"
DETACH DELETE n;

CREATE (:SyncModel {model: "office", generation: 0, dirty: false});

CREATE (:SyncState {model: "office", state_key: "A", state_id: 0});
CREATE (:SyncState {model: "office", state_key: "B", state_id: 1});
CREATE (:SyncState {model: "office", state_key: "C", state_id: 2});

CREATE (:SyncLetter {model: "office", letter: "north", letter_id: 0});
CREATE (:SyncLetter {model: "office", letter: "east", letter_id: 1});

MATCH (a:SyncState {model: "office", state_key: "A"})
MATCH (b:SyncState {model: "office", state_key: "B"})
MATCH (c:SyncState {model: "office", state_key: "C"})
CREATE (a)-[:SYNC_TRANS {model: "office", letter: "north"}]->(b)
CREATE (b)-[:SYNC_TRANS {model: "office", letter: "north"}]->(c)
CREATE (c)-[:SYNC_TRANS {model: "office", letter: "north"}]->(c)
CREATE (a)-[:SYNC_TRANS {model: "office", letter: "east"}]->(a)
CREATE (b)-[:SYNC_TRANS {model: "office", letter: "east"}]->(b)
CREATE (c)-[:SYNC_TRANS {model: "office", letter: "east"}]->(c);
