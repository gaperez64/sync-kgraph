#include "sync_kgraph/sync.h"

#include "dynamic.h"
#include "snapshot.h"
#include "snapshot_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
      exit(EXIT_FAILURE);                                                                          \
    }                                                                                              \
  } while (0)

enum {
  NUMERIC_STATE_COUNT = 6,
  NUMERIC_ACTION_COUNT = 3,
  NUMERIC_OUTPUT_COUNT = 2,
  NUMERIC_CELL_COUNT = NUMERIC_STATE_COUNT * NUMERIC_ACTION_COUNT,
  NUMERIC_MUTATION_COUNT = 48,
  NUMERIC_MUTATION_STRIDE = 5,
  NUMERIC_ACTION_STRIDE = 7,
  SYNC_TRAP_STATE_COUNT = 7,
  SYNC_TRAP_LEFT = 5,
  SYNC_TRAP_RIGHT = 6,
  SYNC_NEUTRAL_STATE_COUNT = 10,
  SYNC_NEUTRAL_BUDGET = 5,
  SYNC_PERMUTATION_STATE_COUNT = 6,
  SYNC_PERMUTATION_SUPPORT_COUNT = 20,
  SYNC_SMALL_STATE_COUNT = 3,
  SYNC_SMALL_ACTION_COUNT = 2,
  SYNC_SMALL_CELL_COUNT = SYNC_SMALL_STATE_COUNT * SYNC_SMALL_ACTION_COUNT,
  SYNC_SMALL_TABLE_COUNT = 729,
  SYNC_SMALL_SUPPORT_COUNT = 1U << SYNC_SMALL_STATE_COUNT,
};

typedef struct {
  const char *source;
  const char *action;
  const char *target;
  const char *output;
} machine_cell;

typedef struct {
  size_t calls;
  size_t initial_rows;
  size_t action_rows;
  size_t singleton_rows;
} explain_counts;

typedef struct {
  size_t state_count;
  size_t action_count;
  size_t pair_count;
  size_t *first_states;
  size_t *second_states;
  sg_pair_record *records;
  sg_pair_arc *arcs;
  size_t record_reads;
} memory_pair_store;

static sg_status memory_read_records(void *context, const size_t *pair_ids, size_t pair_count,
                                     sg_pair_record *records) {
  memory_pair_store *store = context;
  ++store->record_reads;
  for (size_t index = 0U; index < pair_count; ++index) {
    if (pair_ids[index] >= store->pair_count) {
      return SG_ERR_INVALID_ARGUMENT;
    }
    records[index] = store->records[pair_ids[index]];
  }
  return SG_OK;
}

static sg_status memory_read_outgoing(void *context, const size_t *source_pairs,
                                      size_t source_count, sg_pair_arc_batch *batch) {
  memory_pair_store *store = context;
  batch->count = source_count * store->action_count;
  batch->items = calloc(batch->count == 0U ? 1U : batch->count, sizeof(*batch->items));
  if (batch->items == NULL) {
    return SG_ERR_ALLOC;
  }
  size_t position = 0U;
  for (size_t source = 0U; source < source_count; ++source) {
    if (source_pairs[source] >= store->pair_count) {
      free(batch->items);
      *batch = (sg_pair_arc_batch){0};
      return SG_ERR_INVALID_ARGUMENT;
    }
    for (size_t action = 0U; action < store->action_count; ++action) {
      batch->items[position] = store->arcs[(source_pairs[source] * store->action_count) + action];
      ++position;
    }
  }
  return SG_OK;
}

static sg_status memory_read_incoming(void *context, const size_t *target_pairs,
                                      size_t target_count, sg_pair_arc_batch *batch) {
  memory_pair_store *store = context;
  bool *targets = calloc(store->pair_count, sizeof(*targets));
  if (targets == NULL) {
    return SG_ERR_ALLOC;
  }
  for (size_t index = 0U; index < target_count; ++index) {
    if (target_pairs[index] >= store->pair_count) {
      free(targets);
      return SG_ERR_INVALID_ARGUMENT;
    }
    targets[target_pairs[index]] = true;
  }
  size_t count = 0U;
  for (size_t edge = 0U; edge < store->pair_count * store->action_count; ++edge) {
    if (targets[store->arcs[edge].target_pair]) {
      ++count;
    }
  }
  batch->items = calloc(count == 0U ? 1U : count, sizeof(*batch->items));
  if (batch->items == NULL) {
    free(targets);
    return SG_ERR_ALLOC;
  }
  batch->count = count;
  size_t position = 0U;
  for (size_t edge = 0U; edge < store->pair_count * store->action_count; ++edge) {
    if (targets[store->arcs[edge].target_pair]) {
      batch->items[position] = store->arcs[edge];
      ++position;
    }
  }
  free(targets);
  return SG_OK;
}

static sg_status memory_write_records(void *context, const sg_pair_record *records,
                                      size_t record_count) {
  memory_pair_store *store = context;
  for (size_t index = 0U; index < record_count; ++index) {
    if (records[index].pair >= store->pair_count) {
      return SG_ERR_INVALID_ARGUMENT;
    }
    store->records[records[index].pair] = records[index];
  }
  return SG_OK;
}

static sg_status snapshot_read_records(void *context, const size_t *pair_ids, size_t pair_count,
                                       sg_pair_record *records) {
  return sg_pair_snapshot_read(context, pair_ids, pair_count, records);
}

typedef struct {
  const sg_pair_oracle *oracle;
  size_t calls;
  size_t fail_on_call;
  sg_status failure;
  bool corrupt;
} faulty_record_source;

static sg_status faulty_read_records(void *context, const size_t *pair_ids, size_t pair_count,
                                     sg_pair_record *records) {
  faulty_record_source *source = context;
  ++source->calls;
  if (source->calls == source->fail_on_call) {
    return source->failure;
  }
  for (size_t index = 0U; index < pair_count; ++index) {
    const sg_status status =
        sg_pair_oracle_record(source->oracle, pair_ids[index], &records[index]);
    if (status != SG_OK) {
      return status;
    }
    if (source->corrupt) {
      records[index].pair = SG_INDEX_NONE;
    }
  }
  return SG_OK;
}

static void add_keys(sg_automaton_builder *builder, const char *const *states, size_t state_count,
                     const char *const *actions, size_t action_count, const char *const *outputs,
                     size_t output_count) {
  for (size_t index = 0U; index < state_count; ++index) {
    CHECK(sg_automaton_builder_add_state(builder, states[index]) == SG_OK);
  }
  for (size_t index = 0U; index < action_count; ++index) {
    CHECK(sg_automaton_builder_add_action(builder, actions[index]) == SG_OK);
  }
  for (size_t index = 0U; index < output_count; ++index) {
    CHECK(sg_automaton_builder_add_output(builder, outputs[index]) == SG_OK);
  }
}

static void add_cells(sg_automaton_builder *builder, const machine_cell *cells, size_t cell_count) {
  for (size_t index = 0U; index < cell_count; ++index) {
    CHECK(sg_automaton_builder_add_transition(builder, cells[index].source, cells[index].action,
                                              cells[index].target) == SG_OK);
    CHECK(sg_automaton_builder_add_observation(builder, cells[index].source, cells[index].action,
                                               cells[index].output) == SG_OK);
  }
}

