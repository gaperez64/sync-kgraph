// Reset only these two synchronization examples.
MATCH (n)
WHERE n.model IN ["sync_fallback_positive", "sync_fallback_negative"]
  AND (n:SyncModel OR n:SyncState OR n:SyncAction OR n:SyncOutput OR n:SyncPair)
DETACH DELETE n;

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
CREATE (:SyncModel {model: model, generation: 1, dirty: true});

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
UNWIND range(0, 6) AS id
CREATE (:SyncState {model: model, state_key: "q" + toString(id), state_id: id});

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
UNWIND range(0, 2) AS id
CREATE (:SyncAction {model: model, action_key: ["a", "b", "c"][id], action_id: id});

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
CREATE (:SyncOutput {model: model, output_key: "quiet", output_id: 0});

UNWIND ["sync_fallback_positive", "sync_fallback_negative"] AS model
WITH model, [
  [3, 3, 4, 3, 4, 5, 6],
  [5, 6, 5, 3, 4, 5, 6],
  CASE WHEN model = "sync_fallback_positive"
       THEN [4, 3, 3, 3, 4, 3, 3]
       ELSE [4, 3, 3, 3, 4, 5, 6] END
] AS columns
UNWIND range(0, 2) AS action_id
UNWIND range(0, 6) AS state_id
WITH model, state_id, columns[action_id][state_id] AS target_id,
     ["a", "b", "c"][action_id] AS action_key
MATCH (src:SyncState {model: model, state_id: state_id})
MATCH (dst:SyncState {model: model, state_id: target_id})
MATCH (output:SyncOutput {model: model, output_key: "quiet"})
CREATE (src)-[:SYNC_TRANS {model: model, action_key: action_key}]->(dst)
CREATE (src)-[:SYNC_OBS {model: model, action_key: action_key}]->(output);
