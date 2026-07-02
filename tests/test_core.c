#include "sync_kgraph/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)

static sg_dfa *build_office(void) {
  sg_dfa_builder *builder = NULL;
  CHECK(sg_dfa_builder_init(&builder) == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "A") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "B") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "C") == SG_OK);
  CHECK(sg_dfa_builder_add_letter(builder, "north") == SG_OK);
  CHECK(sg_dfa_builder_add_letter(builder, "east") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "A", "north", "B") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "B", "north", "C") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "C", "north", "C") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "A", "east", "A") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "B", "east", "B") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "C", "east", "C") == SG_OK);
  sg_dfa *dfa = NULL;
  CHECK(sg_dfa_builder_build(builder, false, &dfa) == SG_OK);
  sg_dfa_builder_free(builder);
  return dfa;
}

typedef struct {
  size_t calls;
  size_t nonempty;
} cache_visit_counts;

static sg_status count_cache_visit(void *ctx, const size_t *states, size_t state_count,
                                   const sg_word *word) {
  cache_visit_counts *counts = ctx;
  CHECK(counts != NULL);
  CHECK(word != NULL);
  CHECK(states != NULL || state_count == 0U);
  ++counts->calls;
  if (state_count != 0U) {
    ++counts->nonempty;
  }
  return SG_OK;
}

static void test_builder_validation(void) {
  sg_dfa_builder *builder = NULL;
  CHECK(sg_dfa_builder_init(&builder) == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "S") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "T") == SG_OK);
  CHECK(sg_dfa_builder_add_letter(builder, "a") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "S", "a", "T") == SG_OK);

  sg_dfa *dfa = NULL;
  CHECK(sg_dfa_builder_build(builder, false, &dfa) == SG_ERR_INCOMPLETE);
  CHECK(dfa == NULL);
  CHECK(sg_dfa_builder_build(builder, true, &dfa) == SG_OK);
  CHECK(sg_dfa_state_count(dfa) == 3U);
  CHECK(strcmp(sg_dfa_state_key(dfa, 2U), "__sink") == 0);
  sg_dfa_free(dfa);
  sg_dfa_builder_free(builder);

  CHECK(sg_dfa_builder_init(&builder) == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "S") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "T") == SG_OK);
  CHECK(sg_dfa_builder_add_letter(builder, "a") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "S", "a", "S") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "S", "a", "T") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "T", "a", "T") == SG_OK);
  CHECK(sg_dfa_builder_build(builder, false, &dfa) == SG_ERR_NONDETERMINISTIC);
  sg_dfa_builder_free(builder);
}

