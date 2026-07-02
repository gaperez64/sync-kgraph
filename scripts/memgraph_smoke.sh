#!/usr/bin/env sh
set -eu

builddir="${1:-build-memgraph}"
module="$PWD/$builddir/sync.so"
image="${MEMGRAPH_IMAGE:-memgraph/memgraph:3.11.0}"
container="sync-kgraph-smoke-$$"

if [ ! -f "$module" ]; then
  echo "missing Memgraph module: $module" >&2
  exit 2
fi

cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker run \
  -d \
  --name "$container" \
  -v "$module:/usr/lib/memgraph/query_modules/sync.so:ro" \
  "$image" \
  --also-log-to-stderr >/dev/null

run_query() {
  printf '%s\n' "$1" | docker exec -i "$container" mgconsole \
    --host=127.0.0.1 \
    --port=7687 \
    --output_format=csv \
    --no_history
}

run_file() {
  docker exec -i "$container" mgconsole \
    --host=127.0.0.1 \
    --port=7687 \
    --output_format=csv \
    --no_history <"$1"
}

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if run_query "RETURN 1;" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done

if [ "$ready" -ne 1 ]; then
  docker logs "$container" >&2
  echo "Memgraph did not become ready" >&2
  exit 1
fi

run_query 'CALL mg.load("sync");' >/dev/null
run_file examples/office/00_reset_and_load.cypher >/dev/null

validate="$(run_query 'CALL sync.validate_model("office") YIELD ok, code RETURN ok, code;')"
printf '%s\n' "$validate" | grep -q "true"
printf '%s\n' "$validate" | grep -q "OK"

oracle="$(run_query 'CALL sync.build_pair_oracle("office") YIELD status, pairs, pair_edges, mergeable_pairs, materialized RETURN status, pairs, pair_edges, mergeable_pairs, materialized;')"
printf '%s\n' "$oracle" | grep -q "OK"
printf '%s\n' "$oracle" | grep -q "true"

pairs="$(run_query 'MATCH (p:SyncPair {model: "office"}) RETURN count(p) AS pairs;')"
printf '%s\n' "$pairs" | grep -q "6"

pair_edges="$(run_query 'MATCH (:SyncPair {model: "office"})-[r:PAIR_NEXT {model: "office"}]->(:SyncPair {model: "office"}) RETURN count(r) AS pair_next;')"
printf '%s\n' "$pair_edges" | grep -q "12"

word="$(run_query 'CALL sync.word_to_target("office", ["A", "B"], ["C"], "REACH_AND_SYNC", 64) YIELD status, word, length, final_state_key RETURN status, word, length, final_state_key;')"
printf '%s\n' "$word" | grep -q "PAIR_GREEDY_TARGETED"
printf '%s\n' "$word" | grep -q "north"
printf '%s\n' "$word" | grep -q "C"

cache="$(run_query 'CALL sync.expand_cache("office", ["C"], "REACH_AND_SYNC", 64) YIELD status, expanded, cache_size RETURN status, expanded, cache_size;')"
printf '%s\n' "$cache" | grep -q "OK"

subsets="$(run_query 'MATCH (s:SyncSubset {model: "office", mode: "REACH_AND_SYNC", target_key: "C"}) RETURN count(s) AS subsets;')"
printf '%s\n' "$subsets" | grep -q "3"

echo "Memgraph smoke test passed"
