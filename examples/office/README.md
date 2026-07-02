# Office Example

Run the files in numeric order after installing `sync.so` into Memgraph's query
module directory and loading it with:

```cypher
CALL mg.load("sync");
```

The example materializes a three-state automaton and asks for a word that maps
the hypothesis set `["A", "B"]` into the target `["C"]`.