static void test_pair_oracle_and_greedy_word(void) {
  sg_dfa *dfa = build_office();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(dfa, &oracle) == SG_OK);
  CHECK(sg_pair_oracle_pair_count(oracle) == 6U);
  CHECK(sg_pair_oracle_pair_edge_count(oracle) == 12U);

  size_t a = 0U;
  size_t b = 0U;
  size_t c = 0U;
  size_t north = 0U;
  CHECK(sg_dfa_find_state(dfa, "A", &a) == SG_OK);
  CHECK(sg_dfa_find_state(dfa, "B", &b) == SG_OK);
  CHECK(sg_dfa_find_state(dfa, "C", &c) == SG_OK);
  CHECK(sg_dfa_find_letter(dfa, "north", &north) == SG_OK);
  CHECK(sg_pair_oracle_has_witness(oracle, a, b));

  size_t first = 0U;
  size_t second = 0U;
  CHECK(sg_pair_oracle_pair_states(oracle, 1U, &first, &second) == SG_OK);
  CHECK(first == a);
  CHECK(second == b);
  size_t next_pair = 0U;
  CHECK(sg_pair_oracle_pair_next(oracle, 1U, north, &next_pair) == SG_OK);
  CHECK(next_pair == 4U);
  bool has_witness = false;
  size_t distance = 0U;
  size_t letter = 0U;
  CHECK(sg_pair_oracle_pair_witness(oracle, 1U, &has_witness, &distance, &letter, &next_pair) ==
        SG_OK);
  CHECK(has_witness);
  CHECK(distance == 2U);
  CHECK(letter == north);
  CHECK(next_pair == 4U);

  sg_word witness = {0};
  CHECK(sg_pair_oracle_witness_word(oracle, a, b, &witness) == SG_OK);
  CHECK(witness.length == 2U);
  CHECK(strcmp(sg_dfa_letter_key(dfa, witness.letters[0]), "north") == 0);
  CHECK(strcmp(sg_dfa_letter_key(dfa, witness.letters[1]), "north") == 0);
  sg_word_free(&witness);

  size_t initial[2] = {a, b};
  size_t target[1] = {c};
  sg_word_result result = {0};
  CHECK(sg_word_for_set(dfa, oracle, initial, 2U, target, 1U, SG_MODE_REACH_AND_SYNC, 16U,
                        &result) == SG_OK);
  CHECK(result.kind == SG_RESULT_PAIR_GREEDY_TARGETED);
  CHECK(result.word.length == 2U);
  CHECK(result.final_state == c);

  size_t out[3] = {0U, 0U, 0U};
  size_t out_count = 0U;
  CHECK(sg_apply_word_to_set(dfa, initial, 2U, &result.word, out, &out_count) == SG_OK);
  CHECK(out_count == 1U);
  CHECK(out[0] == c);

  size_t *steps = NULL;
  size_t *counts = NULL;
  size_t step_count = 0U;
  CHECK(sg_explain_word(dfa, initial, 2U, &result.word, &steps, &counts, &step_count) == SG_OK);
  CHECK(step_count == 3U);
  CHECK(counts[0] == 2U);
  CHECK(counts[1] == 2U);
  CHECK(counts[2] == 1U);
  CHECK(steps[(2U * sg_dfa_state_count(dfa))] == c);
  sg_explain_free(steps, counts);

  sg_word_result_free(&result);
  sg_pair_oracle_free(oracle);
  sg_dfa_free(dfa);
}

static sg_dfa *build_reach_only_machine(void) {
  sg_dfa_builder *builder = NULL;
  CHECK(sg_dfa_builder_init(&builder) == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "A") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "B") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "C") == SG_OK);
  CHECK(sg_dfa_builder_add_state(builder, "D") == SG_OK);
  CHECK(sg_dfa_builder_add_letter(builder, "go") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "A", "go", "C") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "B", "go", "D") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "C", "go", "C") == SG_OK);
  CHECK(sg_dfa_builder_add_transition(builder, "D", "go", "D") == SG_OK);
  sg_dfa *dfa = NULL;
  CHECK(sg_dfa_builder_build(builder, false, &dfa) == SG_OK);
  sg_dfa_builder_free(builder);
  return dfa;
}