static sg_automaton *build_warehouse(void) {
  static const char *const states[] = {
      "west_bay:east", "east_bay:west", "corridor_w:east", "corridor_e:west", "dock:north",
  };
  static const char *const actions[] = {
      "to_corridor",
      "to_wall",
      "go_west",
      "go_east",
  };
  static const char *const outputs[] = {
      "west_landmark",
      "east_landmark",
      "symmetric",
      "dock",
  };
  static const machine_cell cells[] = {
      {"west_bay:east", "to_corridor", "corridor_w:east", "west_landmark"},
      {"west_bay:east", "to_wall", "west_bay:east", "symmetric"},
      {"west_bay:east", "go_west", "west_bay:east", "symmetric"},
      {"west_bay:east", "go_east", "west_bay:east", "symmetric"},
      {"east_bay:west", "to_corridor", "corridor_e:west", "east_landmark"},
      {"east_bay:west", "to_wall", "east_bay:west", "symmetric"},
      {"east_bay:west", "go_west", "east_bay:west", "symmetric"},
      {"east_bay:west", "go_east", "east_bay:west", "symmetric"},
      {"corridor_w:east", "to_corridor", "corridor_w:east", "symmetric"},
      {"corridor_w:east", "to_wall", "west_bay:east", "west_landmark"},
      {"corridor_w:east", "go_west", "dock:north", "dock"},
      {"corridor_w:east", "go_east", "corridor_w:east", "symmetric"},
      {"corridor_e:west", "to_corridor", "corridor_e:west", "symmetric"},
      {"corridor_e:west", "to_wall", "east_bay:west", "east_landmark"},
      {"corridor_e:west", "go_west", "dock:north", "dock"},
      {"corridor_e:west", "go_east", "corridor_e:west", "symmetric"},
      {"dock:north", "to_corridor", "dock:north", "dock"},
      {"dock:north", "to_wall", "dock:north", "dock"},
      {"dock:north", "go_west", "dock:north", "dock"},
      {"dock:north", "go_east", "dock:north", "dock"},
  };

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, sizeof(states) / sizeof(states[0]), actions,
           sizeof(actions) / sizeof(actions[0]), outputs, sizeof(outputs) / sizeof(outputs[0]));
  add_cells(builder, cells, sizeof(cells) / sizeof(cells[0]));
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(7), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_automaton *build_two_step_observer(void) {
  static const char *const states[] = {"A", "B", "C"};
  static const char *const actions[] = {"ask_a", "ask_b"};
  static const char *const outputs[] = {"yes", "no"};
  static const machine_cell cells[] = {
      {"A", "ask_a", "A", "yes"}, {"A", "ask_b", "A", "no"}, {"B", "ask_a", "B", "no"},
      {"B", "ask_b", "B", "yes"}, {"C", "ask_a", "C", "no"}, {"C", "ask_b", "C", "no"},
  };

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, sizeof(states) / sizeof(states[0]), actions,
           sizeof(actions) / sizeof(actions[0]), outputs, sizeof(outputs) / sizeof(outputs[0]));
  add_cells(builder, cells, sizeof(cells) / sizeof(cells[0]));
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(11), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_automaton *build_monitor_machine(void) {
  static const char *const states[] = {"q0", "q1", "q2", "q3"};
  static const char *const actions[] = {"forward", "merge"};
  static const char *const outputs[] = {"mpassage", "mjunction", "blocked"};
  static const machine_cell cells[] = {
      {"q0", "forward", "q1", "mpassage"},  {"q1", "forward", "q2", "mpassage"},
      {"q2", "forward", "q3", "mjunction"}, {"q3", "forward", "q0", "blocked"},
      {"q0", "merge", "q3", "mpassage"},    {"q1", "merge", "q3", "mpassage"},
      {"q2", "merge", "q3", "mjunction"},   {"q3", "merge", "q3", "blocked"},
  };
  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, sizeof(states) / sizeof(states[0]), actions,
           sizeof(actions) / sizeof(actions[0]), outputs, sizeof(outputs) / sizeof(outputs[0]));
  add_cells(builder, cells, sizeof(cells) / sizeof(cells[0]));
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(1), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

/* Columns are action-major, deliberately independent of the core's cell layout. */
static sg_automaton *build_silent_machine(const size_t *columns, size_t state_count,
                                          size_t action_count) {
  static const char *const actions[] = {"a", "b", "c", "d"};
  CHECK(action_count <= sizeof(actions) / sizeof(actions[0]));
  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  for (size_t state = 0U; state < state_count; ++state) {
    char key[32] = {0};
    CHECK(snprintf(key, sizeof(key), "q%zu", state) > 0);
    CHECK(sg_automaton_builder_add_state(builder, key) == SG_OK);
  }
  for (size_t action = 0U; action < action_count; ++action) {
    CHECK(sg_automaton_builder_add_action(builder, actions[action]) == SG_OK);
  }
  CHECK(sg_automaton_builder_add_output(builder, "quiet") == SG_OK);
  for (size_t state = 0U; state < state_count; ++state) {
    for (size_t action = 0U; action < action_count; ++action) {
      const size_t target = columns[(action * state_count) + state];
      CHECK(target < state_count);
      char source_key[32] = {0};
      char target_key[32] = {0};
      CHECK(snprintf(source_key, sizeof(source_key), "q%zu", state) > 0);
      CHECK(snprintf(target_key, sizeof(target_key), "q%zu", target) > 0);
      CHECK(sg_automaton_builder_add_transition(builder, source_key, actions[action], target_key) ==
            SG_OK);
      CHECK(sg_automaton_builder_add_observation(builder, source_key, actions[action], "quiet") ==
            SG_OK);
    }
  }
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, UINT64_C(1), &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static sg_automaton *build_sync_trap(bool positive) {
  size_t columns[][SYNC_TRAP_STATE_COUNT] = {
      {3U, 3U, 4U, 3U, 4U, SYNC_TRAP_LEFT, SYNC_TRAP_RIGHT},
      {SYNC_TRAP_LEFT, SYNC_TRAP_RIGHT, SYNC_TRAP_LEFT, 3U, 4U, SYNC_TRAP_LEFT, SYNC_TRAP_RIGHT},
      {4U, 3U, 3U, 3U, 4U, 3U, 3U},
  };
  if (!positive) {
    columns[2][SYNC_TRAP_LEFT] = SYNC_TRAP_LEFT;
    columns[2][SYNC_TRAP_RIGHT] = SYNC_TRAP_RIGHT;
  }
  return build_silent_machine(&columns[0][0], SYNC_TRAP_STATE_COUNT, 3U);
}

static sg_automaton *build_numeric_automaton(const size_t *transitions, const size_t *observations,
                                             uint64_t generation) {
  static const char *const states[] = {"q0", "q1", "q2", "q3", "q4", "q5"};
  static const char *const actions[] = {"a0", "a1", "a2"};
  static const char *const outputs[] = {"o0", "o1"};
  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  add_keys(builder, states, NUMERIC_STATE_COUNT, actions, NUMERIC_ACTION_COUNT, outputs,
           NUMERIC_OUTPUT_COUNT);
  for (size_t state = 0U; state < NUMERIC_STATE_COUNT; ++state) {
    for (size_t action = 0U; action < NUMERIC_ACTION_COUNT; ++action) {
      const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
      CHECK(transitions[cell] < NUMERIC_STATE_COUNT);
      CHECK(observations[cell] < NUMERIC_OUTPUT_COUNT);
      CHECK(sg_automaton_builder_add_transition(builder, states[state], actions[action],
                                                states[transitions[cell]]) == SG_OK);
      CHECK(sg_automaton_builder_add_observation(builder, states[state], actions[action],
                                                 outputs[observations[cell]]) == SG_OK);
    }
  }
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, generation, &automaton) == SG_OK);
  sg_automaton_builder_free(builder);
  return automaton;
}

static void memory_store_init(const sg_pair_oracle *oracle, memory_pair_store *store) {
  store->state_count = NUMERIC_STATE_COUNT;
  store->action_count = NUMERIC_ACTION_COUNT;
  store->pair_count = sg_pair_oracle_pair_count(oracle);
  store->first_states = calloc(store->pair_count, sizeof(*store->first_states));
  store->second_states = calloc(store->pair_count, sizeof(*store->second_states));
  store->records = calloc(store->pair_count, sizeof(*store->records));
  store->arcs = calloc(store->pair_count * store->action_count, sizeof(*store->arcs));
  CHECK(store->first_states != NULL);
  CHECK(store->second_states != NULL);
  CHECK(store->records != NULL);
  CHECK(store->arcs != NULL);
  for (size_t pair = 0U; pair < store->pair_count; ++pair) {
    CHECK(sg_pair_oracle_pair_states(oracle, pair, &store->first_states[pair],
                                     &store->second_states[pair]) == SG_OK);
    CHECK(sg_pair_oracle_record(oracle, pair, &store->records[pair]) == SG_OK);
    for (size_t action = 0U; action < store->action_count; ++action) {
      const size_t edge = (pair * store->action_count) + action;
      store->arcs[edge].source_pair = pair;
      store->arcs[edge].action = action;
      CHECK(sg_pair_oracle_pair_step(oracle, pair, action, &store->arcs[edge].target_pair,
                                     &store->arcs[edge].outputs_differ) == SG_OK);
    }
  }
}

static void memory_store_free(memory_pair_store *store) {
  free(store->first_states);
  free(store->second_states);
  free(store->records);
  free(store->arcs);
  *store = (memory_pair_store){0};
}

