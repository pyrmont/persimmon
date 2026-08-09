(use ../deps/testament)
(import ../_build/release/persimmon :as persimmon)

(defn- pairings
  "Builds a table mapping the integers from 0 to `n`, exclusive, to their doubles"
  [n]
  (def result @{})
  (for i 0 n (put result i (* 2 i)))
  result)

(defn- entries
  "Returns a map's pairs in a settled order, whatever order it holds them in"
  [m]
  (sorted (persimmon/to-array m)))

# A map is a trie taking five bits of a key's hash at each level, so it gains a
# level roughly every 32 entries. The sizes below sit either side of the first
# few of those boundaries.

(deftest map-with-no-entries
  (is (= 0 (length (persimmon/map))))
  (is (== @{} (persimmon/to-table (persimmon/map)))))

(deftest map-with-one-entry
  (def m (persimmon/map :a 1))
  (is (= 1 (length m)))
  (is (= 1 (get m :a)))
  (is (= nil (get m :b))))

(deftest map-with-several-entries
  (def m (persimmon/map :a 1 :b 2))
  (is (= 2 (length m)))
  (is (= 1 (get m :a)))
  (is (= 2 (get m :b))))

(deftest map-drops-entries-with-a-nil-value
  (def m (persimmon/map :a 1 :b nil))
  (is (= 1 (length m)))
  (is (= nil (get m :b))))

(deftest map-with-entries-across-multiple-levels
  (def expect (pairings 1100))
  (def m (persimmon/into (persimmon/map) expect))
  (is (= 1100 (length m)))
  (is (= 0 (get m 0)))
  (is (= 62 (get m 31)))
  (is (= 64 (get m 32)))
  (is (= 2198 (get m 1099)))
  (is (= nil (get m 1100)))
  (is (== expect (persimmon/to-table m))))

(deftest map-refuses-a-nil-key
  (is (thrown? (persimmon/assoc (persimmon/map) nil 1))))

(deftest map-refuses-an-odd-number-of-arguments
  (is (thrown? (persimmon/map :a)))
  (is (thrown? (persimmon/map :a 1 :b))))

(deftest map-refuses-a-nil-key-at-construction
  (is (thrown? (persimmon/map nil 1))))

(deftest into-a-map-from-a-dictionary
  (def m (persimmon/into (persimmon/map) @{:a 1 :b 2}))
  (is (= 2 (length m)))
  (is (= 1 (get m :a)))
  (is (= 2 (get m :b)))
  (is (= 1 (get (persimmon/into (persimmon/map) {:a 1}) :a))))

(deftest into-a-map-merges-with-what-it-already-holds
  (def m1 (persimmon/map :a 1))
  (def m2 (persimmon/into m1 @{:b 2}))
  (is (== @{:a 1 :b 2} (persimmon/to-table m2)))
  (is (== @{:a 1} (persimmon/to-table m1))))

(deftest into-a-map-prefers-the-source-for-a-shared-key
  (is (== @{:a 9} (persimmon/to-table (persimmon/into (persimmon/map :a 1) @{:a 9})))))

(deftest into-a-map-drops-entries-with-a-nil-value
  (def m (persimmon/into (persimmon/map) @{:a 1 :b nil}))
  (is (= 1 (length m)))
  (is (= nil (get m :b))))

# A map is built from entries, so a source of elements has to supply each one
# as a key-value pair.
(deftest into-a-map-from-pairs
  (is (== @{:a 1 :b 2} (persimmon/to-table (persimmon/into (persimmon/map)
                                                           @[[:a 1] [:b 2]]))))
  (is (== @{:a 1} (persimmon/to-table (persimmon/into (persimmon/map)
                                                      (persimmon/vec [:a 1]))))))

(deftest into-a-map-refuses-an-element-that-is-not-a-pair
  (is (thrown? (persimmon/into (persimmon/map) @[[:a]])))
  (is (thrown? (persimmon/into (persimmon/map) @[1 2])))
  (is (thrown? (persimmon/into (persimmon/map) [:a 1]))))

