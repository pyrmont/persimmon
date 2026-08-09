# Persimmon for Janet

This directory contains the [Janet][] binding for the [Persimmon][] C library.
It stores `Janet` values inline, leaves the core's `retain` and `release`
callbacks NULL, and traces values through `janet_mark`.

[Janet]: https://janet-lang.org
[Persimmon]: ../../../README.md

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

(length v1)             # -> 2
(get v2 2)              # -> :qux
(persimmon/to-array v3) # -> @[:quux :bar :qux]

(def l1 (persimmon/list [:bar :qux]))
(def l2 (persimmon/conj l1 :foo))

(persimmon/first l2)                     # -> :foo
(persimmon/to-array (persimmon/rest l2)) # -> @[:bar :qux]

(def m1 (persimmon/map {:foo 1 :bar 2}))
(def m2 (persimmon/assoc m1 :qux 3))
(def m3 (persimmon/dissoc m2 :foo))

(get m2 :qux)           # -> 3
(persimmon/to-table m3) # -> @{:bar 2 :qux 3}

(def s1 (persimmon/set [:foo :bar]))
(def s2 (persimmon/conj s1 :qux))

(get s2 :qux)                # -> :qux
(persimmon/has-key? s2 :foo) # -> true
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
`(persimmon/assoc m :k nil)` removes the key rather than storing nothing under
it. Nil cannot be a key: it is how Janet's iteration protocol says 'start at
the beginning', so a map or set refuses it.

Two structures of the same kind are equal when they hold the same things, so
`=`, `deep=` and use as a key in a table all work as they do for Janet's own
collections. Two structures of different kinds are never equal, whatever they
hold.

A vector and a list compare element by element, which also orders them, so
sorting either is meaningful. A map and a set carry no order of their own:
they are equal when they hold the same entries, and the order between two that
differ is arbitrary and not to be relied on. Equal structures always hash
alike, so a map may be a key in a table or an element of another set.

Every structure can be marshalled, and so written to a file or sent to another
thread. What is written is the contents rather than the shape, and what comes
back is built from those, which is enough to give back what went in: a map read
back holds its entries in the order the original held them, and hashes the
same. Reading one back needs the module to have been loaded, since that is what
registers the type its name refers to.

Marshalling copies. Two threads that exchange a structure hold one each and
share nothing, so neither has to wait on the other.

## API

Documentation for Persimmon's Janet API is in [api.md](api.md).

## Development

From the repository root, build the binding with [Jeep][]:

```console
$ jeep prep build      # vendor the build files
$ jeep prep vendor     # vendor the test library
$ jeep build
$ jeep test
```

[Jeep]: https://github.com/pyrmont/jeep

Most tests exercise the Janet binding. `test/core.janet` also compiles
`test/core.c` against the core alone and runs what comes out. Those checks
cover collision nodes, which need a deliberately colliding hash to exercise;
`retain` and `release`, which Janet never calls because it traces instead; and
allocation failure, injected at each point along representative trie updates
to check that the original remains intact and no partial path leaks.

Building those checks at all is worth something on its own: they include no
Janet header, so the core failing to be host-agnostic is a link error.

Set `PERSIMMON_SANITISE` to build them with the address and undefined behaviour
sanitisers, which is what the Linux job in CI does:

```console
$ PERSIMMON_SANITISE=1 jeep test
```

The checks are skipped, with a notice, where no C compiler can be found. Set
`CC` to name one.

The repository also has standalone core and binding throughput benchmarks.
They are kept out of the test suite so timing never decides whether a
correctness check passes:

```console
$ janet res/bench/core.janet
$ janet res/bench/janet.janet
```

Run them several times on an otherwise idle machine when comparing revisions.
Set `PERSIMMON_BENCH_SCALE` to an integer from 1 to 100 to lengthen every run.