static void check_pair_records_equal(const sg_pair_record *first, const sg_pair_record *second) {
  CHECK(first->pair == second->pair);
  CHECK(first->mergeable == second->mergeable);
  CHECK(first->merge_distance == second->merge_distance);
  CHECK(first->merge_action == second->merge_action);
  CHECK(first->merge_next_pair == second->merge_next_pair);
  CHECK(first->merge_support_count == second->merge_support_count);
  CHECK(first->resolvable == second->resolvable);
  CHECK(first->resolution_distance == second->resolution_distance);
  CHECK(first->resolution_action == second->resolution_action);
  CHECK(first->resolution_next_pair == second->resolution_next_pair);
  CHECK(first->resolution_support_count == second->resolution_support_count);
}

static size_t state_id(const sg_automaton *automaton, const char *key) {
  size_t state = SG_INDEX_NONE;
  CHECK(sg_automaton_find_state(automaton, key, &state) == SG_OK);
  return state;
}

static size_t action_id(const sg_automaton *automaton, const char *key) {
  size_t action = SG_INDEX_NONE;
  CHECK(sg_automaton_find_action(automaton, key, &action) == SG_OK);
  return action;
}

static sg_pair_oracle *restore_oracle(const sg_automaton *automaton, const sg_pair_oracle *source) {
  const size_t pair_count = sg_pair_oracle_pair_count(source);
  sg_pair_record *records = calloc(pair_count, sizeof(*records));
  CHECK(records != NULL);
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    CHECK(sg_pair_oracle_record(source, pair, &records[pair]) == SG_OK);
  }
  sg_pair_oracle *restored = NULL;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_OK);
  free(records);
  return restored;
}

static void check_plan_semantics_equal(const sg_plan_result *first, const sg_plan_result *second) {
  CHECK(first->outcome == second->outcome);
  CHECK(first->method == second->method);
  CHECK(first->word.length == second->word.length);
  for (size_t index = 0U; index < first->word.length; ++index) {
    CHECK(first->word.actions[index] == second->word.actions[index]);
  }
  CHECK(first->final_state == second->final_state);
  CHECK(first->final_support_size == second->final_support_size);
  CHECK(first->best_support_size == second->best_support_size);
  CHECK(first->worst_support_size == second->worst_support_size);
  CHECK(first->branch_count == second->branch_count);
  CHECK(first->expansions == second->expansions);
  CHECK(first->homing == second->homing);
  CHECK(first->generation == second->generation);
}

static sg_status count_explanation(void *context, size_t step, size_t action,
                                   const size_t *predicted_states, size_t predicted_count,
                                   const size_t *output_trace, size_t trace_length,
                                   const size_t *branch_states, size_t branch_count) {
  explain_counts *counts = context;
  CHECK(counts != NULL);
  CHECK(predicted_states != NULL);
  CHECK(predicted_count != 0U);
  CHECK(branch_states != NULL);
  CHECK(branch_count != 0U);
  CHECK(output_trace != NULL || trace_length == 0U);
  ++counts->calls;
  if (step == 0U) {
    CHECK(action == SG_INDEX_NONE);
    CHECK(trace_length == 0U);
    ++counts->initial_rows;
  } else {
    CHECK(action != SG_INDEX_NONE);
    CHECK(trace_length == step);
    ++counts->action_rows;
  }
  if (branch_count == 1U) {
    ++counts->singleton_rows;
  }
  return SG_OK;
}

static void test_names_and_builder_validation(void) {
  CHECK(strcmp(sg_status_name(SG_ERR_STALE_GENERATION), "STALE_GENERATION") == 0);
  CHECK(strcmp(sg_plan_outcome_name(SG_OUTCOME_RESOURCE_BOUND), "RESOURCE_BOUND") == 0);
  CHECK(strcmp(sg_plan_method_name(SG_METHOD_PARTITION_BFS), "PARTITION_BFS") == 0);
  CHECK(strcmp(sg_plan_method_name(SG_METHOD_SUBSET_BFS), "SUBSET_BFS") == 0);
  CHECK(SG_METHOD_PARTITION_BFS == 3 && SG_METHOD_SUBSET_BFS == 4);
  CHECK(strcmp(sg_monitor_decision_name(SG_MONITOR_MODEL_VIOLATION), "MODEL_VIOLATION") == 0);

  sg_automaton_builder *builder = NULL;
  CHECK(sg_automaton_builder_init(&builder) == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "S") == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "T") == SG_OK);
  CHECK(sg_automaton_builder_add_state(builder, "S") == SG_ERR_DUPLICATE);
  CHECK(sg_automaton_builder_add_action(builder, "a") == SG_OK);
  CHECK(sg_automaton_builder_add_output(builder, "quiet") == SG_OK);
  CHECK(sg_automaton_builder_add_transition(builder, "S", "a", "T") == SG_OK);
  CHECK(sg_automaton_builder_add_observation(builder, "S", "a", "quiet") == SG_OK);
  sg_automaton *automaton = NULL;
  CHECK(sg_automaton_builder_build(builder, 1U, &automaton) == SG_ERR_INCOMPLETE);
  CHECK(automaton == NULL);
  CHECK(sg_automaton_builder_add_transition(builder, "T", "a", "T") == SG_OK);
  CHECK(sg_automaton_builder_add_transition(builder, "T", "a", "S") == SG_OK);
  CHECK(sg_automaton_builder_add_observation(builder, "T", "a", "quiet") == SG_OK);
  CHECK(sg_automaton_builder_build(builder, 1U, &automaton) == SG_ERR_NONDETERMINISTIC);
  sg_automaton_builder_free(builder);
}

