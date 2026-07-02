// Coarse invalidation trigger. Adapt the MATCH predicate to the labels and
// relationship types that feed a deployment's automata view.

CREATE TRIGGER sync_kgraph_mark_dirty
AFTER COMMIT
EXECUTE
  MATCH (m:SyncModel)
  SET m.dirty = true,
      m.generation = coalesce(m.generation, 0) + 1;