(deftest into-a-map-refuses-a-nil-key
  (is (thrown? (persimmon/into (persimmon/map) @[[nil 1]]))))

# `length` reads the abstract type's length slot while `:length` goes through
# the method table. Both are supported and must agree.
(deftest length-of-a-map
  (is (= 0 (length (persimmon/map))))
  (is (= 0 (:length (persimmon/map))))
  (def m (persimmon/into (persimmon/map) (pairings 1100)))
  (is (= 1100 (length m)))
  (is (= 1100 (:length m))))

# A map may hold the very keywords the method table answers to, so a key it
# holds is looked up before any method of the same name.
(deftest get-prefers-a-key-over-a-method
  (is (= 99 (get (persimmon/map :length 99) :length)))
  (is (= 1 (:length (persimmon/map :a 1)))))

(deftest get-with-a-missing-key
  (def m (persimmon/map :a 1))
  (is (= nil (get m :b)))
  (is (= nil (get m nil)))
  (is (= :fallback (get m :b :fallback))))

# A map in the operator position answers as get does, so a missing key is nil
# rather than an error and a key it holds still comes before a method.
(deftest calling-a-map
  (def m (persimmon/map :a 1))
  (is (= 1 (m :a)))
  (is (= nil (m :b)))
  (is (= nil (m nil)))
  (is (= :fallback (m :b :fallback)))
  (is (= 1 (m :a :fallback)))
  (is (= 99 ((persimmon/map :length 99) :length)))
  (is (== @[1 nil] (map m [:a :b]))))

(deftest calling-a-map-with-the-wrong-number-of-arguments
  (def m (persimmon/map :a 1))
  (is (thrown? (m)))
  (is (thrown? (m :a :fallback :extra))))

(deftest assoc-with-a-new-key
  (def m1 (persimmon/map :a 1))
  (def m2 (persimmon/assoc m1 :b 2))
  (is (= 1 (length m1)))
  (is (= 2 (length m2)))
  (is (= nil (get m1 :b)))
  (is (= 2 (get m2 :b))))

(deftest assoc-with-an-existing-key
  (def m1 (persimmon/map :a 1 :b 2))
  (def m2 (persimmon/assoc m1 :a 99))
  (is (= 2 (length m2)))
  (is (= 1 (get m1 :a)))
  (is (= 99 (get m2 :a)))
  (is (= 2 (get m2 :b))))

# A nil value is no value, as it is for a Janet table.
(deftest assoc-with-a-nil-value-removes-the-key
  (def m1 (persimmon/map :a 1 :b 2))
  (def m2 (persimmon/assoc m1 :a nil))
  (is (= 1 (length m2)))
  (is (= nil (get m2 :a)))
  (is (= false (persimmon/has-key? m2 :a)))
  (is (= 1 (get m1 :a))))

(deftest assoc-across-multiple-levels
  (var m (persimmon/map))
  (for i 0 1100
    (set m (persimmon/assoc m i (* 2 i))))
  (is (= 1100 (length m)))
  (is (== (pairings 1100) (persimmon/to-table m))))

(deftest dissoc-with-an-existing-key
  (def m1 (persimmon/map :a 1 :b 2))
  (def m2 (persimmon/dissoc m1 :a))
  (is (= 2 (length m1)))
  (is (= 1 (length m2)))
  (is (= 1 (get m1 :a)))
  (is (= nil (get m2 :a)))
  (is (= 2 (get m2 :b))))

(deftest dissoc-with-a-missing-key
  (def m1 (persimmon/map :a 1))
  (def m2 (persimmon/dissoc m1 :b))
  (is (= 1 (length m2)))
  (is (= 1 (get m2 :a))))

(deftest dissoc-back-to-empty
  (var m (persimmon/into (persimmon/map) (pairings 1100)))
  (for i 0 1100
    (set m (persimmon/dissoc m i)))
  (is (= 0 (length m)))
  (is (== @{} (persimmon/to-table m))))

