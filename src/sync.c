#include "sync_kgraph/sync.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SG_BITS_PER_WORD = 64 };

typedef struct {
  char **items;
  size_t length;
  size_t capacity;
} sg_string_vec;

typedef struct {
  char *source;
  char *letter;
  char *target;
} sg_transition_record;

typedef struct {
  sg_transition_record *items;
  size_t length;
  size_t capacity;
} sg_transition_vec;

struct sg_dfa_builder {
  sg_string_vec states;
  sg_string_vec letters;
  sg_transition_vec transitions;
};

struct sg_dfa {
  char **states;
  char **letters;
  size_t state_count;
  size_t letter_count;
  size_t *transitions;
};

struct sg_pair_oracle {
  const sg_dfa *dfa;
  size_t pair_count;
  size_t edge_count;
  size_t mergeable_pairs;
  size_t *forward;
  size_t *reverse_head;
  size_t *reverse_next;
  size_t *reverse_from;
  size_t *reverse_letter;
  size_t *dist;
  size_t *next_pair;
  size_t *witness;
};

typedef struct {
  uint64_t *words;
  size_t word_count;
} sg_bitset;

typedef struct {
  sg_bitset set;
  sg_word word;
} sg_subset_node;

typedef struct {
  sg_subset_node *items;
  size_t length;
  size_t capacity;
} sg_subset_vec;

static const size_t SG_INVALID = (size_t)-1;

const char *sg_status_name(sg_status status) {
  switch (status) {
  case SG_OK:
    return "OK";
  case SG_ERR_ALLOC:
    return "ALLOC";
  case SG_ERR_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case SG_ERR_DUPLICATE:
    return "DUPLICATE";
  case SG_ERR_NOT_FOUND:
    return "NOT_FOUND";
  case SG_ERR_INCOMPLETE:
    return "INCOMPLETE";
  case SG_ERR_NONDETERMINISTIC:
    return "NONDETERMINISTIC";
  case SG_ERR_UNSYNCHRONIZABLE:
    return "UNSYNCHRONIZABLE";
  case SG_ERR_RESOURCE_BOUND:
    return "RESOURCE_BOUND";
  }
  return "UNKNOWN";
}

const char *sg_result_kind_name(sg_result_kind kind) {
  switch (kind) {
  case SG_RESULT_TRIVIAL:
    return "TRIVIAL";
  case SG_RESULT_PAIR_GREEDY:
    return "PAIR_GREEDY";
  case SG_RESULT_PAIR_GREEDY_TARGETED:
    return "PAIR_GREEDY_TARGETED";
  case SG_RESULT_EXACT_EXPANDED:
    return "EXACT_EXPANDED";
  case SG_RESULT_RESOURCE_BOUND:
    return "RESOURCE_BOUND";
  case SG_RESULT_FAILURE:
    return "FAILURE";
  }
  return "UNKNOWN";
}

const char *sg_mode_name(sg_mode mode) {
  switch (mode) {
  case SG_MODE_SYNC:
    return "SYNC";
  case SG_MODE_REACH:
    return "REACH";
  case SG_MODE_REACH_AND_SYNC:
    return "REACH_AND_SYNC";
  }
  return "UNKNOWN";
}

sg_status sg_mode_parse(const char *name, sg_mode *mode) {
  if (name == NULL || mode == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (strcmp(name, "SYNC") == 0) {
    *mode = SG_MODE_SYNC;
    return SG_OK;
  }
  if (strcmp(name, "REACH") == 0) {
    *mode = SG_MODE_REACH;
    return SG_OK;
  }
  if (strcmp(name, "REACH_AND_SYNC") == 0 || strcmp(name, "REACHSYNC") == 0 ||
      strcmp(name, "REACH_SYNC") == 0) {
    *mode = SG_MODE_REACH_AND_SYNC;
    return SG_OK;
  }
  return SG_ERR_INVALID_ARGUMENT;
}

static char *sg_strdup_owned(const char *value) {
  if (value == NULL) {
    return NULL;
  }
  const size_t length = strlen(value);
  char *copy = malloc(length + 1U);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, length + 1U);
  return copy;
}