static void test_automaton_and_oracle(void) {
  sg_automaton *automaton = build_warehouse();
  CHECK(sg_automaton_generation(automaton) == 7U);
  CHECK(sg_automaton_state_count(automaton) == 5U);
  CHECK(sg_automaton_action_count(automaton) == 4U);
  CHECK(sg_automaton_output_count(automaton) == 4U);
  CHECK(sg_automaton_transition_count(automaton) == 20U);
  CHECK(sg_automaton_find_state(automaton, "missing", &(size_t){0U}) == SG_ERR_NOT_FOUND);

  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  CHECK(sg_pair_oracle_pair_count(oracle) == 15U);
  CHECK(sg_pair_oracle_pair_edge_count(oracle) == 60U);
  CHECK(sg_pair_oracle_mergeable_pair_count(oracle) == 15U);
  CHECK(sg_pair_oracle_resolvable_pair_count(oracle) == 15U);

  const size_t west = state_id(automaton, "west_bay:east");
  const size_t east = state_id(automaton, "east_bay:west");
  const size_t to_corridor = action_id(automaton, "to_corridor");
  size_t west_east_pair = SG_INDEX_NONE;
  for (size_t pair = 0U; pair < sg_pair_oracle_pair_count(oracle); ++pair) {
    size_t first = 0U;
    size_t second = 0U;
    CHECK(sg_pair_oracle_pair_states(oracle, pair, &first, &second) == SG_OK);
    if (first == west && second == east) {
      west_east_pair = pair;
    }
  }
  CHECK(west_east_pair != SG_INDEX_NONE);
  bool outputs_differ = false;
  size_t next_pair = SG_INDEX_NONE;
  CHECK(sg_pair_oracle_pair_step(oracle, west_east_pair, to_corridor, &next_pair,
                                 &outputs_differ) == SG_OK);
  CHECK(outputs_differ);
  CHECK(next_pair != SG_INDEX_NONE);

  sg_word merge = {0};
  CHECK(sg_pair_oracle_merge_word(oracle, west, east, &merge) == SG_OK);
  CHECK(merge.length == 2U);
  CHECK(strcmp(sg_automaton_action_key(automaton, merge.actions[0]), "to_corridor") == 0);
  CHECK(strcmp(sg_automaton_action_key(automaton, merge.actions[1]), "go_west") == 0);
  sg_word_free(&merge);

  sg_word resolution = {0};
  CHECK(sg_pair_oracle_resolution_word(oracle, west, east, &resolution) == SG_OK);
  CHECK(resolution.length == 1U);
  CHECK(resolution.actions[0] == to_corridor);
  sg_word_free(&resolution);

  const size_t pair_count = sg_pair_oracle_pair_count(oracle);
  sg_pair_record *records = calloc(pair_count, sizeof(*records));
  CHECK(records != NULL);
  for (size_t pair = 0U; pair < pair_count; ++pair) {
    CHECK(sg_pair_oracle_record(oracle, pair, &records[pair]) == SG_OK);
  }
  sg_pair_oracle *restored = NULL;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_OK);
  CHECK(sg_pair_oracle_mergeable_pair_count(restored) == pair_count);
  sg_pair_oracle_free(restored);
  records[west_east_pair].resolution_action = SG_INDEX_NONE;
  CHECK(sg_pair_oracle_restore(automaton, records, pair_count, &restored) == SG_ERR_INVALID_MODEL);
  free(records);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_planners_explanation_and_monitor(void) {
  sg_automaton *automaton = build_warehouse();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t initial[] = {
      state_id(automaton, "west_bay:east"),
      state_id(automaton, "east_bay:west"),
  };

  sg_plan_result sync = {0};
  CHECK(sg_plan_sync(automaton, oracle, initial, 2U, 16U, &sync) == SG_OK);
  CHECK(sync.outcome == SG_OUTCOME_PLAN);
  CHECK(sync.method == SG_METHOD_PAIR_MERGE);
  CHECK(sync.word.length == 2U);
  CHECK(sync.final_state == state_id(automaton, "dock:north"));
  CHECK(sync.final_support_size == 1U);
  CHECK(sync.generation == 7U);

  size_t final_states[sizeof(initial) / sizeof(initial[0])] = {0U};
  size_t final_count = 0U;
  CHECK(sg_apply_word(automaton, initial, 2U, &sync.word, final_states, &final_count) == SG_OK);
  CHECK(final_count == 1U);
  CHECK(final_states[0] == sync.final_state);

  explain_counts explanation = {0};
  CHECK(sg_explain_plan(automaton, sync.generation, initial, 2U, &sync.word, count_explanation,
                        &explanation) == SG_OK);
  CHECK(explanation.calls == 5U);
  CHECK(explanation.initial_rows == 1U);
  CHECK(explanation.action_rows == 4U);
  CHECK(explanation.singleton_rows == 4U);
  CHECK(sg_explain_plan(automaton, sync.generation + 1U, initial, 2U, &sync.word, count_explanation,
                        &explanation) == SG_ERR_STALE_GENERATION);

  const size_t corridor[] = {
      state_id(automaton, "corridor_w:east"),
      state_id(automaton, "corridor_e:west"),
  };
  sg_monitor_result monitor = {0};
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, corridor, 2U,
                           true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(monitor.expected_count == 2U);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, corridor, 1U,
                           true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_REPLAN);
  sg_monitor_result_free(&monitor);
  const size_t dock[] = {state_id(automaton, "dock:north")};
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, dock, 1U, true,
                           &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(monitor.unexpected_count == 1U);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation, initial, 2U, &sync.word, 1U, NULL, 0U, false,
                           &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_WAIT);
  sg_monitor_result_free(&monitor);
  CHECK(sg_validate_update(automaton, sync.generation + 1U, initial, 2U, &sync.word, 1U, corridor,
                           2U, true, &monitor) == SG_OK);
  CHECK(monitor.decision == SG_MONITOR_STALE_GENERATION);
  sg_monitor_result_free(&monitor);

  sg_plan_result disambiguation = {0};
  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 2U, 1U, 16U, &disambiguation) == SG_OK);
  CHECK(disambiguation.outcome == SG_OUTCOME_PLAN);
  CHECK(disambiguation.method == SG_METHOD_PAIR_RESOLUTION);
  CHECK(disambiguation.word.length == 1U);
  CHECK(disambiguation.word.actions[0] == action_id(automaton, "to_corridor"));
  CHECK(disambiguation.branch_count == 2U);
  CHECK(disambiguation.worst_support_size == 1U);
  CHECK(disambiguation.homing);

  sg_plan_result_free(&disambiguation);
  sg_plan_result_free(&sync);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_observed_monitor(void) {
  sg_automaton *automaton = build_monitor_machine();
  const size_t initial[] = {0U, 1U, 2U};
  const size_t motion_states[] = {1U, 2U, 3U};
  const size_t filtered_states[] = {1U, 2U};
  const size_t wrong_states[] = {3U};
  const size_t passage[] = {0U};
  const size_t blocked[] = {2U};
  size_t actions[] = {0U};
  const sg_word word = {.actions = actions, .length = 1U};
  sg_monitor_result motion = {0};
  CHECK(sg_validate_update(automaton, 1U, initial, 3U, &word, 1U, motion_states, 3U, true,
                           &motion) == SG_OK);
  CHECK(motion.decision == SG_MONITOR_CONTINUE);
  CHECK(motion.expected_count == 3U);
  sg_monitor_result_free(&motion);

  sg_observed_monitor_result result = {0};
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, passage, 1U,
                                    filtered_states, 2U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(result.observation_compatible);
  CHECK(result.failed_observation_step == SG_INDEX_NONE);
  CHECK(result.observed_output == SG_INDEX_NONE);
  CHECK(result.expected_output_count == 0U);
  CHECK(result.monitor.expected_count == 2U);
  CHECK(memcmp(result.monitor.expected_states, filtered_states, sizeof(filtered_states)) == 0);
  CHECK(result.monitor.unexpected_count == 0U);
  CHECK(result.monitor.generation == 1U);
  sg_observed_monitor_result_free(&result);

  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, passage, 1U,
                                    filtered_states, 1U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_REPLAN);
  CHECK(result.observation_compatible);
  CHECK(result.monitor.expected_count == 2U);
  CHECK(result.monitor.unexpected_count == 0U);
  sg_observed_monitor_result_free(&result);

  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, passage, 1U,
                                    wrong_states, 1U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(result.observation_compatible);
  CHECK(result.monitor.expected_count == 2U);
  CHECK(result.monitor.unexpected_count == 1U);
  CHECK(result.monitor.unexpected_states[0] == wrong_states[0]);
  sg_observed_monitor_result_free(&result);

  /* Log lines 51--53: a motion-exact localizer cannot override an impossible output. */
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, blocked, 1U,
                                    motion_states, 3U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(!result.observation_compatible);
  CHECK(result.monitor.expected_count == 0U);
  CHECK(result.monitor.expected_states == NULL);
  CHECK(result.monitor.unexpected_count == 3U);
  CHECK(result.failed_observation_step == 1U);
  CHECK(result.observed_output == blocked[0]);
  CHECK(result.expected_output_count == 2U);
  CHECK(result.expected_outputs[0] == 0U);
  CHECK(result.expected_outputs[1] == 1U);
  sg_observed_monitor_result_free(&result);

  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, passage, 1U, NULL, 0U,
                                    false, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_WAIT);
  CHECK(result.observation_compatible);
  CHECK(result.monitor.expected_count == 2U);
  CHECK(memcmp(result.monitor.expected_states, filtered_states, sizeof(filtered_states)) == 0);
  CHECK(result.monitor.unexpected_count == 0U);
  sg_observed_monitor_result_free(&result);

  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, blocked, 1U, NULL, 0U,
                                    false, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(!result.observation_compatible);
  CHECK(result.monitor.expected_count == 0U);
  CHECK(result.monitor.unexpected_count == 0U);
  CHECK(result.failed_observation_step == 1U);
  CHECK(result.expected_output_count == 2U);
  sg_observed_monitor_result_free(&result);

  CHECK(sg_validate_observed_update(automaton, 1U, initial, 3U, &word, 1U, passage, 1U, NULL, 0U,
                                    true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_REPLAN);
  CHECK(result.observation_compatible);
  sg_observed_monitor_result_free(&result);
  sg_observed_monitor_result_free(&result);
  sg_observed_monitor_result_free(NULL);
  sg_automaton_free(automaton);
}

static void test_observed_monitor_prefixes(void) {
  sg_automaton *automaton = build_monitor_machine();
  const size_t initial[] = {0U, 1U, 2U, 0U};
  size_t actions[] = {0U, 0U, 0U};
  sg_word word = {.actions = actions, .length = 3U};
  const size_t outputs[] = {0U, 1U};
  const size_t terminal[] = {3U, 3U};
  sg_observed_monitor_result result = {0};
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 4U, &word, 2U, outputs, 2U, terminal,
                                    2U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(result.observation_compatible);
  CHECK(result.monitor.expected_count == 1U);
  CHECK(result.monitor.expected_states[0] == 3U);
  sg_observed_monitor_result_free(&result);

  /* After mjunction only q3 remains: mpassage is impossible at step 2. */
  const size_t incompatible[] = {1U, 0U, 1U};
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 4U, &word, 3U, incompatible, 3U, NULL,
                                    0U, false, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_MODEL_VIOLATION);
  CHECK(!result.observation_compatible);
  CHECK(result.failed_observation_step == 2U);
  CHECK(result.observed_output == 0U);
  CHECK(result.expected_output_count == 1U);
  CHECK(result.expected_outputs[0] == 2U);
  CHECK(result.monitor.expected_count == 0U);
  sg_observed_monitor_result_free(&result);

  /* Filtering precedes merging; equal successors and duplicate inputs collapse. */
  actions[0] = 1U;
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 4U, &word, 1U, outputs, 1U, terminal,
                                    2U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(result.monitor.expected_count == 1U);
  CHECK(result.monitor.expected_states[0] == 3U);
  sg_observed_monitor_result_free(&result);

  /* No completed action: no output is required and the original set is retained. */
  word = (sg_word){0};
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 4U, &word, 0U, NULL, 0U, initial, 4U,
                                    true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_CONTINUE);
  CHECK(result.observation_compatible);
  CHECK(result.monitor.expected_count == 3U);
  sg_observed_monitor_result_free(&result);
  sg_automaton_free(automaton);
}

