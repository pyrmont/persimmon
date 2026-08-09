(use ../deps/testament)

(import ../_build/release/persimmon :as persimmon)


(defn- numbers
  "Builds an array of the integers from 0 to `n`, exclusive"
  [n]
  (def result @[])
  (for i 0 n (array/push result i))
  result)


(deftest list-with-no-items
  (def lst (persimmon/list))
  (is (= 0 (length lst)))
  (is (= nil (persimmon/first lst))))


(deftest list-with-one-item
  (def lst (persimmon/list [:a]))
  (is (= 1 (length lst)))
  (is (= :a (persimmon/first lst)))
  (is (= :a (get lst 0)))
  (is (= nil (get lst 1))))


(deftest list-preserves-the-order-it-is-seeded-with
  (def lst (persimmon/list [:foo :bar :qux]))
  (is (= :foo (persimmon/first lst)))
  (is (== [:foo :bar :qux] (persimmon/to-array lst))))


(deftest conj-prepends-to-a-list
  (def lst1 (persimmon/list [:bar :qux]))
  (def lst2 (persimmon/conj lst1 :foo))
  (is (= :foo (persimmon/first lst2)))
  (is (== [:foo :bar :qux] (persimmon/to-array lst2))))


(deftest conj-does-not-modify-the-original-list
  (def lst1 (persimmon/list [:bar :qux]))
  (def lst2 (persimmon/conj lst1 :foo))
  (is (= 2 (length lst1)))
  (is (= 3 (length lst2)))
  (is (= :bar (persimmon/first lst1))))


(deftest conj-rejects-a-value-that-is-not-a-structure
  (is (thrown? (persimmon/conj @[1 2] 3))))


(deftest first-of-an-empty-list
  (is (= nil (persimmon/first (persimmon/list)))))


(deftest rest-of-a-list
  (def lst1 (persimmon/list [:foo :bar :qux]))
  (def lst2 (persimmon/rest lst1))
  (is (= 2 (length lst2)))
  (is (= :bar (persimmon/first lst2)))
  (is (== [:bar :qux] (persimmon/to-array lst2))))


(deftest rest-of-an-empty-list-is-an-empty-list
  (def lst (persimmon/rest (persimmon/list)))
  (is (= 0 (length lst)))
  (is (= nil (persimmon/first lst))))


(deftest rest-does-not-modify-the-original-list
  (def lst1 (persimmon/list [:foo :bar :qux]))
  (def lst2 (persimmon/rest lst1))
  (is (= 3 (length lst1)))
  (is (= :foo (persimmon/first lst1)))
  (is (== [:foo :bar :qux] (persimmon/to-array lst1)))
  (is (== [:bar :qux] (persimmon/to-array lst2))))


# Taking the rest of a list and consing onto the original leaves both sharing
# every cell from the second element onwards.
(deftest lists-sharing-a-tail-stay-independent
  (def lst1 (persimmon/list [:foo :bar :qux]))
  (def lst2 (persimmon/rest lst1))
  (def lst3 (persimmon/conj lst1 :quux))
  (is (== [:foo :bar :qux] (persimmon/to-array lst1)))
  (is (== [:bar :qux] (persimmon/to-array lst2)))
  (is (== [:quux :foo :bar :qux] (persimmon/to-array lst3))))


# `length` reads the abstract type's length slot while `:length` goes through
# the method table. Both are supported and must agree.
(deftest length-of-a-list
  (is (= 0 (length (persimmon/list))))
  (is (= 0 (:length (persimmon/list))))
  (def lst (persimmon/list (numbers 100)))
  (is (= 100 (length lst)))
  (is (= 100 (:length lst)))
  (is (= 99 (length (persimmon/rest lst))))
  (is (= 101 (length (persimmon/conj lst :head)))))


(deftest get-with-negative-index
  (def lst (persimmon/list [:foo :bar :qux]))
  (is (= :qux (get lst -1)))
  (is (= :foo (get lst -3)))
  (is (= nil (get lst -4)))
  (is (= nil (get lst -2147483648))))


(deftest get-with-out-of-bounds-index
  (def lst (persimmon/list [:foo :bar]))
  (is (= nil (get lst 2)))
  (is (= nil (get lst 2147483647))))


(deftest next-with-empty-list
  (is (= nil (next (persimmon/list)))))


(deftest next-with-non-empty-list
  (def lst (persimmon/list [:foo :bar]))
  (is (= 0   (next lst)))
  (is (= 1   (next lst 0)))
  (is (= nil (next lst 1))))


(deftest iterating-over-a-list
  (def lst (persimmon/list [:foo :bar :qux]))
  (def seen @[])
  (each x lst (array/push seen x))
  (is (== [:foo :bar :qux] seen)))


# `get` resumes from where it last stopped, so these exercise the cases where
# it cannot: an index behind the last one, and a list it has not seen.

(deftest iterating-over-a-list-more-than-once
  (def lst (persimmon/list (numbers 100)))
  (def first-pass @[])
  (def second-pass @[])
  (each x lst (array/push first-pass x))
  (each x lst (array/push second-pass x))
  (is (== (numbers 100) first-pass))
  (is (== first-pass second-pass)))