static void test_exact_reach_mode(void) {
  sg_dfa *dfa = build_reach_only_machine();
  sg_pair_oracle *oracle = NULL;
  CHECK(sg_pair_oracle_build(dfa, &oracle) == SG_OK);

  size_t a = 0U;
  size_t b = 0U;
  size_t c = 0U;
  size_t d = 0U;
  CHECK(sg_dfa_find_state(dfa, "A", &a) == SG_OK);
  CHECK(sg_dfa_find_state(dfa, "B", &b) == SG_OK);
  CHECK(sg_dfa_find_state(dfa, "C", &c) == SG_OK);
  CHECK(sg_dfa_find_state(dfa, "D", &d) == SG_OK);

  size_t initial[2] = {a, b};
  size_t targets[2] = {c, d};
  sg_word_result result = {0};
  CHECK(sg_word_for_set(dfa, oracle, initial, 2U, targets, 2U, SG_MODE_REACH, 8U, &result) ==
        SG_OK);
  CHECK(result.kind == SG_RESULT_EXACT_EXPANDED);
  CHECK(result.word.length == 1U);
  CHECK(strcmp(sg_dfa_letter_key(dfa, result.word.letters[0]), "go") == 0);
  sg_word_result_free(&result);

  size_t expanded = 0U;
  size_t cache_size = 0U;
  CHECK(sg_expand_cache(dfa, targets, 2U, SG_MODE_REACH, 8U, &expanded, &cache_size) == SG_OK);
  CHECK(expanded >= 1U);
  CHECK(cache_size >= expanded);

  cache_visit_counts visits = {0U, 0U};
  expanded = 0U;
  cache_size = 0U;
  CHECK(sg_expand_cache_visit(dfa, targets, 2U, SG_MODE_REACH, 8U, count_cache_visit, &visits,
                              &expanded, &cache_size) == SG_OK);
  CHECK(visits.calls == expanded);
  CHECK(visits.nonempty == visits.calls);
  CHECK(cache_size >= expanded);

  expanded = 0U;
  cache_size = 0U;
  CHECK(sg_expand_cache(dfa, targets, 2U, SG_MODE_REACH_AND_SYNC, 8U, &expanded, &cache_size) ==
        SG_OK);
  CHECK(expanded >= 1U);
  CHECK(cache_size >= expanded);

  expanded = 0U;
  cache_size = 0U;
  CHECK(sg_expand_cache(dfa, NULL, 0U, SG_MODE_SYNC, 8U, &expanded, &cache_size) == SG_OK);
  CHECK(expanded >= 1U);
  CHECK(cache_size >= expanded);

  expanded = 8U;
  cache_size = 8U;
  CHECK(sg_expand_cache(dfa, targets, 2U, SG_MODE_REACH, 0U, &expanded, &cache_size) ==
        SG_ERR_RESOURCE_BOUND);
  CHECK(expanded == 0U);
  CHECK(cache_size == 0U);

  CHECK(sg_word_for_set(dfa, oracle, initial, 2U, targets, 2U, SG_MODE_REACH_AND_SYNC, 1U,
                        &result) == SG_ERR_RESOURCE_BOUND);
  CHECK(result.kind == SG_RESULT_RESOURCE_BOUND);
  sg_word_result_free(&result);

  sg_pair_oracle_free(oracle);
  sg_dfa_free(dfa);
}

static void test_mode_parse(void) {
  sg_mode mode = SG_MODE_SYNC;
  CHECK(strcmp(sg_status_name(SG_ERR_ALLOC), "ALLOC") == 0);
  CHECK(strcmp(sg_status_name(SG_ERR_DUPLICATE), "DUPLICATE") == 0);
  CHECK(strcmp(sg_status_name(SG_ERR_INCOMPLETE), "INCOMPLETE") == 0);
  CHECK(strcmp(sg_status_name(SG_ERR_NONDETERMINISTIC), "NONDETERMINISTIC") == 0);
  CHECK(strcmp(sg_status_name(SG_ERR_UNSYNCHRONIZABLE), "UNSYNCHRONIZABLE") == 0);
  CHECK(strcmp(sg_result_kind_name(SG_RESULT_TRIVIAL), "TRIVIAL") == 0);
  CHECK(strcmp(sg_result_kind_name(SG_RESULT_PAIR_GREEDY), "PAIR_GREEDY") == 0);
  CHECK(strcmp(sg_result_kind_name(SG_RESULT_FAILURE), "FAILURE") == 0);
  CHECK(strcmp(sg_mode_name(SG_MODE_REACH_AND_SYNC), "REACH_AND_SYNC") == 0);
  CHECK(sg_mode_parse("SYNC", &mode) == SG_OK);
  CHECK(mode == SG_MODE_SYNC);
  CHECK(sg_mode_parse("REACH", &mode) == SG_OK);
  CHECK(mode == SG_MODE_REACH);
  CHECK(sg_mode_parse("REACH_AND_SYNC", &mode) == SG_OK);
  CHECK(mode == SG_MODE_REACH_AND_SYNC);
  CHECK(sg_mode_parse("bad", &mode) == SG_ERR_INVALID_ARGUMENT);
}

int main(void) {
  test_mode_parse();
  test_builder_validation();
  test_pair_oracle_and_greedy_word();
  test_exact_reach_mode();
  return 0;
}