static void test_observed_monitor_arguments(void) {
  sg_automaton *automaton = build_monitor_machine();
  const size_t initial[] = {0U};
  size_t actions[] = {0U, 0U};
  const sg_word word = {.actions = actions, .length = 2U};
  const sg_word missing_actions = {.length = 2U};
  const size_t outputs[] = {0U, 0U};
  const size_t invalid_outputs[] = {2U, 3U};
  const size_t invalid_state[] = {4U};
  sg_observed_monitor_result result = {0};
  CHECK(sg_validate_observed_update(NULL, 1U, initial, 1U, &word, 1U, outputs, 1U, NULL, 0U, false,
                                    &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, NULL, 1U, &word, 1U, outputs, 1U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 0U, &word, 1U, outputs, 1U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, NULL, 1U, outputs, 1U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 3U, outputs, 3U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &missing_actions, 1U, outputs, 1U,
                                    NULL, 0U, false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, NULL, 1U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 0U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 2U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 1U, NULL, 1U,
                                    true, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 1U, NULL, 0U,
                                    false, NULL) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, invalid_state, 1U, &word, 1U, outputs, 1U, NULL,
                                    0U, false, &result) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 1U,
                                    invalid_state, 1U, true, &result) == SG_ERR_INVALID_ARGUMENT);
  /* Validate even IDs after an impossible first output. */
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 2U, invalid_outputs, 2U,
                                    NULL, 0U, false, &result) == SG_ERR_INVALID_ARGUMENT);
  actions[1] = 2U;
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 2U, outputs, 2U, NULL, 0U,
                                    false, &result) == SG_ERR_INVALID_ARGUMENT);
  /* Unconsumed actions and unavailable reports are ignored by the C monitor. */
  CHECK(sg_validate_observed_update(automaton, 1U, initial, 1U, &word, 1U, outputs, 1U,
                                    invalid_state, 1U, false, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_WAIT);
  sg_observed_monitor_result_free(&result);
  /* Stale plans must not resolve IDs against the replacement model. */
  CHECK(sg_validate_observed_update(automaton, 0U, invalid_state, 1U, &word, 2U, invalid_outputs,
                                    2U, invalid_state, 1U, true, &result) == SG_OK);
  CHECK(result.monitor.decision == SG_MONITOR_STALE_GENERATION);
  CHECK(result.monitor.generation == 1U);
  CHECK(result.monitor.expected_count == 0U);
  CHECK(result.monitor.unexpected_count == 0U);
  CHECK(result.failed_observation_step == SG_INDEX_NONE);
  CHECK(result.observed_output == SG_INDEX_NONE);
  CHECK(result.expected_output_count == 0U);
  sg_observed_monitor_result_free(&result);
  sg_automaton_free(automaton);
}

static void test_exact_partition_search(void) {
  sg_automaton *automaton = build_two_step_observer();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t initial[] = {
      state_id(automaton, "A"),
      state_id(automaton, "B"),
      state_id(automaton, "C"),
  };

  sg_plan_result result = {0};
  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 3U, 1U, 1U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_RESOURCE_BOUND);
  sg_plan_result_free(&result);

  CHECK(sg_plan_disambiguate(automaton, oracle, initial, 3U, 1U, 16U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_PLAN);
  CHECK(result.method == SG_METHOD_PARTITION_BFS);
  CHECK(result.word.length == 2U);
  CHECK(result.worst_support_size == 1U);
  CHECK(result.branch_count == 3U);
  sg_plan_result_free(&result);

  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, 16U, &result) == SG_OK);
  CHECK(result.outcome == SG_OUTCOME_NO_PLAN);
  sg_plan_result_free(&result);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void check_empty_sync_result(const sg_plan_result *result) {
  CHECK(result->word.length == 0U);
  CHECK(result->word.actions == NULL);
  CHECK(result->word.capacity == 0U);
  CHECK(result->method == SG_METHOD_NONE);
  CHECK(result->final_state == SG_INDEX_NONE);
  CHECK(result->final_support_size == 0U);
  CHECK(result->best_support_size == 0U);
  CHECK(result->worst_support_size == 0U);
  CHECK(result->branch_count == 0U);
  CHECK(!result->homing);
  CHECK(result->generation == 1U);
}

static void check_sync_replay(const sg_automaton *automaton, const size_t *initial,
                              size_t initial_count, const sg_plan_result *result) {
  size_t *final = calloc(initial_count, sizeof(*final));
  CHECK(final != NULL);
  size_t final_count = 0U;
  CHECK(sg_apply_word(automaton, initial, initial_count, &result->word, final, &final_count) ==
        SG_OK);
  CHECK(final_count == 1U);
  CHECK(final[0] == result->final_state);
  CHECK(result->final_support_size == 1U);
  free(final);
}

static void test_sync_fallback_case(bool positive) {
  sg_automaton *automaton = build_sync_trap(positive);
  sg_pair_oracle *built = NULL;
  CHECK(sg_pair_oracle_build(automaton, &built) == SG_OK);
  sg_pair_oracle *restored = restore_oracle(automaton, built);
  sg_pair_snapshot *snapshot = NULL;
  CHECK(sg_pair_snapshot_from_oracle(automaton, restored, &snapshot) == SG_OK);
  const sg_pair_record_source source = {.context = snapshot, .read = snapshot_read_records};
  const size_t initial[] = {0U, 1U, 2U};
  const size_t pairs[][2] = {{0U, 1U}, {0U, 2U}, {1U, 2U}};
  for (size_t pair = 0U; pair < 3U; ++pair) {
    sg_word witness = {0};
    CHECK(sg_pair_oracle_merge_word(built, pairs[pair][0], pairs[pair][1], &witness) == SG_OK);
    CHECK(witness.length == 1U);
    CHECK(witness.actions[0] == pair);
    sg_word_free(&witness);
  }
  /* Baseline d83e521 chooses a and returns NO_PLAN with that abandoned prefix.
   * All original pairs merge even in the negative fixture: that is not enough. */
  for (size_t budget = 1U; budget <= 4U; ++budget) {
    sg_plan_result plan = {0};
    sg_plan_result restored_plan = {0};
    sg_plan_result record_plan = {0};
    CHECK(sg_plan_sync(automaton, built, initial, 3U, budget, &plan) == SG_OK);
    CHECK(sg_plan_sync(automaton, restored, initial, 3U, budget, &restored_plan) == SG_OK);
    CHECK(sg_plan_sync_from_records(automaton, &source, initial, 3U, budget, &record_plan) ==
          SG_OK);
    check_plan_semantics_equal(&plan, &restored_plan);
    check_plan_semantics_equal(&plan, &record_plan);
    CHECK(plan.expansions == budget);
    if (budget < 4U) {
      CHECK(plan.outcome == SG_OUTCOME_RESOURCE_BOUND);
      check_empty_sync_result(&plan);
    } else if (positive) {
      CHECK(plan.outcome == SG_OUTCOME_PLAN);
      CHECK(plan.method == SG_METHOD_SUBSET_BFS);
      CHECK(plan.word.length == 2U);
      CHECK(plan.word.actions[0] == 1U && plan.word.actions[1] == 2U);
      CHECK(plan.final_state == 3U);
      CHECK(plan.best_support_size == 1U && plan.worst_support_size == 1U);
      CHECK(plan.branch_count == 1U && plan.homing);
      CHECK(plan.generation == 1U);
      check_sync_replay(automaton, initial, 3U, &plan);
    } else {
      CHECK(plan.outcome == SG_OUTCOME_NO_PLAN);
      check_empty_sync_result(&plan);
    }
    sg_plan_result_free(&plan);
    sg_plan_result_free(&restored_plan);
    sg_plan_result_free(&record_plan);
  }
  sg_pair_snapshot_release(snapshot);
  sg_pair_oracle_free(restored);
  sg_pair_oracle_free(built);
  sg_automaton_free(automaton);
}