(deftest reading-a-list-backwards
  (def lst (persimmon/list (numbers 100)))
  (def seen @[])
  (for i 0 100
    (array/push seen (get lst (- 99 i))))
  (is (== (reverse (numbers 100)) seen)))


(deftest reading-a-list-out-of-order
  (def lst (persimmon/list (numbers 100)))
  (each i [0 50 3 99 3 0 98 1]
    (is (= i (get lst i)))))


(deftest iterating-lists-that-share-a-tail
  (def lst1 (persimmon/list (numbers 100)))
  (def lst2 (persimmon/conj lst1 :head))
  (def lst3 (persimmon/rest lst1))
  (def seen1 @[])
  (def seen2 @[])
  (def seen3 @[])
  (each x lst1 (array/push seen1 x))
  (each x lst2 (array/push seen2 x))
  (each x lst3 (array/push seen3 x))
  (is (== (numbers 100) seen1))
  (is (== (array/concat @[:head] ;(numbers 100)) seen2))
  (is (== (array/slice (numbers 100) 1) seen3)))


(deftest stringifying-a-list
  (is (= "()" (string (persimmon/list))))
  (is (= "(foo bar)" (string (persimmon/list [:foo :bar])))))


(deftest hashing-with-equivalent-lists
  (def h1 (hash (persimmon/list [:foo :bar])))
  (def h2 (hash (persimmon/list [:foo :bar])))
  (is (= h1 h2)))


(deftest hashing-with-different-lists
  (def h1 (hash (persimmon/list [:foo :bar])))
  (def h2 (hash (persimmon/list [:bar :foo])))
  (is (not (= h1 h2))))


(deftest a-list-seeded-from-many-items
  (def expect (numbers 1000))
  (def lst (persimmon/list expect))
  (is (= 1000 (length lst)))
  (is (= 0 (persimmon/first lst)))
  (is (= 999 (get lst 999)))
  (is (== expect (persimmon/to-array lst))))


# A chain long enough that releasing it by recursion would exhaust the stack.
(deftest releasing-a-very-long-list
  (var lst (persimmon/list))
  (for i 0 200000
    (set lst (persimmon/conj lst i)))
  (is (= 200000 (length lst)))
  (is (= 199999 (persimmon/first lst)))
  (set lst nil)
  (gccollect)
  (is (= nil lst)))


(deftest comparing-equivalent-lists
  (is (= (persimmon/list) (persimmon/list)))
  (is (= (persimmon/list [:foo :bar]) (persimmon/list [:foo :bar])))
  (is (deep= (persimmon/list [:foo :bar]) (persimmon/list [:foo :bar]))))


(deftest comparing-different-lists
  (is (not (= (persimmon/list [:foo :bar]) (persimmon/list [:bar :foo]))))
  (is (not (= (persimmon/list [:foo]) (persimmon/list [:foo :bar])))))


(deftest ordering-lists
  (def lists (sort @[(persimmon/list [2]) (persimmon/list [1 9]) (persimmon/list [1])]))
  (is (== @[@[1] @[1 9] @[2]] (map |(persimmon/to-array $) lists))))


# Comparing two lists resumes from each one's cursor and leaves both moved, so
# a read after a comparison has to be as good as one before it.
(deftest comparing-does-not-disturb-a-list
  (def lst1 (persimmon/list (numbers 100)))
  (def lst2 (persimmon/list (numbers 100)))
  (is (= lst1 lst2))
  (is (= 50 (get lst1 50)))
  (is (= 50 (get lst2 50)))
  (is (== (numbers 100) (persimmon/to-array lst1))))


(deftest comparing-a-list-with-another-kind-of-structure
  (is (not (= (persimmon/list [1]) (persimmon/vec [1]))))
  (is (not (= (persimmon/list) (persimmon/set)))))


(deftest a-list-as-a-table-key
  (def t @{})
  (put t (persimmon/list [:foo]) :found)
  (is (= :found (get t (persimmon/list [:foo]))))
  (is (= nil (get t (persimmon/list [:bar])))))


(deftest marshalling-a-list
  (def lst1 (persimmon/list [:foo :bar :qux]))
  (def lst2 (unmarshal (marshal lst1)))
  (is (= lst1 lst2))
  (is (= :foo (persimmon/first lst2)))
  (is (== @[:foo :bar :qux] (persimmon/to-array lst2))))


(deftest marshalling-an-empty-list
  (is (= (persimmon/list) (unmarshal (marshal (persimmon/list)))))
  (is (= 0 (length (unmarshal (marshal (persimmon/list)))))))


# Consing puts an element on the front, so a list read back in the order it
# was written comes out reversed and has to be turned around again.
(deftest marshalling-preserves-the-order-of-a-list
  (def lst (unmarshal (marshal (persimmon/list (numbers 1000)))))
  (is (= 1000 (length lst)))
  (is (= 0 (persimmon/first lst)))
  (is (== (numbers 1000) (persimmon/to-array lst))))


# A list read back carries a cursor of its own, which has to start from the
# head of the chain it ended up with rather than the one it was built on.
(deftest reading-a-list-after-it-is-read-back
  (def lst (unmarshal (marshal (persimmon/list (numbers 1000)))))
  (is (= 500 (get lst 500)))
  (is (= 10 (get lst 10)))
  (is (= 999 (get lst 999))))


(run-tests!)
