# Persimmon

[![Test Status][icon]][status]

[icon]: https://github.com/pyrmont/persimmon/workflows/test/badge.svg
[status]: https://github.com/pyrmont/persimmon/actions?query=workflow%3Atest

Persimmon is a C library providing four persistent immutable data
structures: a vector, a list, a map and a set.

Persimmon is designed to be simple to use directly in C or via bindings from
another language. An example binding for the [Janet][] programming language is
included in the project.

[Janet]: https://janet-lang.org

## Terminology

In Persimmon:

- A **persistent** collection preserves its earlier versions. It does not mean
  that the value is written to disk. An operation that would normally change
  its input collection returns a new collection instead, leaving the input
  available for further use.

- An **immutable** collection does not change its contents. This makes it safe
  for several versions to coexist without one update changing another.

- A collection uses **structural sharing** to avoid making complete copies. The
  input collection and the resulting collection reuse the internal nodes they
  have in common. Internal reference counts keep shared nodes alive until the
  last collection using them is deinitialised.

- A **transient** collection is a temporary, uniquely owned mutable view used
  to apply a batch of vector, map or set updates efficiently. It may begin
  empty or share a persistent source; shared paths are copied when first
  changed and can then be reused by later edits. Converting the transient back
  to a persistent collection consumes it. Lists have no transient form because
  prepending and taking the rest already take constant time without copying the
  shared tail.

## Implementation

Persimmon implements its four persistent immutable collections as follows:

| Structure | Representation                        | Cheap Operations      |
| --------- | ------------------------------------- | --------------------- |
| Vector    | 32-way bit-partitioned trie with tail | index, append         |
| List      | Singly linked chain of cells          | first, rest, prepend  |
| Map       | 32-way CHAMP hash array mapped trie   | lookup, store, remove |
| Set       | The same trie over keys alone         | lookup, add, remove   |

The **vector** is a trie of the kind Clojure popularised: 32 items to a node,
with a tail buffer so that repeated appends usually touch nothing but the last
node.

The **list** is a cons list, so two lists share every cell from their first
common element onwards and prepending costs one cell however long the list is.
Indexing a list is linear. Iterating one is not: a host can keep a cursor that
lets a read resume from where the last one stopped, so walking a list front to
back costs one step per element. An index behind the cursor, into a different
list, or into a list changed since the cursor was used simply starts again from
the head.

The **map** is a compressed hash-array mapped prefix-tree (i.e. CHAMP) trie.
Five bits of a key's hash choose a slot at each level, and a node carries two
bitmaps: one for the slots holding an entry and one for the slots holding a
child. The second bitmap also keeps a trie in canonical form, so two maps
holding the same keys have the same shape however they were built. They
therefore iterate in the same order, and a map that reached its contents by
adding and removing entries is indistinguishable from one handed them outright.
Keys whose hashes agree in all 32 bits are the exception: they share a
collision node and sit in the order they arrived.

The **set** is the same trie over entries that are keys and nothing else, so the
two share their whole implementation.

### Structure

The source code is separated into three components:

```
include/                the public C header
src/                    the core C source
src/bind/janet/         the Janet binding
```

### Lifecycle

The elements stored in all the collections are treated as opaque blobs of a
fixed size and stored inline in internal nodes and cells. A library consumer
may provide lifecycle functions for managed elements:

```c
/* used by vectors, lists and maps */
typedef struct {
    void (*retain)(const void *slot, void *ctx);
    void (*release)(const void *slot, void *ctx);
    void (*trace)(const void *slot, void *ctx);
} persimm_elem_ops;

/* used by maps and sets */
typedef struct {
    uint32_t (*hash)(const void *key, size_t key_size, void *ctx);
    bool (*equals)(const void *key_a, const void *key_b, size_t key_size, void *ctx);
    void (*retain)(const void *slot, void *ctx);
    void (*release)(const void *slot, void *ctx);
    void (*trace)(const void *slot, void *ctx);
} persimm_key_ops;
```

A few notes:

- A host that reference counts fills in `retain` and `release`. A host with a
  tracing collector fills in `trace`. A host storing plain data can leave those
  callbacks NULL.

- Vectors and lists use `persimm_elem_ops` for their elements while maps use it
  for their values.

- Maps and sets use `persimm_key_ops` for their keys. If `hash` or `equals` is
  NULL, Persimmon uses FNV-1a over the key bytes or `memcmp`, respectively.
  Those defaults suit plain keys without padding or multiple byte
  representations of the same value; other key types must supply both
  operations. Maps accept separate contexts for their keys and values, so the
  two need not share a representation or ownership scheme.

- Operation tables and their contexts are borrowed. They must outlive the
  collection and every clone or transient derived from it. `static const`
  tables are the recommended choice.

- Node and cell reference counts are atomic wherever the toolchain provides
  atomics, whether through C11 `<stdatomic.h>`, the GCC and Clang builtins, or
  the Windows interlocked functions. `persimm_has_atomic_refcounts` reports
  whether that was the case for a given build. Where it returns false, a graph
  of structures must not be shared across threads.

## Installation

The public C interface is [include/persimmon.h](include/persimmon.h). Building
the core needs a C99 compiler, `make` and `ar`:

```console
$ make
$ make check
$ make example
$ make help
```

The resulting archive is `_build/core/libpersimmon.a`. `make install` installs
it under `$PREFIX/lib` and the public header under `$PREFIX/include`, with
`PREFIX` defaulting to `/usr/local`. `DESTDIR`, `LIBDIR` and `INCLUDEDIR` may
all be overridden for packaging:

```console
$ make install DESTDIR=/tmp/package-root PREFIX=/usr
```

### Vendoring

The core only depends on the C standard library and so can be dropped
straight into another project instead of installed. Copying `src/` and
`include/` and adding `src/*.c` to an existing build is enough: the sources
reach the public header by a relative path, so no include flag is needed to
compile them. Only a project's own use of `persimmon.h` needs to find it.

Alternatively, `make amalgamation` writes the core to `_build/amalgam` as a
single `persimmon.c` and `persimmon.h` pair for users who would rather vendor
two files than eight. The recipe is a shell one-liner, so a POSIX shell is
required.

The update functions for persistent collections take separate source and
destination structures. The destination must be uninitialised and distinct from
the source. The source is unchanged; on success, both structures own references
and must eventually be deinitialised. A failed operation also leaves its
destination safe to deinitialise.

Transient collections can be used for improved performance when repeatedly
updating a vector, map or set. A transient can start empty or share a
persistent source, and is consumed when persisted. Lists need no transient
because prepending and taking the rest already cost constant time.

The complete example in [res/examples/core.c](res/examples/core.c) builds with:

```console
$ make example
$ ./_build/core/persimmon-example
empty: 0, first: 1
first: 1, result: 1 2 3 4 5
```

The [Janet binding](src/bind/janet/README.md) has separate installation and
usage documentation.

## Development

The core C library is built and checked independently of any host language:

```console
$ make
$ make check
```

The core checks cover collision nodes, element lifecycle callbacks and
allocation failures without including a host-language header. The
[Janet binding documentation](src/bind/janet/README.md) describes its build,
tests and benchmarks.

## Bugs

Found a bug? I'd love to know about it. The best way is to report it in the
[Issues][] section on GitHub.

[Issues]: https://github.com/pyrmont/persimmon/issues

## License

Persimmon is licensed under the MIT License. See [LICENSE][] for more details.

[LICENSE]: LICENSE
