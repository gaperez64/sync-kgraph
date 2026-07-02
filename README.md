# sync-kgraph

`sync-kgraph` implements schema-agnostic synchronizing-word queries over a
deterministic automaton view of a knowledge graph. The core algorithms are C23
and build with Meson. Query/view glue is Cypher, and the native Memgraph module
is compiled from C when Memgraph's `mg_procedure.h` is available.

Implemented pieces:

- DFA view validation for completeness and determinism.
- Reverse transition/preimage support.
- Reverse pair graph with BFS pair-compression witnesses, materialized as
  `SyncPair`, `PAIR_NEXT`, and `PAIR_PRE`.
- Greedy synchronizing-word construction for hypothesis sets.
- Target validation for `SYNC`, `REACH`, and `REACH_AND_SYNC`.
- Bounded reverse-subset expansion for exact reachability cases, materialized
  as `SyncSubset`.
- Step-by-step explanation of active hypotheses after each action.

## Build

```sh
CC=clang meson setup build -Dmemgraph=disabled
ninja -C build
meson test -C build --print-errorlogs
sh scripts/valgrind.sh build-valgrind
./build/sync-kgraph-cli --example office
```

To build the Memgraph module, point Meson at the Memgraph C API header:

```sh
CC=clang meson setup build-memgraph \
  -Dmemgraph=enabled \
  -Dmemgraph_include_dir=/usr/include/memgraph
ninja -C build-memgraph
```

For a locally installed Memgraph, prefer the installed header so the module is
compiled against the server ABI:

```sh
CC=clang meson setup build-local-memgraph \
  -Dmemgraph=enabled \
  -Dmemgraph_include_dir=/usr/include/memgraph
ninja -C build-local-memgraph
```

The module artifact is `sync.so`. Install it into Memgraph's query module
directory, then load it:

```cypher
CALL mg.load("sync");
CALL mg.procedures() YIELD name
WHERE name STARTS WITH "sync."
RETURN name
ORDER BY name;
```

To smoke-test a local server after loading the module:

```sh
MEMGRAPH_PORT=7687 sh scripts/memgraph_local_smoke.sh
```

The smoke test uses the temporary model name `sync_kgraph_smoke` and deletes
only nodes with that exact `model` property before and after the test.

## Memgraph View Contract

The application schema is not rewritten. A deployment manually materializes the
automaton view into Sync-KGraph's namespace:

```cypher
(:SyncModel {model, generation, dirty})
(:SyncState {model, state_key, state_id, base_id?})
(:SyncLetter {model, letter, letter_id})
(:SyncState)-[:SYNC_TRANS {model, letter}]->(:SyncState)
```

The native module writes auxiliary view objects:

```cypher
(:SyncPair {model, pair_id, first_key, second_key, distance, has_witness,
            witness, next_pair, generation})
(:SyncPair)-[:PAIR_NEXT {model, letter, letter_id}]->(:SyncPair)
(:SyncPair)-[:PAIR_PRE {model, letter, letter_id}]->(:SyncPair)
(:SyncSubset {model, mode, target_key, subset_key, word, size,
              word_length, generation})
```

Run `cypher/install_schema.cypher` once for indexes. Then map existing graph
objects into `SyncState`, map symbolic commands into `SyncLetter`, and create
one `SYNC_TRANS` relationship for every `(state, letter)` pair. The C module
expects the materialized view to be complete and deterministic unless
`sync.build_model(model, true)` is used to complete missing transitions with a
sink state in memory.

Primary procedures:

```cypher
CALL sync.validate_model(model)
CALL sync.build_pair_oracle(model, materialize)
CALL sync.word_for_set(model, state_keys, target_keys, mode, budget)
CALL sync.word_to_target(model, state_keys, target_keys, mode, budget)
CALL sync.expand_cache(model, target_keys, mode, budget)
CALL sync.explain(model, state_keys, word)
CALL sync.mark_dirty(model)
```

