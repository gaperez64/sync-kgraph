#ifndef SYNC_KGRAPH_SYNC_H
#define SYNC_KGRAPH_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SG_OK = 0,
  SG_ERR_ALLOC,
  SG_ERR_INVALID_ARGUMENT,
  SG_ERR_DUPLICATE,
  SG_ERR_NOT_FOUND,
  SG_ERR_INCOMPLETE,
  SG_ERR_NONDETERMINISTIC,
  SG_ERR_UNSYNCHRONIZABLE,
  SG_ERR_RESOURCE_BOUND,
} sg_status;

typedef enum {
  SG_MODE_SYNC = 0,
  SG_MODE_REACH = 1,
  SG_MODE_REACH_AND_SYNC = 2,
} sg_mode;

typedef enum {
  SG_RESULT_TRIVIAL = 0,
  SG_RESULT_PAIR_GREEDY,
  SG_RESULT_PAIR_GREEDY_TARGETED,
  SG_RESULT_EXACT_EXPANDED,
  SG_RESULT_RESOURCE_BOUND,
  SG_RESULT_FAILURE,
} sg_result_kind;

typedef struct sg_dfa sg_dfa;
typedef struct sg_dfa_builder sg_dfa_builder;
typedef struct sg_pair_oracle sg_pair_oracle;

typedef struct {
  size_t *letters;
  size_t length;
  size_t capacity;
} sg_word;

typedef struct {
  sg_result_kind kind;
  sg_status status;
  sg_word word;
  size_t final_state;
  size_t final_count;
} sg_word_result;

typedef sg_status (*sg_cache_visitor)(void *ctx, const size_t *states, size_t state_count,
                                      const sg_word *word);

const char *sg_status_name(sg_status status);
const char *sg_result_kind_name(sg_result_kind kind);
const char *sg_mode_name(sg_mode mode);
sg_status sg_mode_parse(const char *name, sg_mode *mode);

sg_status sg_word_init(sg_word *word);
void sg_word_free(sg_word *word);
sg_status sg_word_append(sg_word *word, size_t letter);
sg_status sg_word_prepend(sg_word *word, size_t letter);

sg_status sg_dfa_builder_init(sg_dfa_builder **builder);
void sg_dfa_builder_free(sg_dfa_builder *builder);
sg_status sg_dfa_builder_add_state(sg_dfa_builder *builder, const char *state_key);
sg_status sg_dfa_builder_add_letter(sg_dfa_builder *builder, const char *letter);
sg_status sg_dfa_builder_add_transition(sg_dfa_builder *builder, const char *source_key,
                                        const char *letter, const char *target_key);
sg_status sg_dfa_builder_build(sg_dfa_builder *builder, bool complete_with_sink, sg_dfa **dfa);

void sg_dfa_free(sg_dfa *dfa);
size_t sg_dfa_state_count(const sg_dfa *dfa);
size_t sg_dfa_letter_count(const sg_dfa *dfa);
size_t sg_dfa_transition_count(const sg_dfa *dfa);
const char *sg_dfa_state_key(const sg_dfa *dfa, size_t state);
const char *sg_dfa_letter_key(const sg_dfa *dfa, size_t letter);
size_t sg_dfa_transition(const sg_dfa *dfa, size_t state, size_t letter);
sg_status sg_dfa_find_state(const sg_dfa *dfa, const char *state_key, size_t *state);
sg_status sg_dfa_find_letter(const sg_dfa *dfa, const char *letter, size_t *letter_id);

sg_status sg_pair_oracle_build(const sg_dfa *dfa, sg_pair_oracle **oracle);
void sg_pair_oracle_free(sg_pair_oracle *oracle);
size_t sg_pair_oracle_pair_count(const sg_pair_oracle *oracle);
size_t sg_pair_oracle_pair_edge_count(const sg_pair_oracle *oracle);
size_t sg_pair_oracle_mergeable_pair_count(const sg_pair_oracle *oracle);
sg_status sg_pair_oracle_pair_states(const sg_pair_oracle *oracle, size_t pair, size_t *first,
                                     size_t *second);
sg_status sg_pair_oracle_pair_next(const sg_pair_oracle *oracle, size_t pair, size_t letter,
                                   size_t *next_pair);
sg_status sg_pair_oracle_pair_witness(const sg_pair_oracle *oracle, size_t pair, bool *has_witness,
                                      size_t *distance, size_t *letter, size_t *next_pair);
bool sg_pair_oracle_has_witness(const sg_pair_oracle *oracle, size_t first, size_t second);
sg_status sg_pair_oracle_witness_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                      sg_word *word);

sg_status sg_word_for_set(const sg_dfa *dfa, const sg_pair_oracle *oracle, const size_t *initial,
                          size_t initial_count, const size_t *targets, size_t target_count,
                          sg_mode mode, size_t exact_budget, sg_word_result *result);
void sg_word_result_free(sg_word_result *result);
sg_status sg_expand_cache(const sg_dfa *dfa, const size_t *targets, size_t target_count,
                          sg_mode mode, size_t budget, size_t *expanded, size_t *cache_size);
sg_status sg_expand_cache_visit(const sg_dfa *dfa, const size_t *targets, size_t target_count,
                                sg_mode mode, size_t budget, sg_cache_visitor visitor, void *ctx,
                                size_t *expanded, size_t *cache_size);

sg_status sg_apply_word_to_set(const sg_dfa *dfa, const size_t *initial, size_t initial_count,
                               const sg_word *word, size_t *output, size_t *output_count);
sg_status sg_explain_word(const sg_dfa *dfa, const size_t *initial, size_t initial_count,
                          const sg_word *word, size_t **steps, size_t **step_counts,
                          size_t *step_count);
void sg_explain_free(size_t *steps, size_t *step_counts);

#ifdef __cplusplus
}
#endif

#endif
