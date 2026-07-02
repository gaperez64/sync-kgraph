#!/usr/bin/env sh
set -eu

host="${MEMGRAPH_HOST:-127.0.0.1}"
port="${MEMGRAPH_PORT:-7687}"
model="${SYNC_KGRAPH_SMOKE_MODEL:-sync_kgraph_smoke}"

run_query() {
  printf '%s\n' "$1" | mgconsole \
    --host="$host" \
    --port="$port" \
    --output_format=csv \
    --no_history
}

run_query 'CALL mg.load("sync");' >/dev/null

run_query "MATCH (n)
WHERE (n:SyncModel OR n:SyncState OR n:SyncLetter OR n:SyncPair OR n:SyncSubset)
  AND n.model = \"$model\"
DETACH DELETE n;" >/dev/null

run_query "CREATE (:SyncModel {model: \"$model\", generation: 0, dirty: false});
CREATE (:SyncState {model: \"$model\", state_key: \"A\", state_id: 0});
CREATE (:SyncState {model: \"$model\", state_key: \"B\", state_id: 1});
CREATE (:SyncState {model: \"$model\", state_key: \"C\", state_id: 2});
CREATE (:SyncLetter {model: \"$model\", letter: \"north\", letter_id: 0});
CREATE (:SyncLetter {model: \"$model\", letter: \"east\", letter_id: 1});
MATCH (a:SyncState {model: \"$model\", state_key: \"A\"})
MATCH (b:SyncState {model: \"$model\", state_key: \"B\"})
MATCH (c:SyncState {model: \"$model\", state_key: \"C\"})
CREATE (a)-[:SYNC_TRANS {model: \"$model\", letter: \"north\"}]->(b)
CREATE (b)-[:SYNC_TRANS {model: \"$model\", letter: \"north\"}]->(c)
CREATE (c)-[:SYNC_TRANS {model: \"$model\", letter: \"north\"}]->(c)
CREATE (a)-[:SYNC_TRANS {model: \"$model\", letter: \"east\"}]->(a)
CREATE (b)-[:SYNC_TRANS {model: \"$model\", letter: \"east\"}]->(b)
CREATE (c)-[:SYNC_TRANS {model: \"$model\", letter: \"east\"}]->(c);" >/dev/null

validate="$(run_query "CALL sync.validate_model(\"$model\") YIELD ok, code RETURN ok, code;")"
printf '%s\n' "$validate" | grep -q "true"
printf '%s\n' "$validate" | grep -q "OK"

oracle="$(run_query "CALL sync.build_pair_oracle(\"$model\") YIELD status, pairs, pair_edges, mergeable_pairs, materialized RETURN status, pairs, pair_edges, mergeable_pairs, materialized;")"
printf '%s\n' "$oracle" | grep -q "OK"
printf '%s\n' "$oracle" | grep -q "true"

pairs="$(run_query "MATCH (p:SyncPair {model: \"$model\"}) RETURN count(p) AS pairs;")"
printf '%s\n' "$pairs" | grep -q "6"

pair_edges="$(run_query "MATCH (:SyncPair {model: \"$model\"})-[r:PAIR_NEXT {model: \"$model\"}]->(:SyncPair {model: \"$model\"}) RETURN count(r) AS pair_next;")"
printf '%s\n' "$pair_edges" | grep -q "12"

word="$(run_query "CALL sync.word_to_target(\"$model\", [\"A\", \"B\"], [\"C\"], \"REACH_AND_SYNC\", 64) YIELD status, word, length, final_state_key RETURN status, word, length, final_state_key;")"
printf '%s\n' "$word" | grep -q "PAIR_GREEDY_TARGETED"
printf '%s\n' "$word" | grep -q "north"
printf '%s\n' "$word" | grep -q "C"

cache="$(run_query "CALL sync.expand_cache(\"$model\", [\"C\"], \"REACH_AND_SYNC\", 64) YIELD status, expanded, cache_size RETURN status, expanded, cache_size;")"
printf '%s\n' "$cache" | grep -q "OK"

subsets="$(run_query "MATCH (s:SyncSubset {model: \"$model\", mode: \"REACH_AND_SYNC\", target_key: \"C\"}) RETURN count(s) AS subsets;")"
printf '%s\n' "$subsets" | grep -q "3"

run_query "MATCH (n)
WHERE (n:SyncModel OR n:SyncState OR n:SyncLetter OR n:SyncPair OR n:SyncSubset)
  AND n.model = \"$model\"
DETACH DELETE n;" >/dev/null

echo "Local Memgraph smoke test passed"