static void test_sync_neutral_moves_and_queue_growth(void) {
  static const size_t columns[][SYNC_NEUTRAL_STATE_COUNT] = {
      {3U, 3U, 4U, 3U, 4U, 5U, 6U, 7U, 8U, 9U},
      {7U, 8U, 9U, 3U, 4U, 5U, 6U, 7U, 8U, 9U},
      {4U, 3U, 3U, 3U, 4U, 3U, 3U, 7U, 8U, 9U},
      {0U, 1U, 2U, 3U, 4U, 5U, 6U, 5U, 6U, 5U},
  };
  sg_automaton *automaton = build_silent_machine(&columns[0][0], SYNC_NEUTRAL_STATE_COUNT, 4U);
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t initial[] = {0U, 1U, 2U};
  sg_plan_result plan = {0};
  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, SYNC_NEUTRAL_BUDGET - 1U, &plan) == SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_RESOURCE_BOUND);
  CHECK(plan.expansions == SYNC_NEUTRAL_BUDGET - 1U);
  check_empty_sync_result(&plan);
  sg_plan_result_free(&plan);
  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, SYNC_NEUTRAL_BUDGET, &plan) == SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_PLAN && plan.method == SG_METHOD_SUBSET_BFS);
  CHECK(plan.expansions == SYNC_NEUTRAL_BUDGET);
  CHECK(plan.word.length == 3U);
  CHECK(plan.word.actions[0] == 1U && plan.word.actions[1] == 3U && plan.word.actions[2] == 2U);
  check_sync_replay(automaton, initial, 3U, &plan);
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);

  /* Two permutations generate all 20 three-state supports, forcing queue growth
   * past its initial capacity. Every image has the same cardinality. */
  static const size_t permutations[][SYNC_PERMUTATION_STATE_COUNT] = {{1U, 2U, 3U, 4U, 5U, 0U},
                                                                      {1U, 0U, 2U, 3U, 4U, 5U}};
  automaton = build_silent_machine(&permutations[0][0], SYNC_PERMUTATION_STATE_COUNT, 2U);
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, SYNC_PERMUTATION_SUPPORT_COUNT - 1U, &plan) ==
        SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_RESOURCE_BOUND);
  CHECK(plan.expansions == SYNC_PERMUTATION_SUPPORT_COUNT - 1U);
  check_empty_sync_result(&plan);
  sg_plan_result_free(&plan);
  CHECK(sg_plan_sync(automaton, oracle, initial, 3U, SYNC_PERMUTATION_SUPPORT_COUNT, &plan) ==
        SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_NO_PLAN);
  CHECK(plan.expansions == SYNC_PERMUTATION_SUPPORT_COUNT);
  check_empty_sync_result(&plan);
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

