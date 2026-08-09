# Persimmon

[![Test Status][icon]][status]

[icon]: https://github.com/pyrmont/persimmon/workflows/test/badge.svg
[status]: https://github.com/pyrmont/persimmon/actions?query=workflow%3Atest

Persimmon is a C library of persistent immutable data structures, together
with a binding for the [Janet][] programming language.

[Janet]: https://janet-lang.org

## Implementation

A persistent data structure is never modified. An operation that would change
it returns a new structure instead, and the two share as much of their
internal representation as they can.

| Structure | Representation                        | Cheap operations      |
| --------- | ------------------------------------- | --------------------- |
| Vector    | 32-way bit-partitioned trie with tail | index, append         |
| List      | Singly linked chain of cells          | first, rest, prepend  |
| Map       | 32-way CHAMP hash array mapped trie   | lookup, store, remove |
| Set       | The same trie over keys alone         | lookup, add, remove   |

The vector is a trie of the kind Clojure popularised: 32 items to a node, with
a tail buffer so that repeated appends usually touch nothing but the last node.

The list is a cons list, so two lists share every cell from their first common
element onwards and prepending costs one cell however long the list is.
Indexing a list is linear. Iterating one is not: a host can keep a cursor that
lets a read resume from where the last one stopped, so walking a list front to
back costs one step per element. An index behind the cursor, into a different
list, or into a list changed since the cursor was used simply starts again from
the head.

The map is a compressed hash-array mapped prefix-tree (i.e. CHAMP) trie. Five
bits of a key's hash choose a slot at each level, and a node carries two
bitmaps: one for the slots holding an entry and one for the slots holding a
child. The second bitmap also keeps a trie in canonical form, so two maps
holding the same keys have the same shape however they were built. They
therefore iterate in the same order, and a map that reached its contents by
adding and removing entries is indistinguishable from one handed them outright.
Keys whose hashes agree in all 32 bits are the exception: they share a
collision node and sit in the order they arrived.

A set is the same trie over entries that are keys and nothing else, so the
two share their whole implementation.

## Structure

The library is in two layers, and the directories say which is which.

```
src/                    the core, which knows no host language
wrappers/janet/         the Janet wrapper
```

Everything under `src` is the core. Its elements are opaque blobs of a fixed
size, stored inline in the structure, and it delegates their lifecycle to a
table of callbacks the host supplies:

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

`wrappers/janet` holds the Janet wrapper, which is the reference
implementation of that interface. It stores `Janet` values inline, leaves
`retain` and `release` NULL, and traces through `janet_mark`. A wrapper for
another language would sit beside it, and nothing in `src` would change.

Node and cell reference counts are atomic wherever the toolchain provides
atomics, whether through C11 `<stdatomic.h>`, the GCC and Clang builtins, or
the Windows interlocked functions. `persimm_has_atomic_refcounts` reports
whether that was the case for a given build. Where it returns false, a graph
of structures must not be shared across threads.

## Installation

### C

The public C interface is [src/persimmon.h](src/persimmon.h). An embedding can
compile the files under `src` directly, excluding the Janet wrapper, or link
the static archive produced by `jeep build` and add `src` to its header search
path.

Persistent update functions take separate source and destination structures.
The destination must be uninitialised and distinct from the source. The source
is unchanged; on success, both structures own references and must eventually
be deinitialised. A failed operation also leaves its destination safe to
deinitialise.

For several vector, map or set updates, use a transient. A transient can start
empty or share a persistent source, and is consumed when persisted. Lists need
no transient because prepending and taking the rest already cost constant time.

The complete example in [res/examples/core.c](res/examples/core.c) builds with:

```console
$ cc -std=c99 -Isrc res/examples/core.c src/persimmon*.c -o persimmon-example
$ ./persimmon-example
empty: 0, first: 1
first: 1, result: 1 2 3 4 5
```

The core stores opaque, fixed-size values inline. Applications storing managed
objects supply `persimm_elem_ops`; maps additionally describe their entry
layout and, where byte hashing is unsuitable, their key operations. The public
header documents these host integration contracts in full.

The repository also includes a Janet wrapper as an example of how Persimmon
can be integrated into another language.

### Janet

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

(def m1 (persimmon/map {:foo 1 :bar 2}))
(def m2 (persimmon/assoc m1 :qux 3))
(def m3 (persimmon/dissoc m2 :foo))

(get m2 :qux)       # -> 3
(persimmon/to-table m3)
                    # -> @{:bar 2 :qux 3}

(def s1 (persimmon/set [:foo :bar]))
(def s2 (persimmon/conj s1 :qux))

(get s2 :qux)       # -> :qux
(persimmon/has-key? s2 :foo)
                    # -> true
```

As in Clojure, `conj` adds an element wherever the structure takes one most
cheaply — the end of a vector, the front of a list, anywhere in a set.

Vectors, maps and sets can also be changed in a transient when several edits
belong to one operation. The transient is mutable and uniquely owned; turning
it persistent consumes it, and any later attempt to use it is an error:

```janet
(def t (persimmon/transient (persimmon/vec)))
(for i 0 1000
  (persimmon/conj! t i))