static sg_status sg_checked_mul(size_t lhs, size_t rhs, size_t *result) {
  if (result == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (lhs != 0U && rhs > SIZE_MAX / lhs) {
    return SG_ERR_ALLOC;
  }
  *result = lhs * rhs;
  return SG_OK;
}

static sg_status sg_grow(void **items, size_t item_size, size_t *capacity, size_t needed) {
  if (items == NULL || capacity == NULL || item_size == 0U) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (*capacity >= needed) {
    return SG_OK;
  }

  size_t next = (*capacity == 0U) ? 8U : *capacity;
  while (next < needed) {
    if (next > SIZE_MAX / 2U) {
      return SG_ERR_ALLOC;
    }
    next *= 2U;
  }

  size_t bytes = 0U;
  sg_status status = sg_checked_mul(next, item_size, &bytes);
  if (status != SG_OK) {
    return status;
  }

  void *grown = realloc(*items, bytes);
  if (grown == NULL) {
    return SG_ERR_ALLOC;
  }
  *items = grown;
  *capacity = next;
  return SG_OK;
}

static void sg_string_vec_free(sg_string_vec *vec) {
  if (vec == NULL) {
    return;
  }
  for (size_t i = 0U; i < vec->length; ++i) {
    free(vec->items[i]);
  }
  free((void *)vec->items);
  vec->items = NULL;
  vec->length = 0U;
  vec->capacity = 0U;
}

static sg_status sg_string_vec_find(const sg_string_vec *vec, const char *value, size_t *index) {
  if (vec == NULL || value == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0U; i < vec->length; ++i) {
    if (strcmp(vec->items[i], value) == 0) {
      if (index != NULL) {
        *index = i;
      }
      return SG_OK;
    }
  }
  return SG_ERR_NOT_FOUND;
}

static sg_status sg_string_vec_add_unique(sg_string_vec *vec, const char *value, size_t *index) {
  if (vec == NULL || value == NULL || value[0] == '\0') {
    return SG_ERR_INVALID_ARGUMENT;
  }
  size_t existing = 0U;
  if (sg_string_vec_find(vec, value, &existing) == SG_OK) {
    if (index != NULL) {
      *index = existing;
    }
    return SG_OK;
  }

  sg_status status =
      sg_grow((void **)&vec->items, sizeof(vec->items[0]), &vec->capacity, vec->length + 1U);
  if (status != SG_OK) {
    return status;
  }

  char *copy = sg_strdup_owned(value);
  if (copy == NULL) {
    return SG_ERR_ALLOC;
  }
  vec->items[vec->length] = copy;
  if (index != NULL) {
    *index = vec->length;
  }
  ++vec->length;
  return SG_OK;
}

static void sg_transition_record_free(sg_transition_record *record) {
  if (record == NULL) {
    return;
  }
  free(record->source);
  free(record->letter);
  free(record->target);
  record->source = NULL;
  record->letter = NULL;
  record->target = NULL;
}

static void sg_transition_vec_free(sg_transition_vec *vec) {
  if (vec == NULL) {
    return;
  }
  for (size_t i = 0U; i < vec->length; ++i) {
    sg_transition_record_free(&vec->items[i]);
  }
  free(vec->items);
  vec->items = NULL;
  vec->length = 0U;
  vec->capacity = 0U;
}

sg_status sg_word_init(sg_word *word) {
  if (word == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  word->letters = NULL;
  word->length = 0U;
  word->capacity = 0U;
  return SG_OK;
}

void sg_word_free(sg_word *word) {
  if (word == NULL) {
    return;
  }
  free(word->letters);
  word->letters = NULL;
  word->length = 0U;
  word->capacity = 0U;
}

sg_status sg_word_append(sg_word *word, size_t letter) {
  if (word == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_grow((void **)&word->letters, sizeof(word->letters[0]), &word->capacity,
                             word->length + 1U);
  if (status != SG_OK) {
    return status;
  }
  word->letters[word->length] = letter;
  ++word->length;
  return SG_OK;
}

sg_status sg_word_prepend(sg_word *word, size_t letter) {
  if (word == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_grow((void **)&word->letters, sizeof(word->letters[0]), &word->capacity,
                             word->length + 1U);
  if (status != SG_OK) {
    return status;
  }
  memmove(&word->letters[1], &word->letters[0], word->length * sizeof(word->letters[0]));
  word->letters[0] = letter;
  ++word->length;
  return SG_OK;
}

static sg_status sg_word_extend(sg_word *dst, const sg_word *src) {
  if (dst == NULL || src == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0U; i < src->length; ++i) {
    sg_status status = sg_word_append(dst, src->letters[i]);
    if (status != SG_OK) {
      return status;
    }
  }
  return SG_OK;
}

static sg_status sg_word_copy(const sg_word *src, sg_word *dst) {
  if (src == NULL || dst == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_word_init(dst);
  if (status != SG_OK) {
    return status;
  }
  status = sg_word_extend(dst, src);
  if (status != SG_OK) {
    sg_word_free(dst);
  }
  return status;
}

sg_status sg_dfa_builder_init(sg_dfa_builder **builder) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_dfa_builder *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  *builder = created;
  return SG_OK;
}

void sg_dfa_builder_free(sg_dfa_builder *builder) {
  if (builder == NULL) {
    return;
  }
  sg_string_vec_free(&builder->states);
  sg_string_vec_free(&builder->letters);
  sg_transition_vec_free(&builder->transitions);
  free(builder);
}

sg_status sg_dfa_builder_add_state(sg_dfa_builder *builder, const char *state_key) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_string_vec_add_unique(&builder->states, state_key, NULL);
}

sg_status sg_dfa_builder_add_letter(sg_dfa_builder *builder, const char *letter) {
  if (builder == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return sg_string_vec_add_unique(&builder->letters, letter, NULL);
}

sg_status sg_dfa_builder_add_transition(sg_dfa_builder *builder, const char *source_key,
                                        const char *letter, const char *target_key) {
  if (builder == NULL || source_key == NULL || letter == NULL || target_key == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status =
      sg_grow((void **)&builder->transitions.items, sizeof(builder->transitions.items[0]),
              &builder->transitions.capacity, builder->transitions.length + 1U);
  if (status != SG_OK) {
    return status;
  }

  sg_transition_record record = {
      .source = sg_strdup_owned(source_key),
      .letter = sg_strdup_owned(letter),
      .target = sg_strdup_owned(target_key),
  };
  if (record.source == NULL || record.letter == NULL || record.target == NULL) {
    sg_transition_record_free(&record);
    return SG_ERR_ALLOC;
  }

  builder->transitions.items[builder->transitions.length] = record;
  ++builder->transitions.length;
  return SG_OK;
}

static sg_status sg_dfa_alloc_from_builder(const sg_dfa_builder *builder, bool needs_sink,
                                           sg_dfa **dfa) {
  sg_dfa *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }

  created->state_count = builder->states.length + (needs_sink ? 1U : 0U);
  created->letter_count = builder->letters.length;

  created->states = (char **)calloc(created->state_count, sizeof(created->states[0]));
  created->letters = (char **)calloc(created->letter_count, sizeof(created->letters[0]));
  if (created->states == NULL || created->letters == NULL) {
    free((void *)created->states);
    free((void *)created->letters);
    free(created);
    return SG_ERR_ALLOC;
  }

  for (size_t i = 0U; i < builder->states.length; ++i) {
    created->states[i] = sg_strdup_owned(builder->states.items[i]);
    if (created->states[i] == NULL) {
      sg_dfa_free(created);
      return SG_ERR_ALLOC;
    }
  }
  if (needs_sink) {
    created->states[created->state_count - 1U] = sg_strdup_owned("__sink");
    if (created->states[created->state_count - 1U] == NULL) {
      sg_dfa_free(created);
      return SG_ERR_ALLOC;
    }
  }

  for (size_t i = 0U; i < builder->letters.length; ++i) {
    created->letters[i] = sg_strdup_owned(builder->letters.items[i]);
    if (created->letters[i] == NULL) {
      sg_dfa_free(created);
      return SG_ERR_ALLOC;
    }
  }

  size_t transition_count = 0U;
  sg_status status = sg_checked_mul(created->state_count, created->letter_count, &transition_count);
  if (status != SG_OK) {
    sg_dfa_free(created);
    return status;
  }
  created->transitions = malloc(transition_count * sizeof(created->transitions[0]));
  if (created->transitions == NULL && transition_count != 0U) {
    sg_dfa_free(created);
    return SG_ERR_ALLOC;
  }
  for (size_t i = 0U; i < transition_count; ++i) {
    created->transitions[i] = SG_INVALID;
  }

  *dfa = created;
  return SG_OK;
}

sg_status sg_dfa_builder_build(sg_dfa_builder *builder, bool complete_with_sink, sg_dfa **dfa) {
  if (builder == NULL || dfa == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *dfa = NULL;
  if (builder->states.length == 0U || builder->letters.length == 0U) {
    return SG_ERR_INCOMPLETE;
  }

  bool needs_sink = false;
  sg_status status = sg_dfa_alloc_from_builder(builder, false, dfa);
  if (status != SG_OK) {
    return status;
  }

  for (size_t i = 0U; i < builder->transitions.length; ++i) {
    const sg_transition_record *record = &builder->transitions.items[i];
    size_t source = 0U;
    size_t letter = 0U;
    size_t target = 0U;
    if (sg_dfa_find_state(*dfa, record->source, &source) != SG_OK ||
        sg_dfa_find_letter(*dfa, record->letter, &letter) != SG_OK ||
        sg_dfa_find_state(*dfa, record->target, &target) != SG_OK) {
      sg_dfa_free(*dfa);
      *dfa = NULL;
      return SG_ERR_NOT_FOUND;
    }
    size_t index = (source * (*dfa)->letter_count) + letter;
    if ((*dfa)->transitions[index] != SG_INVALID) {
      sg_dfa_free(*dfa);
      *dfa = NULL;
      return SG_ERR_NONDETERMINISTIC;
    }
    (*dfa)->transitions[index] = target;
  }

  const size_t raw_transition_count = sg_dfa_transition_count(*dfa);
  for (size_t i = 0U; i < raw_transition_count; ++i) {
    if ((*dfa)->transitions[i] == SG_INVALID) {
      needs_sink = true;
      break;
    }
  }

  if (!needs_sink) {
    return SG_OK;
  }
  if (!complete_with_sink) {
    sg_dfa_free(*dfa);
    *dfa = NULL;
    return SG_ERR_INCOMPLETE;
  }

  sg_dfa_free(*dfa);
  *dfa = NULL;
  status = sg_dfa_alloc_from_builder(builder, true, dfa);
  if (status != SG_OK) {
    return status;
  }

  const size_t sink = (*dfa)->state_count - 1U;
  const size_t transition_count = sg_dfa_transition_count(*dfa);
  for (size_t i = 0U; i < transition_count; ++i) {
    (*dfa)->transitions[i] = sink;
  }
  for (size_t i = 0U; i < builder->transitions.length; ++i) {
    const sg_transition_record *record = &builder->transitions.items[i];
    size_t source = 0U;
    size_t letter = 0U;
    size_t target = 0U;
    (void)sg_dfa_find_state(*dfa, record->source, &source);
    (void)sg_dfa_find_letter(*dfa, record->letter, &letter);
    (void)sg_dfa_find_state(*dfa, record->target, &target);
    const size_t index = (source * (*dfa)->letter_count) + letter;
    if ((*dfa)->transitions[index] != sink) {
      sg_dfa_free(*dfa);
      *dfa = NULL;
      return SG_ERR_NONDETERMINISTIC;
    }
    (*dfa)->transitions[index] = target;
  }
  return SG_OK;
}

void sg_dfa_free(sg_dfa *dfa) {
  if (dfa == NULL) {
    return;
  }
  if (dfa->states != NULL) {
    for (size_t i = 0U; i < dfa->state_count; ++i) {
      free(dfa->states[i]);
    }
  }
  if (dfa->letters != NULL) {
    for (size_t i = 0U; i < dfa->letter_count; ++i) {
      free(dfa->letters[i]);
    }
  }
  free((void *)dfa->states);
  free((void *)dfa->letters);
  free(dfa->transitions);
  free(dfa);
}

size_t sg_dfa_state_count(const sg_dfa *dfa) {
  return (dfa == NULL) ? 0U : dfa->state_count;
}

size_t sg_dfa_letter_count(const sg_dfa *dfa) {
  return (dfa == NULL) ? 0U : dfa->letter_count;
}

size_t sg_dfa_transition_count(const sg_dfa *dfa) {
  if (dfa == NULL) {
    return 0U;
  }
  return dfa->state_count * dfa->letter_count;
}

const char *sg_dfa_state_key(const sg_dfa *dfa, size_t state) {
  if (dfa == NULL || state >= dfa->state_count) {
    return NULL;
  }
  return dfa->states[state];
}

const char *sg_dfa_letter_key(const sg_dfa *dfa, size_t letter) {
  if (dfa == NULL || letter >= dfa->letter_count) {
    return NULL;
  }
  return dfa->letters[letter];
}

size_t sg_dfa_transition(const sg_dfa *dfa, size_t state, size_t letter) {
  if (dfa == NULL || state >= dfa->state_count || letter >= dfa->letter_count) {
    return SG_INVALID;
  }
  return dfa->transitions[(state * dfa->letter_count) + letter];
}

sg_status sg_dfa_find_state(const sg_dfa *dfa, const char *state_key, size_t *state) {
  if (dfa == NULL || state_key == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0U; i < dfa->state_count; ++i) {
    if (strcmp(dfa->states[i], state_key) == 0) {
      if (state != NULL) {
        *state = i;
      }
      return SG_OK;
    }
  }
  return SG_ERR_NOT_FOUND;
}

sg_status sg_dfa_find_letter(const sg_dfa *dfa, const char *letter, size_t *letter_id) {
  if (dfa == NULL || letter == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0U; i < dfa->letter_count; ++i) {
    if (strcmp(dfa->letters[i], letter) == 0) {
      if (letter_id != NULL) {
        *letter_id = i;
      }
      return SG_OK;
    }
  }
  return SG_ERR_NOT_FOUND;
}

static size_t sg_pair_index(size_t state_count, size_t first, size_t second) {
  size_t low = first;
  size_t high = second;
  if (low > high) {
    low = second;
    high = first;
  }
  return (low * state_count) - ((low * (low - 1U)) / 2U) + (high - low);
}

static void sg_pair_from_index(size_t state_count, size_t index, size_t *first, size_t *second) {
  size_t row_start = 0U;
  for (size_t low = 0U; low < state_count; ++low) {
    const size_t row_len = state_count - low;
    if (index < row_start + row_len) {
      *first = low;
      *second = low + (index - row_start);
      return;
    }
    row_start += row_len;
  }
  *first = SG_INVALID;
  *second = SG_INVALID;
}

sg_status sg_pair_oracle_build(const sg_dfa *dfa, sg_pair_oracle **oracle) {
  if (dfa == NULL || oracle == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  if (dfa->state_count == 0U || dfa->letter_count == 0U) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *oracle = NULL;

  sg_pair_oracle *created = calloc(1U, sizeof(*created));
  if (created == NULL) {
    return SG_ERR_ALLOC;
  }
  created->dfa = dfa;
  created->pair_count = (dfa->state_count * (dfa->state_count + 1U)) / 2U;

  sg_status status = sg_checked_mul(created->pair_count, dfa->letter_count, &created->edge_count);
  if (status != SG_OK) {
    sg_pair_oracle_free(created);
    return status;
  }

  const size_t edge_slots = (created->edge_count == 0U) ? 1U : created->edge_count;
  created->forward = malloc(edge_slots * sizeof(created->forward[0]));
  created->reverse_next = malloc(edge_slots * sizeof(created->reverse_next[0]));
  created->reverse_from = malloc(edge_slots * sizeof(created->reverse_from[0]));
  created->reverse_letter = malloc(edge_slots * sizeof(created->reverse_letter[0]));
  created->reverse_head = malloc(created->pair_count * sizeof(created->reverse_head[0]));
  created->dist = malloc(created->pair_count * sizeof(created->dist[0]));
  created->next_pair = malloc(created->pair_count * sizeof(created->next_pair[0]));
  created->witness = malloc(created->pair_count * sizeof(created->witness[0]));
  if (created->forward == NULL || created->reverse_next == NULL || created->reverse_from == NULL ||
      created->reverse_letter == NULL || created->reverse_head == NULL || created->dist == NULL ||
      created->next_pair == NULL || created->witness == NULL) {
    sg_pair_oracle_free(created);
    return SG_ERR_ALLOC;
  }

  for (size_t i = 0U; i < created->pair_count; ++i) {
    created->reverse_head[i] = SG_INVALID;
    created->dist[i] = SG_INVALID;
    created->next_pair[i] = SG_INVALID;
    created->witness[i] = SG_INVALID;
  }

  for (size_t pair = 0U; pair < created->pair_count; ++pair) {
    size_t first = 0U;
    size_t second = 0U;
    sg_pair_from_index(dfa->state_count, pair, &first, &second);
    for (size_t letter = 0U; letter < dfa->letter_count; ++letter) {
      const size_t image_first = sg_dfa_transition(dfa, first, letter);
      const size_t image_second = sg_dfa_transition(dfa, second, letter);
      const size_t image_pair = sg_pair_index(dfa->state_count, image_first, image_second);
      const size_t edge = (pair * dfa->letter_count) + letter;
      created->forward[edge] = image_pair;
      created->reverse_from[edge] = pair;
      created->reverse_letter[edge] = letter;
      created->reverse_next[edge] = created->reverse_head[image_pair];
      created->reverse_head[image_pair] = edge;
    }
  }

  size_t *queue = malloc(created->pair_count * sizeof(queue[0]));
  if (queue == NULL && created->pair_count != 0U) {
    sg_pair_oracle_free(created);
    return SG_ERR_ALLOC;
  }

  size_t head = 0U;
  size_t tail = 0U;
  for (size_t state = 0U; state < dfa->state_count; ++state) {
    const size_t diagonal = sg_pair_index(dfa->state_count, state, state);
    created->dist[diagonal] = 0U;
    queue[tail] = diagonal;
    ++tail;
  }

  while (head < tail) {
    const size_t image_pair = queue[head];
    ++head;
    for (size_t edge = created->reverse_head[image_pair]; edge != SG_INVALID;
         edge = created->reverse_next[edge]) {
      const size_t source_pair = created->reverse_from[edge];
      if (created->dist[source_pair] == SG_INVALID) {
        created->dist[source_pair] = created->dist[image_pair] + 1U;
        created->next_pair[source_pair] = image_pair;
        created->witness[source_pair] = created->reverse_letter[edge];
        queue[tail] = source_pair;
        ++tail;
      }
    }
  }

  free(queue);

  for (size_t pair = 0U; pair < created->pair_count; ++pair) {
    if (created->dist[pair] != SG_INVALID) {
      ++created->mergeable_pairs;
    }
  }
  *oracle = created;
  return SG_OK;
}

void sg_pair_oracle_free(sg_pair_oracle *oracle) {
  if (oracle == NULL) {
    return;
  }
  free(oracle->forward);
  free(oracle->reverse_head);
  free(oracle->reverse_next);
  free(oracle->reverse_from);
  free(oracle->reverse_letter);
  free(oracle->dist);
  free(oracle->next_pair);
  free(oracle->witness);
  free(oracle);
}

size_t sg_pair_oracle_pair_count(const sg_pair_oracle *oracle) {
  return (oracle == NULL) ? 0U : oracle->pair_count;
}

size_t sg_pair_oracle_pair_edge_count(const sg_pair_oracle *oracle) {
  return (oracle == NULL) ? 0U : oracle->edge_count;
}

size_t sg_pair_oracle_mergeable_pair_count(const sg_pair_oracle *oracle) {
  return (oracle == NULL) ? 0U : oracle->mergeable_pairs;
}

sg_status sg_pair_oracle_pair_states(const sg_pair_oracle *oracle, size_t pair, size_t *first,
                                     size_t *second) {
  if (oracle == NULL || oracle->dfa == NULL || first == NULL || second == NULL ||
      pair >= oracle->pair_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_pair_from_index(oracle->dfa->state_count, pair, first, second);
  if (*first == SG_INVALID || *second == SG_INVALID) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  return SG_OK;
}

sg_status sg_pair_oracle_pair_next(const sg_pair_oracle *oracle, size_t pair, size_t letter,
                                   size_t *next_pair) {
  if (oracle == NULL || oracle->dfa == NULL || next_pair == NULL || pair >= oracle->pair_count ||
      letter >= oracle->dfa->letter_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *next_pair = oracle->forward[(pair * oracle->dfa->letter_count) + letter];
  return SG_OK;
}

sg_status sg_pair_oracle_pair_witness(const sg_pair_oracle *oracle, size_t pair, bool *has_witness,
                                      size_t *distance, size_t *letter, size_t *next_pair) {
  if (oracle == NULL || has_witness == NULL || distance == NULL || letter == NULL ||
      next_pair == NULL || pair >= oracle->pair_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *has_witness = oracle->dist[pair] != SG_INVALID;
  *distance = oracle->dist[pair];
  *letter = oracle->witness[pair];
  *next_pair = oracle->next_pair[pair];
  return SG_OK;
}

bool sg_pair_oracle_has_witness(const sg_pair_oracle *oracle, size_t first, size_t second) {
  if (oracle == NULL || oracle->dfa == NULL || first >= oracle->dfa->state_count ||
      second >= oracle->dfa->state_count) {
    return false;
  }
  const size_t pair = sg_pair_index(oracle->dfa->state_count, first, second);
  return oracle->dist[pair] != SG_INVALID;
}

sg_status sg_pair_oracle_witness_word(const sg_pair_oracle *oracle, size_t first, size_t second,
                                      sg_word *word) {
  if (oracle == NULL || word == NULL || oracle->dfa == NULL || first >= oracle->dfa->state_count ||
      second >= oracle->dfa->state_count) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_status status = sg_word_init(word);
  if (status != SG_OK) {
    return status;
  }
  size_t pair = sg_pair_index(oracle->dfa->state_count, first, second);
  if (oracle->dist[pair] == SG_INVALID) {
    return SG_ERR_UNSYNCHRONIZABLE;
  }
  while (oracle->dist[pair] != 0U) {
    status = sg_word_append(word, oracle->witness[pair]);
    if (status != SG_OK) {
      sg_word_free(word);
      return status;
    }
    pair = oracle->next_pair[pair];
  }
  return SG_OK;
}

static size_t sg_bitset_word_count(size_t bits) {
  return (bits + (size_t)SG_BITS_PER_WORD - 1U) / (size_t)SG_BITS_PER_WORD;
}

static sg_status sg_bitset_init(sg_bitset *set, size_t bit_count) {
  if (set == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  set->word_count = sg_bitset_word_count(bit_count);
  set->words = calloc(set->word_count, sizeof(set->words[0]));
  if (set->words == NULL && set->word_count != 0U) {
    return SG_ERR_ALLOC;
  }
  return SG_OK;
}

static void sg_bitset_free(sg_bitset *set) {
  if (set == NULL) {
    return;
  }
  free(set->words);
  set->words = NULL;
  set->word_count = 0U;
}

static void sg_bitset_clear(sg_bitset *set) {
  if (set == NULL) {
    return;
  }
  memset(set->words, 0, set->word_count * sizeof(set->words[0]));
}

static void sg_bitset_set(sg_bitset *set, size_t bit) {
  set->words[bit / (size_t)SG_BITS_PER_WORD] |= UINT64_C(1) << (bit % (size_t)SG_BITS_PER_WORD);
}

static bool sg_bitset_has(const sg_bitset *set, size_t bit) {
  return (set->words[bit / (size_t)SG_BITS_PER_WORD] &
          (UINT64_C(1) << (bit % (size_t)SG_BITS_PER_WORD))) != 0U;
}

static size_t sg_bitset_count(const sg_bitset *set) {
  size_t count = 0U;
  for (size_t i = 0U; i < set->word_count; ++i) {
    count += (size_t)__builtin_popcountll(set->words[i]);
  }
  return count;
}

static bool sg_bitset_equal(const sg_bitset *lhs, const sg_bitset *rhs) {
  if (lhs->word_count != rhs->word_count) {
    return false;
  }
  return memcmp(lhs->words, rhs->words, lhs->word_count * sizeof(lhs->words[0])) == 0;
}

static bool sg_bitset_subset_of(const sg_bitset *small, const sg_bitset *large) {
  if (small->word_count != large->word_count) {
    return false;
  }
  for (size_t i = 0U; i < small->word_count; ++i) {
    if ((small->words[i] & ~large->words[i]) != 0U) {
      return false;
    }
  }
  return true;
}

static sg_status sg_bitset_copy(const sg_bitset *src, sg_bitset *dst) {
  if (src == NULL || dst == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  dst->word_count = src->word_count;
  dst->words = malloc(dst->word_count * sizeof(dst->words[0]));
  if (dst->words == NULL && dst->word_count != 0U) {
    return SG_ERR_ALLOC;
  }
  memcpy(dst->words, src->words, dst->word_count * sizeof(dst->words[0]));
  return SG_OK;
}

static sg_status sg_bitset_from_ids(size_t state_count, const size_t *ids, size_t id_count,
                                    sg_bitset *set) {
  sg_status status = sg_bitset_init(set, state_count);
  if (status != SG_OK) {
    return status;
  }
  for (size_t i = 0U; i < id_count; ++i) {
    if (ids[i] >= state_count) {
      sg_bitset_free(set);
      return SG_ERR_INVALID_ARGUMENT;
    }
    sg_bitset_set(set, ids[i]);
  }
  return SG_OK;
}

static sg_status sg_ids_from_bitset(const sg_bitset *set, size_t state_count, size_t *ids,
                                    size_t *id_count) {
  if (set == NULL || ids == NULL || id_count == NULL) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  size_t written = 0U;
  for (size_t state = 0U; state < state_count; ++state) {
    if (sg_bitset_has(set, state)) {
      ids[written] = state;
      ++written;
    }
  }
  *id_count = written;
  return SG_OK;
}

static sg_status sg_visit_cache_node(const sg_dfa *dfa, const sg_bitset *set, const sg_word *word,
                                     sg_cache_visitor visitor, void *ctx) {
  if (visitor == NULL) {
    return SG_OK;
  }
  size_t *states = malloc(dfa->state_count * sizeof(states[0]));
  size_t state_count = 0U;
  if (states == NULL && dfa->state_count != 0U) {
    return SG_ERR_ALLOC;
  }
  sg_status status = sg_ids_from_bitset(set, dfa->state_count, states, &state_count);
  if (status == SG_OK) {
    status = visitor(ctx, states, state_count, word);
  }
  free(states);
  return status;
}

static sg_status sg_apply_letter_to_bitset(const sg_dfa *dfa, const sg_bitset *input, size_t letter,
                                           sg_bitset *output) {
  sg_bitset_clear(output);
  for (size_t state = 0U; state < dfa->state_count; ++state) {
    if (sg_bitset_has(input, state)) {
      sg_bitset_set(output, sg_dfa_transition(dfa, state, letter));
    }
  }
  return SG_OK;
}

static sg_status sg_apply_word_to_bitset(const sg_dfa *dfa, const sg_bitset *initial,
                                         const sg_word *word, sg_bitset *output) {
  sg_bitset current = {0};
  sg_bitset next = {0};
  sg_status status = sg_bitset_copy(initial, &current);
  if (status != SG_OK) {
    return status;
  }
  status = sg_bitset_init(&next, dfa->state_count);
  if (status != SG_OK) {
    sg_bitset_free(&current);
    return status;
  }
  for (size_t i = 0U; i < word->length; ++i) {
    (void)sg_apply_letter_to_bitset(dfa, &current, word->letters[i], &next);
    sg_bitset temp = current;
    current = next;
    next = temp;
  }
  sg_bitset_free(&next);
  *output = current;
  return SG_OK;
}

static bool sg_bitset_intersects_ids(const sg_bitset *set, const size_t *ids, size_t id_count) {
  for (size_t i = 0U; i < id_count; ++i) {
    if (sg_bitset_has(set, ids[i])) {
      return true;
    }
  }
  return false;
}

static bool sg_target_condition_holds(const sg_bitset *final, const size_t *targets,
                                      size_t target_count, sg_mode mode) {
  const size_t count = sg_bitset_count(final);
  if (mode == SG_MODE_SYNC) {
    return count <= 1U;
  }
  if (target_count == 0U) {
    return false;
  }
  for (size_t word = 0U; word < final->word_count; ++word) {
    uint64_t remaining = final->words[word];
    while (remaining != 0U) {
      const size_t offset = (size_t)__builtin_ctzll(remaining);
      const size_t state = (word * (size_t)SG_BITS_PER_WORD) + offset;
      bool found = false;
      for (size_t i = 0U; i < target_count; ++i) {
        if (targets[i] == state) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
      remaining &= remaining - 1U;
    }
  }
  if (mode == SG_MODE_REACH) {
    return true;
  }
  return count == 1U && sg_bitset_intersects_ids(final, targets, target_count);
}

static sg_status sg_subset_vec_push(sg_subset_vec *vec, sg_bitset *set, sg_word *word) {
  sg_status status =
      sg_grow((void **)&vec->items, sizeof(vec->items[0]), &vec->capacity, vec->length + 1U);
  if (status != SG_OK) {
    return status;
  }
  vec->items[vec->length].set = *set;
  vec->items[vec->length].word = *word;
  ++vec->length;
  set->words = NULL;
  set->word_count = 0U;
  word->letters = NULL;
  word->length = 0U;
  word->capacity = 0U;
  return SG_OK;
}

static void sg_subset_vec_free(sg_subset_vec *vec) {
  if (vec == NULL) {
    return;
  }
  for (size_t i = 0U; i < vec->length; ++i) {
    sg_bitset_free(&vec->items[i].set);
    sg_word_free(&vec->items[i].word);
  }
  free(vec->items);
  vec->items = NULL;
  vec->length = 0U;
  vec->capacity = 0U;
}

static bool sg_subset_vec_prunes(const sg_subset_vec *vec, const sg_bitset *candidate) {
  for (size_t i = 0U; i < vec->length; ++i) {
    if (sg_bitset_equal(&vec->items[i].set, candidate) ||
        sg_bitset_subset_of(candidate, &vec->items[i].set)) {
      return true;
    }
  }
  return false;
}

static sg_status sg_preimage(const sg_dfa *dfa, const sg_bitset *target, size_t letter,
                             sg_bitset *preimage) {
  sg_status status = sg_bitset_init(preimage, dfa->state_count);
  if (status != SG_OK) {
    return status;
  }
  for (size_t state = 0U; state < dfa->state_count; ++state) {
    const size_t image = sg_dfa_transition(dfa, state, letter);
    if (sg_bitset_has(target, image)) {
      sg_bitset_set(preimage, state);
    }
  }
  return SG_OK;
}

static sg_status sg_seed_reverse_queue(const sg_dfa *dfa, const size_t *targets,
                                       size_t target_count, sg_mode mode, sg_subset_vec *queue) {
  sg_status status = SG_OK;
  if (mode == SG_MODE_REACH) {
    sg_bitset seed = {0};
    sg_word seed_word = {0};
    status = sg_bitset_from_ids(dfa->state_count, targets, target_count, &seed);
    if (status != SG_OK) {
      return status;
    }
    status = sg_word_init(&seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      return status;
    }
    status = sg_subset_vec_push(queue, &seed, &seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      sg_word_free(&seed_word);
    }
    return status;
  }

  const bool all_singletons = (mode == SG_MODE_SYNC && target_count == 0U);
  const size_t seed_count = all_singletons ? dfa->state_count : target_count;
  for (size_t i = 0U; i < seed_count; ++i) {
    const size_t target = all_singletons ? i : targets[i];
    sg_bitset seed = {0};
    sg_word seed_word = {0};
    status = sg_bitset_init(&seed, dfa->state_count);
    if (status != SG_OK) {
      return status;
    }
    sg_bitset_set(&seed, target);
    status = sg_word_init(&seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      return status;
    }
    status = sg_subset_vec_push(queue, &seed, &seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      sg_word_free(&seed_word);
      return status;
    }
  }
  return SG_OK;
}

static sg_status sg_exact_reverse_search(const sg_dfa *dfa, const sg_bitset *initial,
                                         const size_t *targets, size_t target_count, sg_mode mode,
                                         size_t budget, sg_word *word) {
  if (budget == 0U) {
    return SG_ERR_RESOURCE_BOUND;
  }

  sg_subset_vec queue = {0};
  sg_subset_vec antichain = {0};
  size_t head = 0U;
  sg_status status = SG_OK;

  if (mode == SG_MODE_REACH) {
    sg_bitset seed = {0};
    sg_word seed_word = {0};
    status = sg_bitset_from_ids(dfa->state_count, targets, target_count, &seed);
    if (status != SG_OK) {
      goto cleanup;
    }
    status = sg_word_init(&seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      goto cleanup;
    }
    status = sg_subset_vec_push(&queue, &seed, &seed_word);
    if (status != SG_OK) {
      sg_bitset_free(&seed);
      sg_word_free(&seed_word);
      goto cleanup;
    }
  } else {
    const bool all_singletons = (mode == SG_MODE_SYNC && target_count == 0U);
    const size_t seed_count = all_singletons ? dfa->state_count : target_count;
    for (size_t i = 0U; i < seed_count; ++i) {
      const size_t target = all_singletons ? i : targets[i];
      sg_bitset seed = {0};
      sg_word seed_word = {0};
      status = sg_bitset_init(&seed, dfa->state_count);
      if (status != SG_OK) {
        goto cleanup;
      }
      sg_bitset_set(&seed, target);
      status = sg_word_init(&seed_word);
      if (status != SG_OK) {
        sg_bitset_free(&seed);
        goto cleanup;
      }
      status = sg_subset_vec_push(&queue, &seed, &seed_word);
      if (status != SG_OK) {
        sg_bitset_free(&seed);
        sg_word_free(&seed_word);
        goto cleanup;
      }
    }
  }

  while (head < queue.length && head < budget) {
    sg_bitset current_set = {0};
    sg_word current_word = {0};
    status = sg_bitset_copy(&queue.items[head].set, &current_set);
    if (status != SG_OK) {
      goto cleanup;
    }
    status = sg_word_copy(&queue.items[head].word, &current_word);
    if (status != SG_OK) {
      sg_bitset_free(&current_set);
      goto cleanup;
    }

    if (sg_bitset_subset_of(initial, &current_set)) {
      status = sg_word_copy(&current_word, word);
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }

    for (size_t letter = 0U; letter < dfa->letter_count; ++letter) {
      sg_bitset preimage = {0};
      sg_word next_word = {0};
      status = sg_preimage(dfa, &current_set, letter, &preimage);
      if (status != SG_OK) {
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      if (sg_subset_vec_prunes(&antichain, &preimage) || sg_subset_vec_prunes(&queue, &preimage)) {
        sg_bitset_free(&preimage);
        continue;
      }
      status = sg_word_copy(&current_word, &next_word);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      status = sg_word_prepend(&next_word, letter);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_word_free(&next_word);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      status = sg_subset_vec_push(&queue, &preimage, &next_word);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_word_free(&next_word);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
    }

    sg_bitset antichain_set = {0};
    sg_word antichain_word = {0};
    status = sg_bitset_copy(&current_set, &antichain_set);
    if (status != SG_OK) {
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }
    status = sg_word_copy(&current_word, &antichain_word);
    if (status != SG_OK) {
      sg_bitset_free(&antichain_set);
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }
    status = sg_subset_vec_push(&antichain, &antichain_set, &antichain_word);
    if (status != SG_OK) {
      sg_bitset_free(&antichain_set);
      sg_word_free(&antichain_word);
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }
    sg_bitset_free(&current_set);
    sg_word_free(&current_word);
    ++head;
  }

  status = SG_ERR_RESOURCE_BOUND;

cleanup:
  sg_subset_vec_free(&queue);
  sg_subset_vec_free(&antichain);
  return status;
}

sg_status sg_expand_cache_visit(const sg_dfa *dfa, const size_t *targets, size_t target_count,
                                sg_mode mode, size_t budget, sg_cache_visitor visitor, void *ctx,
                                size_t *expanded, size_t *cache_size) {
  if (dfa == NULL || expanded == NULL || cache_size == NULL ||
      (targets == NULL && target_count != 0U)) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *expanded = 0U;
  *cache_size = 0U;
  if (budget == 0U) {
    return SG_ERR_RESOURCE_BOUND;
  }

  sg_subset_vec queue = {0};
  sg_subset_vec antichain = {0};
  sg_status status = SG_OK;
  size_t head = 0U;

  status = sg_seed_reverse_queue(dfa, targets, target_count, mode, &queue);
  if (status != SG_OK) {
    goto cleanup;
  }

  while (head < queue.length && *expanded < budget) {
    sg_bitset current_set = {0};
    sg_word current_word = {0};
    status = sg_bitset_copy(&queue.items[head].set, &current_set);
    if (status != SG_OK) {
      goto cleanup;
    }
    status = sg_word_copy(&queue.items[head].word, &current_word);
    if (status != SG_OK) {
      sg_bitset_free(&current_set);
      goto cleanup;
    }

    status = sg_visit_cache_node(dfa, &current_set, &current_word, visitor, ctx);
    if (status != SG_OK) {
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }

    for (size_t letter = 0U; letter < dfa->letter_count; ++letter) {
      sg_bitset preimage = {0};
      sg_word next_word = {0};
      status = sg_preimage(dfa, &current_set, letter, &preimage);
      if (status != SG_OK) {
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      if (sg_subset_vec_prunes(&antichain, &preimage) || sg_subset_vec_prunes(&queue, &preimage)) {
        sg_bitset_free(&preimage);
        continue;
      }
      status = sg_word_copy(&current_word, &next_word);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      status = sg_word_prepend(&next_word, letter);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_word_free(&next_word);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
      status = sg_subset_vec_push(&queue, &preimage, &next_word);
      if (status != SG_OK) {
        sg_bitset_free(&preimage);
        sg_word_free(&next_word);
        sg_bitset_free(&current_set);
        sg_word_free(&current_word);
        goto cleanup;
      }
    }

    sg_bitset antichain_set = {0};
    sg_word antichain_word = {0};
    status = sg_bitset_copy(&current_set, &antichain_set);
    if (status != SG_OK) {
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }
    status = sg_word_copy(&current_word, &antichain_word);
    if (status != SG_OK) {
      sg_bitset_free(&antichain_set);
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }
    status = sg_subset_vec_push(&antichain, &antichain_set, &antichain_word);
    if (status != SG_OK) {
      sg_bitset_free(&antichain_set);
      sg_word_free(&antichain_word);
      sg_bitset_free(&current_set);
      sg_word_free(&current_word);
      goto cleanup;
    }

    sg_bitset_free(&current_set);
    sg_word_free(&current_word);
    ++head;
    ++*expanded;
  }

  *cache_size = queue.length;
  status = (head < queue.length) ? SG_ERR_RESOURCE_BOUND : SG_OK;

cleanup:
  if (status != SG_OK && status != SG_ERR_RESOURCE_BOUND) {
    *expanded = 0U;
    *cache_size = 0U;
  } else {
    *cache_size = queue.length;
  }
  sg_subset_vec_free(&queue);
  sg_subset_vec_free(&antichain);
  return status;
}

sg_status sg_expand_cache(const sg_dfa *dfa, const size_t *targets, size_t target_count,
                          sg_mode mode, size_t budget, size_t *expanded, size_t *cache_size) {
  return sg_expand_cache_visit(dfa, targets, target_count, mode, budget, NULL, NULL, expanded,
                               cache_size);
}

static sg_status sg_greedy_sync(const sg_dfa *dfa, const sg_pair_oracle *oracle,
                                const sg_bitset *initial, sg_word *word, sg_bitset *final) {
  sg_status status = sg_word_init(word);
  if (status != SG_OK) {
    return status;
  }

  sg_bitset active = {0};
  sg_bitset next = {0};
  status = sg_bitset_copy(initial, &active);
  if (status != SG_OK) {
    sg_word_free(word);
    return status;
  }
  status = sg_bitset_init(&next, dfa->state_count);
  if (status != SG_OK) {
    sg_word_free(word);
    sg_bitset_free(&active);
    return status;
  }

  while (sg_bitset_count(&active) > 1U) {
    size_t best_first = SG_INVALID;
    size_t best_second = SG_INVALID;
    size_t best_dist = SG_INVALID;

    for (size_t first = 0U; first < dfa->state_count; ++first) {
      if (!sg_bitset_has(&active, first)) {
        continue;
      }
      for (size_t second = first + 1U; second < dfa->state_count; ++second) {
        if (!sg_bitset_has(&active, second)) {
          continue;
        }
        const size_t pair = sg_pair_index(dfa->state_count, first, second);
        if (oracle->dist[pair] != SG_INVALID && oracle->dist[pair] < best_dist) {
          best_dist = oracle->dist[pair];
          best_first = first;
          best_second = second;
        }
      }
    }

    if (best_first == SG_INVALID || best_second == SG_INVALID) {
      sg_bitset_free(&active);
      sg_bitset_free(&next);
      sg_word_free(word);
      return SG_ERR_UNSYNCHRONIZABLE;
    }

    sg_word witness = {0};
    status = sg_pair_oracle_witness_word(oracle, best_first, best_second, &witness);
    if (status != SG_OK) {
      sg_bitset_free(&active);
      sg_bitset_free(&next);
      sg_word_free(word);
      return status;
    }
    for (size_t i = 0U; i < witness.length; ++i) {
      (void)sg_apply_letter_to_bitset(dfa, &active, witness.letters[i], &next);
      sg_bitset temp = active;
      active = next;
      next = temp;
    }
    status = sg_word_extend(word, &witness);
    sg_word_free(&witness);
    if (status != SG_OK) {
      sg_bitset_free(&active);
      sg_bitset_free(&next);
      sg_word_free(word);
      return status;
    }
  }

  sg_bitset_free(&next);
  *final = active;
  return SG_OK;
}

sg_status sg_word_for_set(const sg_dfa *dfa, const sg_pair_oracle *oracle, const size_t *initial,
                          size_t initial_count, const size_t *targets, size_t target_count,
                          sg_mode mode, size_t exact_budget, sg_word_result *result) {
  if (dfa == NULL || oracle == NULL || result == NULL || (initial == NULL && initial_count != 0U) ||
      (targets == NULL && target_count != 0U)) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  result->kind = SG_RESULT_FAILURE;
  result->status = SG_OK;
  result->final_state = SG_INVALID;
  result->final_count = 0U;
  sg_status status = sg_word_init(&result->word);
  if (status != SG_OK) {
    return status;
  }

  sg_bitset initial_set = {0};
  status = sg_bitset_from_ids(dfa->state_count, initial, initial_count, &initial_set);
  if (status != SG_OK) {
    sg_word_result_free(result);
    return status;
  }

  const size_t unique_initial_count = sg_bitset_count(&initial_set);
  if (unique_initial_count <= 1U) {
    result->kind = SG_RESULT_TRIVIAL;
    result->status = SG_OK;
    result->final_count = unique_initial_count;
    if (unique_initial_count == 1U) {
      size_t states[1] = {0U};
      size_t count = 0U;
      (void)sg_ids_from_bitset(&initial_set, dfa->state_count, states, &count);
      result->final_state = states[0];
    }
    sg_bitset_free(&initial_set);
    return SG_OK;
  }

  if (mode == SG_MODE_REACH &&
      sg_target_condition_holds(&initial_set, targets, target_count, mode)) {
    result->kind = SG_RESULT_TRIVIAL;
    result->status = SG_OK;
    result->final_count = unique_initial_count;
    sg_bitset_free(&initial_set);
    return SG_OK;
  }

  sg_word greedy_word = {0};
  sg_bitset greedy_final = {0};
  status = sg_greedy_sync(dfa, oracle, &initial_set, &greedy_word, &greedy_final);
  if (status == SG_OK) {
    const bool target_ok = sg_target_condition_holds(&greedy_final, targets, target_count, mode);
    if (mode == SG_MODE_SYNC || target_ok) {
      result->kind =
          (mode == SG_MODE_SYNC) ? SG_RESULT_PAIR_GREEDY : SG_RESULT_PAIR_GREEDY_TARGETED;
      result->status = SG_OK;
      result->word = greedy_word;
      result->final_count = sg_bitset_count(&greedy_final);
      if (result->final_count == 1U) {
        size_t states[1] = {0U};
        size_t count = 0U;
        (void)sg_ids_from_bitset(&greedy_final, dfa->state_count, states, &count);
        result->final_state = states[0];
      }
      sg_bitset_free(&greedy_final);
      sg_bitset_free(&initial_set);
      return SG_OK;
    }
  }
  sg_word_free(&greedy_word);
  sg_bitset_free(&greedy_final);

  sg_word exact_word = {0};
  status = sg_exact_reverse_search(dfa, &initial_set, targets, target_count, mode, exact_budget,
                                   &exact_word);
  if (status == SG_OK) {
    sg_bitset exact_final = {0};
    status = sg_apply_word_to_bitset(dfa, &initial_set, &exact_word, &exact_final);
    if (status != SG_OK) {
      sg_word_free(&exact_word);
      sg_bitset_free(&initial_set);
      return status;
    }
    result->kind = SG_RESULT_EXACT_EXPANDED;
    result->status = SG_OK;
    result->word = exact_word;
    result->final_count = sg_bitset_count(&exact_final);
    if (result->final_count == 1U) {
      size_t states[1] = {0U};
      size_t count = 0U;
      (void)sg_ids_from_bitset(&exact_final, dfa->state_count, states, &count);
      result->final_state = states[0];
    }
    sg_bitset_free(&exact_final);
    sg_bitset_free(&initial_set);
    return SG_OK;
  }

  result->kind = (status == SG_ERR_RESOURCE_BOUND) ? SG_RESULT_RESOURCE_BOUND : SG_RESULT_FAILURE;
  result->status = status;
  sg_bitset_free(&initial_set);
  return status;
}

void sg_word_result_free(sg_word_result *result) {
  if (result == NULL) {
    return;
  }
  sg_word_free(&result->word);
  result->kind = SG_RESULT_FAILURE;
  result->status = SG_OK;
  result->final_state = SG_INVALID;
  result->final_count = 0U;
}

sg_status sg_apply_word_to_set(const sg_dfa *dfa, const size_t *initial, size_t initial_count,
                               const sg_word *word, size_t *output, size_t *output_count) {
  if (dfa == NULL || word == NULL || output == NULL || output_count == NULL ||
      (initial == NULL && initial_count != 0U)) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  sg_bitset initial_set = {0};
  sg_status status = sg_bitset_from_ids(dfa->state_count, initial, initial_count, &initial_set);
  if (status != SG_OK) {
    return status;
  }
  sg_bitset final_set = {0};
  status = sg_apply_word_to_bitset(dfa, &initial_set, word, &final_set);
  sg_bitset_free(&initial_set);
  if (status != SG_OK) {
    return status;
  }
  status = sg_ids_from_bitset(&final_set, dfa->state_count, output, output_count);
  sg_bitset_free(&final_set);
  return status;
}

sg_status sg_explain_word(const sg_dfa *dfa, const size_t *initial, size_t initial_count,
                          const sg_word *word, size_t **steps, size_t **step_counts,
                          size_t *step_count) {
  if (dfa == NULL || word == NULL || steps == NULL || step_counts == NULL || step_count == NULL ||
      (initial == NULL && initial_count != 0U)) {
    return SG_ERR_INVALID_ARGUMENT;
  }
  *steps = NULL;
  *step_counts = NULL;
  *step_count = word->length + 1U;

  size_t *created_steps = calloc(*step_count * dfa->state_count, sizeof(created_steps[0]));
  size_t *created_counts = calloc(*step_count, sizeof(created_counts[0]));
  if ((created_steps == NULL && *step_count * dfa->state_count != 0U) || created_counts == NULL) {
    free(created_steps);
    free(created_counts);
    return SG_ERR_ALLOC;
  }

  sg_bitset active = {0};
  sg_bitset next = {0};
  sg_status status = sg_bitset_from_ids(dfa->state_count, initial, initial_count, &active);
  if (status != SG_OK) {
    free(created_steps);
    free(created_counts);
    return status;
  }
  status = sg_bitset_init(&next, dfa->state_count);
  if (status != SG_OK) {
    free(created_steps);
    free(created_counts);
    sg_bitset_free(&active);
    return status;
  }

  for (size_t step = 0U; step < *step_count; ++step) {
    status = sg_ids_from_bitset(&active, dfa->state_count, &created_steps[step * dfa->state_count],
                                &created_counts[step]);
    if (status != SG_OK) {
      free(created_steps);
      free(created_counts);
      sg_bitset_free(&active);
      sg_bitset_free(&next);
      return status;
    }
    if (step < word->length) {
      (void)sg_apply_letter_to_bitset(dfa, &active, word->letters[step], &next);
      sg_bitset temp = active;
      active = next;
      next = temp;
    }
  }

  sg_bitset_free(&active);
  sg_bitset_free(&next);
  *steps = created_steps;
  *step_counts = created_counts;
  return SG_OK;
}

void sg_explain_free(size_t *steps, size_t *step_counts) {
  free(steps);
  free(step_counts);
}