(deftest dissoc-leaves-the-remaining-entries-reachable
  (var m (persimmon/into (persimmon/map) (pairings 1100)))
  (for i 0 1100
    (when (even? i) (set m (persimmon/dissoc m i))))
  (is (= 550 (length m)))
  (for i 0 1100
    (if (even? i)
      (is (= nil (get m i)))
      (is (= (* 2 i) (get m i))))))

(deftest has-key?-with-a-map
  (def m (persimmon/map :a 1))
  (is (= true (persimmon/has-key? m :a)))
  (is (= false (persimmon/has-key? m :b)))
  (is (= false (persimmon/has-key? m nil))))

(deftest next-with-an-empty-map
  (is (= nil (next (persimmon/map)))))

(deftest next-with-a-missing-key
  (is (= nil (next (persimmon/map :a 1) :absent))))

# Janet builds `keys`, `values`, `pairs` and `each` out of `next` and `get`,
# so all of them ride on the map answering those two.
(deftest iterating-a-map
  (def m (persimmon/map :a 1 :b 2 :c 3))
  (is (== @[:a :b :c] (sorted (keys m))))
  (is (== @[1 2 3] (sorted (values m))))
  (is (== @[[:a 1] [:b 2] [:c 3]] (sorted (pairs m))))
  (is (== @[1 2 3] (sorted (seq [v :in m] v)))))

(deftest iterating-a-map-across-multiple-levels
  (def m (persimmon/into (persimmon/map) (pairings 1100)))
  (is (= 1100 (length (keys m))))
  (is (== (sorted (keys (pairings 1100))) (sorted (keys m)))))

# Two maps holding the same entries agree on the order they hand them over,
# however each of them was built.
(deftest iteration-order-does-not-depend-on-how-a-map-was-built
  (def direct (persimmon/into (persimmon/map) (pairings 100)))
  (var grown (persimmon/map))
  (for i 0 100
    (set grown (persimmon/assoc grown i (* 2 i))))
  (var pruned (persimmon/into (persimmon/map) (pairings 200)))
  (for i 100 200
    (set pruned (persimmon/dissoc pruned i)))
  (is (== (keys direct) (keys grown)))
  (is (== (keys direct) (keys pruned))))

(deftest to-array-with-a-map
  (def m (persimmon/map :a 1 :b 2))
  (is (== @[[:a 1] [:b 2]] (entries m))))

(deftest to-table-with-a-map
  (is (== @{:a 1 :b 2} (persimmon/to-table (persimmon/map :a 1 :b 2)))))

(deftest stringifying-a-map
  (is (= "{}" (string (persimmon/map))))
  (is (= "{a 1}" (string (persimmon/map :a 1)))))

(deftest hashing-with-equivalent-maps
  (def h1 (hash (persimmon/map :a 1 :b 2)))
  (def h2 (hash (persimmon/map :b 2 :a 1)))
  (is (= h1 h2)))

# A map's hash cannot lean on the order its entries arrive in, because two
# equal maps need not agree on that order when keys share a hash.
(deftest hashing-does-not-depend-on-how-a-map-was-built
  (def direct (persimmon/into (persimmon/map) (pairings 100)))
  (var grown (persimmon/map))
  (for i 0 100
    (set grown (persimmon/assoc grown i (* 2 i))))
  (is (= (hash direct) (hash grown))))

(deftest hashing-with-different-maps
  (def h1 (hash (persimmon/map :a 1)))
  (def h2 (hash (persimmon/map :a 2)))
  (is (not (= h1 h2))))

