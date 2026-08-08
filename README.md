# Persimmon

[![Test Status][icon]][status]

[icon]: https://github.com/pyrmont/persimmon/workflows/test/badge.svg
[status]: https://github.com/pyrmont/persimmon/actions?query=workflow%3Atest

Persimmon is a C library of persistent immutable data structures, together
with a binding for the [Janet][] programming language.

[Janet]: https://janet-lang.org

A persistent data structure is never modified. An operation that would change
it returns a new structure instead, and the two share as much of their
internal representation as they can.

| Structure | Representation                        | Cheap operations      |
| --------- | ------------------------------------- | --------------------- |
| Vector    | 32-way bit-partitioned trie with tail | index, append         |
| List      | Singly linked chain of cells          | first, rest, prepend  |

The vector is a trie of the kind Clojure popularised: 32 items to a node,
with a tail buffer so that repeated appends usually touch nothing but the
last node. The list is a cons list, so two lists share every cell from their
first common element onwards and prepending costs one cell however long the
list is.

Indexing a list is linear, as it is in Clojure. Iterating one is not: each
list keeps a cursor that lets a read resume from where the last one stopped,
so walking a list front to back costs one step per element. An index behind
the cursor, or into a different list, simply starts again from the head.

## Structure

The library is in two layers.

Everything but `src/persimmon_janet.c` is the core, and knows nothing about
any host language. Its elements are opaque blobs of a fixed size, stored
inline in the structure, and it delegates their lifecycle to a table of
callbacks the host supplies:

```c
typedef struct {
    void (*retain)(void *slot, void *ctx);
    void (*release)(void *slot, void *ctx);
    void (*trace)(void *slot, void *ctx);
} persimm_elem_ops;
```

A host that reference counts fills in `retain` and `release`; a host with a
tracing collector fills in `trace`; a host storing plain data passes NULL and
pays for none of it.

`src/persimmon_janet.c` is the Janet binding and the reference implementation
of that interface. It stores `Janet` values inline, leaves `retain` and
`release` NULL, and traces through `janet_mark`.

Node and cell reference counts are atomic wherever the toolchain provides
atomics, whether through C11 `<stdatomic.h>`, the GCC and Clang builtins, or
the Windows interlocked functions. `persimm_has_atomic_refcounts` reports
whether that was the case for a given build. Where it returns false, a graph
of structures must not be shared across threads.

## Installation

Add the dependency to your `info.jdn` file:

```janet
  :dependencies ["https://github.com/pyrmont/persimmon"]
```

## Usage

```janet
(import persimmon)

(def v1 (persimmon/vec [:foo :bar]))
(def v2 (persimmon/conj v1 :qux))
(def v3 (persimmon/assoc v2 0 :quux))

(length v1)         # -> 2
(get v2 2)          # -> :qux
(persimmon/to-array v3)
                    # -> @[:quux :bar :qux]

(def l1 (persimmon/list [:bar :qux]))
(def l2 (persimmon/conj l1 :foo))

(persimmon/first l2)
                    # -> :foo
(persimmon/to-array (persimmon/rest l2))
                    # -> @[:bar :qux]
```

As in Clojure, `conj` adds an element wherever the structure takes one most
cheaply — the end of a vector, the front of a list.

| Function                      | Result                                      |
| ----------------------------- | ------------------------------------------- |
| `(persimmon/vec &opt coll)`   | a vector, optionally seeded from an indexed |
| `(persimmon/list &opt coll)`  | a list, optionally seeded from an indexed   |
| `(persimmon/conj coll x)`     | a vector or list with `x` added             |
| `(persimmon/assoc vec i x)`   | a vector with index `i` replaced by `x`     |
| `(persimmon/first lst)`       | the head of a list, or nil if it is empty   |
| `(persimmon/rest lst)`        | a list without its head                     |
| `(persimmon/to-array coll)`   | the elements as a mutable array             |

Both structures support `length`, `get` (including negative indices), `next`
and so the whole of Janet's iteration machinery, `string`, and `hash`. A
vector prints as `[foo bar]` and a list as `(foo bar)`.

## Development

Persimmon is built with [Jeep][]:

```console
$ jeep prep build      # vendor the build files
$ jeep prep vendor     # vendor the test library
$ jeep build
$ jeep test
```

[Jeep]: https://github.com/pyrmont/jeep

## Bugs

Found a bug? I'd love to know about it. The best way is to report it in the
[Issues][] section on GitHub.

[Issues]: https://github.com/pyrmont/persimmon/issues

## License

Persimmon is licensed under the MIT License. See [LICENSE][] for more details.

[LICENSE]: LICENSE