The module does not execute arbitrary user Cypher strings. Compute hypotheses
with normal Cypher, collect their `state_key` values, then pass that list to
`sync.word_for_set` or `sync.word_to_target`. The `materialize` argument to
`sync.build_pair_oracle` defaults to `true`.

## Worked Example

Load the office automaton:

```cypher
\i examples/office/00_reset_and_load.cypher
```

It resets only Sync-KGraph view objects with `model: "office"`, then creates
states `A`, `B`, `C` and letters `north`, `east`.

Transitions:

```text
north: A -> B, B -> C, C -> C
east:  A -> A, B -> B, C -> C
```

Validate the view:

```cypher
CALL sync.validate_model("office")
YIELD ok, code, message, states, letters, transitions
RETURN ok, code, message, states, letters, transitions;
```

Expected:

```text
ok: true
code: "OK"
message: "model is complete and deterministic"
states: 3
letters: 2
transitions: 6
```

Build/check the pair oracle:

```cypher
CALL sync.build_pair_oracle("office")
YIELD status, pairs, pair_edges, mergeable_pairs, materialized, generation
RETURN status, pairs, pair_edges, mergeable_pairs, materialized, generation;
```

Expected:

```text
status: "OK"
pairs: 6
pair_edges: 12
mergeable_pairs: 6
materialized: true
generation: 0
```

Inspect the materialized pair graph:

```cypher
MATCH (p:SyncPair {model: "office"})
RETURN count(p) AS pairs;
```

Expected:

```text
pairs: 6
```

```cypher
MATCH (:SyncPair {model: "office"})-[r:PAIR_NEXT {model: "office"}]->
      (:SyncPair {model: "office"})
RETURN count(r) AS pair_next;
```

Expected:

```text
pair_next: 12
```

Ask for a word from hypothesis `["A", "B"]` to target `["C"]`:

```cypher
CALL sync.word_to_target("office", ["A", "B"], ["C"], "REACH_AND_SYNC", 64)
YIELD status, word, length, final_state_key, generation
RETURN status, word, length, final_state_key, generation;
```

Expected:

```text
status: "PAIR_GREEDY_TARGETED"
word: ["north", "north"]
length: 2
final_state_key: "C"
generation: 0
```

Explain the returned word:

```cypher
CALL sync.explain("office", ["A", "B"], ["north", "north"])
YIELD step, letter, active_state_keys
RETURN step, letter, active_state_keys
ORDER BY step;
```

Expected:

```text
step: 0, letter: "", active_state_keys: ["A", "B"]
step: 1, letter: "north", active_state_keys: ["B", "C"]
step: 2, letter: "north", active_state_keys: ["C"]
```

Expand the reverse-subset cache for target `C`:

```cypher
CALL sync.expand_cache("office", ["C"], "REACH_AND_SYNC", 64)
YIELD status, expanded, cache_size
RETURN status, expanded, cache_size;
```

Expected:

```text
status: "OK"
expanded: 3
cache_size: 3
```

Inspect the persisted cache:

```cypher
MATCH (s:SyncSubset {model: "office", mode: "REACH_AND_SYNC", target_key: "C"})
RETURN count(s) AS subsets;
```

Expected:

```text
subsets: 3
```

The same example can be checked without Memgraph:

```sh
./build/sync-kgraph-cli --example office
```

## Quality Gates

CI runs `clang-format`, `clang-tidy`, `meson test`, Valgrind leak checks,
`gcovr` with a minimum line coverage gate of 75%, and a Dockerized Memgraph
module smoke test. Release tags publish:

- `sync-kgraph-linux-x86_64.tar.gz`
- `sync-kgraph-macos-arm64.tar.gz`

Each bundle contains the native binary artifacts, Cypher scripts, the GSS view,
the office example, `README.md`, `LICENSE`, and a SHA-256 checksum.