(deftest keys-of-every-type
  (def m (persimmon/map :kw 1 "str" 2 'sym 3 42 4 true 5 [1 2] 6))
  (is (= 6 (length m)))
  (is (= 1 (get m :kw)))
  (is (= 2 (get m "str")))
  (is (= 3 (get m 'sym)))
  (is (= 4 (get m 42)))
  (is (= 5 (get m true)))
  (is (= 6 (get m [1 2]))))

# The collector reaches a map's keys and values only through its mark
# callback, so anything it misses is freed while the map still holds it.
(deftest a-map-survives-a-collection
  (def m (persimmon/into (persimmon/map) (pairings 1100)))
  (gccollect)
  (is (= 1100 (length m)))
  (is (== (pairings 1100) (persimmon/to-table m))))

(deftest a-shared-map-survives-its-original
  (var m1 (persimmon/into (persimmon/map) (pairings 1100)))
  (def m2 (persimmon/assoc m1 2000 1))
  (set m1 nil)
  (gccollect)
  (is (= 1101 (length m2)))
  (is (= 2198 (get m2 1099))))

(deftest comparing-equivalent-maps
  (is (= (persimmon/map) (persimmon/map)))
  (is (= (persimmon/map :a 1 :b 2) (persimmon/map :b 2 :a 1)))
  (is (deep= (persimmon/map :a 1) (persimmon/map :a 1))))

# Equality is what canonical form is worth having for: however two maps came
# by their entries, holding the same ones makes them the same map.
(deftest comparing-maps-built-differently
  (def direct (persimmon/into (persimmon/map) (pairings 100)))
  (var grown (persimmon/map))
  (for i 0 100
    (set grown (persimmon/assoc grown i (* 2 i))))
  (var pruned (persimmon/into (persimmon/map) (pairings 200)))
  (for i 100 200
    (set pruned (persimmon/dissoc pruned i)))
  (is (= direct grown))
  (is (= direct pruned)))

(deftest comparing-different-maps
  (is (not (= (persimmon/map :a 1) (persimmon/map :a 2))))
  (is (not (= (persimmon/map :a 1) (persimmon/map :b 1))))
  (is (not (= (persimmon/map :a 1) (persimmon/map :a 1 :b 2)))))

(deftest comparing-maps-across-multiple-levels
  (def map1 (persimmon/into (persimmon/map) (pairings 1100)))
  (is (= map1 (persimmon/into (persimmon/map) (pairings 1100))))
  (is (not (= map1 (persimmon/assoc map1 1099 :qux))))
  (is (not (= map1 (persimmon/dissoc map1 1099)))))

(deftest comparing-a-map-with-another-kind-of-structure
  (is (not (= (persimmon/map) (persimmon/set))))
  (is (not (= (persimmon/map) (persimmon/vec)))))

(deftest a-map-as-a-table-key
  (def t @{})
  (put t (persimmon/map :a 1) :found)
  (is (= :found (get t (persimmon/map :a 1))))
  (is (= nil (get t (persimmon/map :a 2)))))

(deftest marshalling-a-map
  (def map1 (persimmon/map :a 1 :b 2))
  (def map2 (unmarshal (marshal map1)))
  (is (= map1 map2))
  (is (= 1 (get map2 :a)))
  (is (== @{:a 1 :b 2} (persimmon/to-table map2))))

(deftest marshalling-an-empty-map
  (is (= (persimmon/map) (unmarshal (marshal (persimmon/map)))))
  (is (= 0 (length (unmarshal (marshal (persimmon/map)))))))

# A map is kept canonical, so one read back holds its entries in the order the
# one written out held them, and hashes the same.
(deftest marshalling-a-map-across-multiple-levels
  (def map1 (persimmon/into (persimmon/map) (pairings 1100)))
  (def map2 (unmarshal (marshal map1)))
  (is (= map1 map2))
  (is (= (hash map1) (hash map2)))
  (is (== (keys map1) (keys map2)))
  (is (== (pairings 1100) (persimmon/to-table map2))))

# Reading a map back holds each key while its value is read, or the collector
# would be free to take the key before the entry is stored.
(deftest a-map-read-back-survives-a-collection
  (def m (unmarshal (marshal (persimmon/into (persimmon/map) (pairings 1100)))))
  (gccollect)
  (is (= 1100 (length m)))
  (is (== (pairings 1100) (persimmon/to-table m))))

(run-tests!)