static void test_sync_fallback_inputs_and_errors(void) {
  sg_automaton *automaton = build_sync_trap(true);
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t duplicates[] = {0U, 1U, 2U, 0U};
  const size_t singleton[] = {3U, 3U};
  const size_t invalid[] = {SYNC_TRAP_STATE_COUNT};
  sg_plan_result plan = {0};
  CHECK(sg_plan_sync(automaton, oracle, duplicates, 4U, 4U, &plan) == SG_OK);
  CHECK(plan.method == SG_METHOD_SUBSET_BFS && plan.word.length == 2U);
  CHECK(plan.word.actions[0] == 1U && plan.word.actions[1] == 2U);
  check_sync_replay(automaton, duplicates, 4U, &plan);
  sg_plan_result_free(&plan);
  CHECK(sg_plan_sync(automaton, oracle, singleton, 2U, 1U, &plan) == SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_ALREADY_SATISFIED);
  CHECK(plan.method == SG_METHOD_NONE && plan.word.length == 0U && plan.expansions == 0U);
  CHECK(plan.final_state == 3U && plan.final_support_size == 1U);
  CHECK(plan.best_support_size == 1U && plan.worst_support_size == 1U);
  CHECK(plan.branch_count == 1U && plan.homing);
  check_sync_replay(automaton, singleton, 2U, &plan);
  sg_plan_result_free(&plan);
  CHECK(sg_plan_sync(automaton, oracle, duplicates, 4U, 0U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(automaton, oracle, duplicates, 0U, 1U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(automaton, oracle, NULL, 1U, 1U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(automaton, oracle, invalid, 1U, 1U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(NULL, oracle, duplicates, 4U, 1U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(automaton, NULL, duplicates, 4U, 1U, &plan) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync(automaton, oracle, duplicates, 4U, 1U, NULL) == SG_ERR_INVALID_ARGUMENT);
  CHECK(sg_plan_sync_from_records(automaton, NULL, duplicates, 4U, 1U, &plan) ==
        SG_ERR_INVALID_ARGUMENT);
  static const sg_status failures[] = {SG_ERR_ALLOC, SG_ERR_INVALID_MODEL, SG_ERR_NOT_FOUND,
                                       SG_ERR_STALE_GENERATION, SG_ERR_RESOURCE_BOUND};
  for (size_t failure = 0U; failure < sizeof(failures) / sizeof(failures[0]); ++failure) {
    for (size_t fail_on_call = 1U; fail_on_call <= 3U; ++fail_on_call) {
      faulty_record_source context = {
          .oracle = oracle, .fail_on_call = fail_on_call, .failure = failures[failure]};
      const sg_pair_record_source source = {.context = &context, .read = faulty_read_records};
      CHECK(sg_plan_sync_from_records(automaton, &source, duplicates, 4U, 4U, &plan) ==
            failures[failure]);
      CHECK(context.calls == fail_on_call);
      CHECK(plan.word.length == 0U && plan.word.actions == NULL);
      sg_plan_result_free(&plan);
    }
  }
  faulty_record_source corrupt = {.oracle = oracle, .corrupt = true};
  const sg_pair_record_source source = {.context = &corrupt, .read = faulty_read_records};
  CHECK(sg_plan_sync_from_records(automaton, &source, duplicates, 4U, 4U, &plan) ==
        SG_ERR_INVALID_MODEL);
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);

  automaton = build_two_step_observer();
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const size_t observer_initial[] = {0U, 1U};
  CHECK(sg_plan_disambiguate(automaton, oracle, observer_initial, 2U, 1U, 4U, &plan) == SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_PLAN && plan.homing);
  sg_plan_result_free(&plan);
  CHECK(sg_plan_sync(automaton, oracle, observer_initial, 2U, 1U, &plan) == SG_OK);
  CHECK(plan.outcome == SG_OUTCOME_NO_PLAN && plan.expansions == 1U);
  CHECK(plan.word.length == 0U && plan.final_state == SG_INDEX_NONE);
  sg_plan_result_free(&plan);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
}

/* Deliberately independent of the planner's bitsets, transitions and replay. */
static unsigned small_sync_image(const size_t *columns, unsigned support, size_t action) {
  unsigned image = 0U;
  for (size_t state = 0U; state < SYNC_SMALL_STATE_COUNT; ++state) {
    if ((support & (1U << state)) != 0U) {
      image |= 1U << columns[(action * SYNC_SMALL_STATE_COUNT) + state];
    }
  }
  return image;
}

static bool small_sync_exists(const size_t *columns, unsigned initial) {
  unsigned queue[SYNC_SMALL_SUPPORT_COUNT] = {initial};
  bool visited[SYNC_SMALL_SUPPORT_COUNT] = {false};
  size_t count = 1U;
  visited[initial] = true;
  for (size_t head = 0U; head < count; ++head) {
    const unsigned support = queue[head];
    if ((support & (support - 1U)) == 0U) {
      return true;
    }
    for (size_t action = 0U; action < SYNC_SMALL_ACTION_COUNT; ++action) {
      const unsigned next = small_sync_image(columns, support, action);
      if (!visited[next]) {
        CHECK(count < SYNC_SMALL_SUPPORT_COUNT);
        visited[next] = true;
        queue[count++] = next;
      }
    }
  }
  return false;
}

static void check_small_sync_plan(const size_t *columns, unsigned initial,
                                  const sg_plan_result *plan) {
  unsigned support = initial;
  for (size_t step = 0U; step < plan->word.length; ++step) {
    CHECK(plan->word.actions[step] < SYNC_SMALL_ACTION_COUNT);
    support = small_sync_image(columns, support, plan->word.actions[step]);
  }
  CHECK(support != 0U && (support & (support - 1U)) == 0U);
  CHECK(plan->final_state < SYNC_SMALL_STATE_COUNT);
  CHECK(support == (1U << plan->final_state));
  CHECK(plan->final_support_size == 1U);
  CHECK(plan->generation == 1U);
}

static void test_sync_exhaustive_small_models(void) {
  static const size_t budgets[] = {1U, 64U};
  for (size_t table = 0U; table < SYNC_SMALL_TABLE_COUNT; ++table) {
    size_t columns[SYNC_SMALL_CELL_COUNT] = {0};
    size_t encoding = table;
    for (size_t cell = 0U; cell < SYNC_SMALL_CELL_COUNT; ++cell) {
      columns[cell] = encoding % SYNC_SMALL_STATE_COUNT;
      encoding /= SYNC_SMALL_STATE_COUNT;
    }
    sg_automaton *automaton =
        build_silent_machine(columns, SYNC_SMALL_STATE_COUNT, SYNC_SMALL_ACTION_COUNT);
    sg_pair_oracle *oracle = NULL;
    CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
    for (unsigned support = 1U; support < SYNC_SMALL_SUPPORT_COUNT; ++support) {
      const bool exists = small_sync_exists(columns, support);
      size_t initial[SYNC_SMALL_STATE_COUNT] = {0};
      size_t initial_count = 0U;
      for (size_t state = 0U; state < SYNC_SMALL_STATE_COUNT; ++state) {
        if ((support & (1U << state)) != 0U) {
          initial[initial_count++] = state;
        }
      }
      for (size_t index = 0U; index < sizeof(budgets) / sizeof(budgets[0]); ++index) {
        sg_plan_result plan = {0};
        CHECK(sg_plan_sync(automaton, oracle, initial, initial_count, budgets[index], &plan) ==
              SG_OK);
        CHECK(plan.expansions <= budgets[index]);
        const bool success =
            plan.outcome == SG_OUTCOME_PLAN || plan.outcome == SG_OUTCOME_ALREADY_SATISFIED;
        if (success) {
          CHECK(exists);
          check_small_sync_plan(columns, support, &plan);
        } else {
          CHECK(plan.outcome == SG_OUTCOME_NO_PLAN || plan.outcome == SG_OUTCOME_RESOURCE_BOUND);
          CHECK(plan.outcome != SG_OUTCOME_NO_PLAN || !exists);
          check_empty_sync_result(&plan);
        }
        if (budgets[index] == 64U) {
          CHECK(success == exists);
          CHECK(success || plan.outcome == SG_OUTCOME_NO_PLAN);
        }
        sg_plan_result_free(&plan);
      }
    }
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
  }
}

static void test_oracle_source_equivalence(void) {
  sg_automaton *warehouse = build_warehouse();
  sg_pair_oracle *built = NULL;
  CHECK(sg_pair_oracle_build(warehouse, &built) == SG_OK);
  sg_pair_oracle *restored = restore_oracle(warehouse, built);
  const size_t ambiguous[] = {
      state_id(warehouse, "west_bay:east"),
      state_id(warehouse, "east_bay:west"),
  };
  const size_t singleton[] = {state_id(warehouse, "dock:north")};
  sg_plan_result first = {0};
  sg_plan_result second = {0};

  CHECK(sg_plan_sync(warehouse, built, ambiguous, 2U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(warehouse, restored, ambiguous, 2U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_disambiguate(warehouse, built, ambiguous, 2U, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(warehouse, restored, ambiguous, 2U, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_sync(warehouse, built, singleton, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(warehouse, restored, singleton, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_ALREADY_SATISFIED);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  sg_pair_oracle_free(restored);
  sg_pair_oracle_free(built);
  sg_automaton_free(warehouse);

  sg_automaton *observer = build_two_step_observer();
  CHECK(sg_pair_oracle_build(observer, &built) == SG_OK);
  restored = restore_oracle(observer, built);
  const size_t observer_initial[] = {
      state_id(observer, "A"),
      state_id(observer, "B"),
      state_id(observer, "C"),
  };

  CHECK(sg_plan_disambiguate(observer, built, observer_initial, 3U, 1U, 1U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(observer, restored, observer_initial, 3U, 1U, 1U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_RESOURCE_BOUND);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_disambiguate(observer, built, observer_initial, 3U, 1U, 16U, &first) == SG_OK);
  CHECK(sg_plan_disambiguate(observer, restored, observer_initial, 3U, 1U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.method == SG_METHOD_PARTITION_BFS);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  CHECK(sg_plan_sync(observer, built, observer_initial, 3U, 16U, &first) == SG_OK);
  CHECK(sg_plan_sync(observer, restored, observer_initial, 3U, 16U, &second) == SG_OK);
  check_plan_semantics_equal(&first, &second);
  CHECK(first.outcome == SG_OUTCOME_NO_PLAN);
  sg_plan_result_free(&first);
  sg_plan_result_free(&second);

  sg_pair_oracle_free(restored);
  sg_pair_oracle_free(built);
  sg_automaton_free(observer);
}

static void test_incremental_pair_maintenance(void) {
  size_t transitions[NUMERIC_CELL_COUNT] = {0};
  size_t observations[NUMERIC_CELL_COUNT] = {0};
  for (size_t state = 0U; state < NUMERIC_STATE_COUNT; ++state) {
    for (size_t action = 0U; action < NUMERIC_ACTION_COUNT; ++action) {
      const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
      transitions[cell] = (state + action + 1U) % NUMERIC_STATE_COUNT;
      observations[cell] = (state + action) % NUMERIC_OUTPUT_COUNT;
    }
  }
  sg_automaton *automaton = build_numeric_automaton(transitions, observations, UINT64_C(1));
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  memory_pair_store memory = {0};
  memory_store_init(oracle, &memory);
  sg_pair_store store = {
      .context = &memory,
      .state_count = memory.state_count,
      .action_count = memory.action_count,
      .pair_count = memory.pair_count,
      .read_records = memory_read_records,
      .read_outgoing = memory_read_outgoing,
      .read_incoming = memory_read_incoming,
      .write_records = memory_write_records,
  };
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);

  for (size_t step = 0U; step < NUMERIC_MUTATION_COUNT; ++step) {
    const size_t state = ((step * NUMERIC_MUTATION_STRIDE) + 1U) % NUMERIC_STATE_COUNT;
    const size_t action = ((step * NUMERIC_ACTION_STRIDE) + 2U) % NUMERIC_ACTION_COUNT;
    const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
    if (step % NUMERIC_ACTION_COUNT != 0U) {
      transitions[cell] =
          (transitions[cell] + 1U + (step % NUMERIC_MUTATION_STRIDE)) % NUMERIC_STATE_COUNT;
    }
    if (step % NUMERIC_ACTION_COUNT != 1U) {
      observations[cell] ^= 1U;
    }
    automaton = build_numeric_automaton(transitions, observations, (uint64_t)step + UINT64_C(2));
    CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
    size_t seeds[NUMERIC_STATE_COUNT] = {0};
    size_t seed_count = 0U;
    for (size_t pair = 0U; pair < memory.pair_count; ++pair) {
      if (memory.first_states[pair] == state || memory.second_states[pair] == state) {
        seeds[seed_count] = pair;
        ++seed_count;
        const size_t edge = (pair * memory.action_count) + action;
        CHECK(sg_pair_oracle_pair_step(oracle, pair, action, &memory.arcs[edge].target_pair,
                                       &memory.arcs[edge].outputs_differ) == SG_OK);
      }
    }
    CHECK(seed_count == NUMERIC_STATE_COUNT);
    sg_pair_repair_metrics metrics = {0};
    CHECK(sg_pair_store_repair(&store, seeds, seed_count, memory.pair_count, &metrics) == SG_OK);
    CHECK(metrics.pair_records_examined >= seed_count);
    CHECK(metrics.pair_records_touched <= memory.pair_count);
    for (size_t pair = 0U; pair < memory.pair_count; ++pair) {
      sg_pair_record expected = {0};
      CHECK(sg_pair_oracle_record(oracle, pair, &expected) == SG_OK);
      check_pair_records_equal(&memory.records[pair], &expected);
    }
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
  }

  automaton = build_numeric_automaton(transitions, observations, UINT64_C(50));
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  const sg_pair_record_source source = {
      .context = &memory,
      .read = memory_read_records,
  };
  const size_t hypotheses[] = {0U, 1U, 2U, 3U};
  sg_plan_result expected_plan = {0};
  sg_plan_result actual_plan = {0};
  CHECK(sg_plan_sync(automaton, oracle, hypotheses, 4U, 64U, &expected_plan) == SG_OK);
  CHECK(sg_plan_sync_from_records(automaton, &source, hypotheses, 4U, 64U, &actual_plan) == SG_OK);
  check_plan_semantics_equal(&expected_plan, &actual_plan);
  sg_plan_result_free(&expected_plan);
  sg_plan_result_free(&actual_plan);
  CHECK(sg_plan_disambiguate(automaton, oracle, hypotheses, 4U, 1U, 64U, &expected_plan) == SG_OK);
  CHECK(sg_plan_disambiguate_from_records(automaton, &source, hypotheses, 4U, 1U, 64U,
                                          &actual_plan) == SG_OK);
  check_plan_semantics_equal(&expected_plan, &actual_plan);
  sg_plan_result_free(&expected_plan);
  sg_plan_result_free(&actual_plan);
  CHECK(memory.record_reads != 0U);

  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);
  memory_store_free(&memory);
}

static void test_snapshot_maintenance_and_cache(void) {
  size_t transitions[NUMERIC_CELL_COUNT] = {0};
  size_t observations[NUMERIC_CELL_COUNT] = {0};
  for (size_t state = 0U; state < NUMERIC_STATE_COUNT; ++state) {
    for (size_t action = 0U; action < NUMERIC_ACTION_COUNT; ++action) {
      const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
      transitions[cell] = (state + action + 1U) % NUMERIC_STATE_COUNT;
      observations[cell] = (state + action) % NUMERIC_OUTPUT_COUNT;
    }
  }
  sg_automaton *automaton = build_numeric_automaton(transitions, observations, UINT64_C(1));
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
  sg_pair_snapshot *snapshot = NULL;
  CHECK(sg_pair_snapshot_from_oracle(automaton, oracle, &snapshot) == SG_OK);
  const size_t pair_count = sg_pair_snapshot_pair_count(snapshot);
  CHECK(pair_count == sg_pair_oracle_pair_count(oracle));
  CHECK(sg_pair_snapshot_memory_bytes(snapshot) != 0U);
  sg_pair_oracle_free(oracle);
  sg_automaton_free(automaton);

  for (size_t step = 0U; step < NUMERIC_MUTATION_COUNT; ++step) {
    const size_t state = ((step * NUMERIC_MUTATION_STRIDE) + 1U) % NUMERIC_STATE_COUNT;
    const size_t action = ((step * NUMERIC_ACTION_STRIDE) + 2U) % NUMERIC_ACTION_COUNT;
    const size_t cell = (state * NUMERIC_ACTION_COUNT) + action;
    if (step % NUMERIC_ACTION_COUNT != 0U) {
      transitions[cell] =
          (transitions[cell] + 1U + (step % NUMERIC_MUTATION_STRIDE)) % NUMERIC_STATE_COUNT;
    }
    if (step % NUMERIC_ACTION_COUNT != 1U) {
      observations[cell] ^= 1U;
    }
    sg_pair_snapshot *candidate = NULL;
    CHECK(sg_pair_snapshot_clone(snapshot, (uint64_t)step + UINT64_C(2), &candidate) == SG_OK);
    bool changed = false;
    CHECK(sg_pair_snapshot_set_cell(candidate, state, action, transitions[cell], observations[cell],
                                    &changed) == SG_OK);
    CHECK(changed);
    size_t seeds[NUMERIC_STATE_COUNT] = {0};
    size_t seed_count = 0U;
    size_t pair = 0U;
    for (size_t first = 0U; first < NUMERIC_STATE_COUNT; ++first) {
      for (size_t second = first; second < NUMERIC_STATE_COUNT; ++second) {
        if (first == state || second == state) {
          seeds[seed_count] = pair;
          ++seed_count;
        }
        ++pair;
      }
    }
    CHECK(seed_count == NUMERIC_STATE_COUNT);
    sg_pair_repair_metrics metrics = {0};
    CHECK(sg_pair_snapshot_repair(candidate, seeds, seed_count, pair_count, &metrics) == SG_OK);
    CHECK(metrics.pair_records_examined >= seed_count);
    CHECK(metrics.pair_edges_examined != 0U);
    CHECK(sg_pair_snapshot_changed_count(candidate) <= pair_count);

    automaton = build_numeric_automaton(transitions, observations, (uint64_t)step + UINT64_C(2));
    CHECK(sg_pair_oracle_build(automaton, &oracle) == SG_OK);
    for (pair = 0U; pair < pair_count; ++pair) {
      sg_pair_record expected = {0};
      sg_pair_record actual = {0};
      CHECK(sg_pair_oracle_record(oracle, pair, &expected) == SG_OK);
      CHECK(sg_pair_snapshot_record(candidate, pair, &actual) == SG_OK);
      check_pair_records_equal(&actual, &expected);
    }
    sg_pair_oracle_free(oracle);
    sg_automaton_free(automaton);
    sg_pair_snapshot_clear_changes(candidate);
    sg_pair_snapshot_release(snapshot);
    snapshot = candidate;
  }

  const sg_automaton *snapshot_automaton = sg_pair_snapshot_automaton(snapshot);
  const size_t hypotheses[] = {0U, 1U, 2U, 3U};
  const sg_pair_record_source source = {
      .context = snapshot,
      .read = snapshot_read_records,
  };
  sg_plan_result from_snapshot = {0};
  sg_plan_result rebuilt = {0};
  CHECK(sg_pair_oracle_build(snapshot_automaton, &oracle) == SG_OK);
  CHECK(sg_plan_sync_from_records(snapshot_automaton, &source, hypotheses, 4U, 64U,
                                  &from_snapshot) == SG_OK);
  CHECK(sg_plan_sync(snapshot_automaton, oracle, hypotheses, 4U, 64U, &rebuilt) == SG_OK);
  check_plan_semantics_equal(&from_snapshot, &rebuilt);
  sg_plan_result_free(&from_snapshot);
  sg_plan_result_free(&rebuilt);
  sg_pair_oracle_free(oracle);

  const size_t bytes = sg_pair_snapshot_memory_bytes(snapshot);
  sg_snapshot_cache *cache = NULL;
  CHECK(sg_snapshot_cache_create(bytes, &cache) == SG_OK);
  bool stored = false;
  CHECK(sg_snapshot_cache_insert(cache, "numeric", UINT64_C(3), UINT64_C(49), "token-a", snapshot,
                                 &stored) == SG_OK);
  CHECK(stored);
  CHECK(sg_snapshot_cache_bytes(cache) == bytes);
  sg_pair_snapshot *found =
      sg_snapshot_cache_lookup(cache, "numeric", UINT64_C(3), UINT64_C(49), "token-a");
  CHECK(found == snapshot);
  sg_pair_snapshot_release(found);
  CHECK(sg_snapshot_cache_lookup(cache, "numeric", UINT64_C(3), UINT64_C(49), "wrong") == NULL);

  sg_pair_snapshot *second = NULL;
  CHECK(sg_pair_snapshot_clone(snapshot, UINT64_C(50), &second) == SG_OK);
  CHECK(sg_snapshot_cache_insert(cache, "numeric", UINT64_C(3), UINT64_C(50), "token-b", second,
                                 &stored) == SG_OK);
  CHECK(stored);
  CHECK(sg_snapshot_cache_lookup(cache, "numeric", UINT64_C(3), UINT64_C(49), "token-a") == NULL);
  found = sg_snapshot_cache_lookup(cache, "numeric", UINT64_C(3), UINT64_C(50), "token-b");
  CHECK(found == second);
  sg_pair_snapshot_release(found);
  sg_pair_snapshot_release(second);
  sg_snapshot_cache_free(cache);
  sg_pair_snapshot_release(snapshot);
}

int main(void) {
  test_names_and_builder_validation();
  test_automaton_and_oracle();
  test_planners_explanation_and_monitor();
  test_observed_monitor();
  test_observed_monitor_prefixes();
  test_observed_monitor_arguments();
  test_exact_partition_search();
  test_sync_fallback_case(true);
  test_sync_fallback_case(false);
  test_sync_neutral_moves_and_queue_growth();
  test_sync_fallback_inputs_and_errors();
  test_sync_exhaustive_small_models();
  test_oracle_source_equivalence();
  test_incremental_pair_maintenance();
  test_snapshot_maintenance_and_cache();
  return EXIT_SUCCESS;
}