(def settled (persimmon/persistent! t))
```

The `!` operations return the transient they receive. Transients support
`length`, `get` and iteration while active, but cannot be compared, hashed or
marshalled. Lists have no transient form: consing already costs one new cell
and never copies the chain it shares.

| Function                       | Result                                       |
| ------------------------------ | -------------------------------------------- |
| `(persimmon/vec &opt coll)`    | a vector, optionally seeded from an indexed  |
| `(persimmon/list &opt coll)`   | a list, optionally seeded from an indexed    |
| `(persimmon/map &opt coll)`    | a map, optionally seeded from a dictionary   |
| `(persimmon/set &opt coll)`    | a set, optionally seeded from an indexed     |
| `(persimmon/conj coll x)`      | a vector, list or set with `x` added         |
| `(persimmon/assoc coll k x)`   | a vector or map with `k` replaced by `x`     |
| `(persimmon/dissoc map k)`     | a map without the key `k`                    |
| `(persimmon/disj set x)`       | a set without the element `x`                |
| `(persimmon/transient coll)`   | a mutable view of a vector, map or set       |
| `(persimmon/persistent! trans)`| consume a transient and return its collection|
| `(persimmon/conj! trans x)`    | append to a vector or add to a set transient |
| `(persimmon/assoc! trans k x)` | update a vector or map transient             |
| `(persimmon/dissoc! trans k)`  | remove a key from a map transient            |
| `(persimmon/disj! trans x)`    | remove an element from a set transient       |
| `(persimmon/has-key? coll k)`  | whether a map or set holds `k`               |
| `(persimmon/first lst)`        | the head of a list, or nil if it is empty    |
| `(persimmon/rest lst)`         | a list without its head                      |
| `(persimmon/to-array coll)`    | the elements as a mutable array              |
| `(persimmon/to-table map)`     | the entries as a mutable table               |

Every structure supports `length`, `get`, `next` and so the whole of Janet's
iteration machinery, `string`, and `hash`. A vector prints as `[foo bar]`, a
list as `(foo bar)`, a map as `{foo 1}` and a set as `#{foo bar}`.

A vector and a list take an index, including a negative one. A map takes a
key and answers with its value, and a set takes an element and answers with
the element, as in Clojure. Because a map may hold the very keywords its
method table answers to, a key it holds is found before any method of the
same name.

Iterating a map or a set goes through the same `next` every Janet dictionary
uses, so `keys`, `values`, `pairs` and `each` all work as they do on a table.
Nothing is kept between steps, so one map may be walked from several places
at once.

A nil value means no entry, as it does for a Janet table, so
`(persimmon/assoc m :k nil)` removes the key rather than storing nothing
under it. Nil cannot be a key: it is how Janet's iteration protocol says
"start at the beginning", so a map or set refuses it.

Two structures of the same kind are equal when they hold the same things, so
`=`, `deep=` and use as a key in a table all work as they do for Janet's own
collections. Two structures of different kinds are never equal, whatever they
hold.

A vector and a list compare element by element, which also orders them, so
sorting either is meaningful. A map and a set carry no order of their own:
they are equal when they hold the same entries, and the order between two
that differ is arbitrary and not to be relied on. Equal structures always
hash alike, so a map may be a key in a table or an element of another set.

Every structure can be marshalled, and so written to a file or sent to
another thread. What is written is the contents rather than the shape, and
what comes back is built from those, which is enough to give back what went
in: a map read back holds its entries in the order the original held them,
and hashes the same. Reading one back needs the module to have been loaded,
since that is what registers the type its name refers to.

Marshalling copies. Two threads that exchange a structure hold one each and
share nothing, so neither has to wait on the other.

## Development

Persimmon is built with [Jeep][]:

```console
$ jeep prep build      # vendor the build files
$ jeep prep vendor     # vendor the test library
$ jeep build
$ jeep test
```

[Jeep]: https://github.com/pyrmont/jeep

Most of the tests exercise the Janet binding, but `test/core.janet` compiles
`test/core.c` against the core alone and runs what comes out. Those checks
cover collision nodes, which need a deliberately colliding hash to exercise;
`retain` and `release`, which Janet never calls because it traces instead; and
allocation failure, injected at each point along representative trie updates
to check that the original remains intact and no partial path leaks.

Building those checks at all is worth something on its own: they include no
Janet header, so the core failing to be host-agnostic is a link error.

Set `PERSIMMON_SANITISE` to build them with the address and undefined
behaviour sanitisers, which is what the Linux job in CI does:

```console
$ PERSIMMON_SANITISE=1 jeep test
```

The checks are skipped, with a notice, where no C compiler can be found. Set
`CC` to name one.

The core also has a standalone throughput benchmark. It is kept out of the
test suite so timing never decides whether a correctness check passes:

```console
$ janet res/bench/core.janet
$ janet res/bench/janet.janet
```

Run it several times on an otherwise idle machine when comparing revisions.
Set `PERSIMMON_BENCH_SCALE` to an integer from 1 to 100 to lengthen every run.

## Bugs

Found a bug? I'd love to know about it. The best way is to report it in the
[Issues][] section on GitHub.

[Issues]: https://github.com/pyrmont/persimmon/issues

## License

Persimmon is licensed under the MIT License. See [LICENSE][] for more details.

[LICENSE]: LICENSE
