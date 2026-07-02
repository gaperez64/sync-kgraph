#include "sync_kgraph/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int add_transition(sg_dfa_builder *builder, const char *source, const char *letter,
                          const char *target) {
  if (sg_dfa_builder_add_transition(builder, source, letter, target) != SG_OK) {
    return 1;
  }
  return 0;
}

static sg_status build_office(sg_dfa **dfa) {
  sg_dfa_builder *builder = NULL;
  sg_status status = sg_dfa_builder_init(&builder);
  if (status != SG_OK) {
    return status;
  }

  const char *states[] = {"A", "B", "C"};
  const char *letters[] = {"north", "east"};
  for (size_t i = 0U; i < sizeof(states) / sizeof(states[0]); ++i) {
    status = sg_dfa_builder_add_state(builder, states[i]);
    if (status != SG_OK) {
      sg_dfa_builder_free(builder);
      return status;
    }
  }
  for (size_t i = 0U; i < sizeof(letters) / sizeof(letters[0]); ++i) {
    status = sg_dfa_builder_add_letter(builder, letters[i]);
    if (status != SG_OK) {
      sg_dfa_builder_free(builder);
      return status;
    }
  }

  if (add_transition(builder, "A", "north", "B") != 0 ||
      add_transition(builder, "B", "north", "C") != 0 ||
      add_transition(builder, "C", "north", "C") != 0 ||
      add_transition(builder, "A", "east", "A") != 0 ||
      add_transition(builder, "B", "east", "B") != 0 ||
      add_transition(builder, "C", "east", "C") != 0) {
    sg_dfa_builder_free(builder);
    return SG_ERR_ALLOC;
  }

  status = sg_dfa_builder_build(builder, false, dfa);
  sg_dfa_builder_free(builder);
  return status;
}

static void print_word(const sg_dfa *dfa, const sg_word *word) {
  printf("[");
  for (size_t i = 0U; i < word->length; ++i) {
    printf("%s\"%s\"", (i == 0U) ? "" : ", ", sg_dfa_letter_key(dfa, word->letters[i]));
  }
  printf("]");
}

static int run_office_example(void) {
  sg_dfa *dfa = NULL;
  sg_status status = build_office(&dfa);
  if (status != SG_OK) {
    fprintf(stderr, "build_office failed: %s\n", sg_status_name(status));
    return 1;
  }

  sg_pair_oracle *oracle = NULL;
  status = sg_pair_oracle_build(dfa, &oracle);
  if (status != SG_OK) {
    fprintf(stderr, "pair oracle failed: %s\n", sg_status_name(status));
    sg_dfa_free(dfa);
    return 1;
  }

  size_t initial[2] = {0U, 0U};
  size_t target[1] = {0U};
  (void)sg_dfa_find_state(dfa, "A", &initial[0]);
  (void)sg_dfa_find_state(dfa, "B", &initial[1]);
  (void)sg_dfa_find_state(dfa, "C", &target[0]);

  sg_word_result result = {0};
  status =
      sg_word_for_set(dfa, oracle, initial, 2U, target, 1U, SG_MODE_REACH_AND_SYNC, 64U, &result);
  if (status != SG_OK) {
    fprintf(stderr, "word_for_set failed: %s\n", sg_status_name(status));
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    return 1;
  }

  printf("status: %s\n", sg_result_kind_name(result.kind));
  printf("word: ");
  print_word(dfa, &result.word);
  printf("\nlength: %zu\n", result.word.length);
  printf("final_state: %s\n", sg_dfa_state_key(dfa, result.final_state));

  size_t *steps = NULL;
  size_t *counts = NULL;
  size_t step_count = 0U;
  status = sg_explain_word(dfa, initial, 2U, &result.word, &steps, &counts, &step_count);
  if (status != SG_OK) {
    fprintf(stderr, "explain failed: %s\n", sg_status_name(status));
    sg_word_result_free(&result);
    sg_pair_oracle_free(oracle);
    sg_dfa_free(dfa);
    return 1;
  }

  for (size_t step = 0U; step < step_count; ++step) {
    printf("step %zu active:", step);
    for (size_t i = 0U; i < counts[step]; ++i) {
      const size_t state = steps[(step * sg_dfa_state_count(dfa)) + i];
      printf(" %s", sg_dfa_state_key(dfa, state));
    }
    printf("\n");
  }

  sg_explain_free(steps, counts);
  sg_word_result_free(&result);
  sg_pair_oracle_free(oracle);
  sg_dfa_free(dfa);
  return 0;
}

static void print_usage(const char *argv0) {
  fprintf(stderr, "usage: %s --example office\n", argv0);
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--example") == 0 && strcmp(argv[2], "office") == 0) {
    return run_office_example();
  }
  print_usage(argv[0]);
  return 2;
}
