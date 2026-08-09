(use ../deps/testament)
(import ../_build/release/persimmon :as persimmon)

(defn- numbers
  "Builds an array of the integers from 0 to `n`, exclusive"
  [n]
  (def result @[])
  (for i 0 n (array/push result i))
  result)

(defn- elements
  "Returns a set's elements in a settled order, whatever order it holds them in"
  [s]
  (sorted (persimmon/to-array s)))

# A set is the same trie a map is, over elements that are keys and nothing
# else, so it gains a level roughly every 32 elements.

(deftest set-with-no-elements
  (is (= 0 (length (persimmon/set))))
  (is (== @[] (persimmon/to-array (persimmon/set)))))

(deftest set-with-one-element
  (def s (persimmon/set :a))
  (is (= 1 (length s)))
  (is (= true (persimmon/has-key? s :a)))
  (is (= false (persimmon/has-key? s :b))))

(deftest set-collapses-duplicates
  (def s (persimmon/set :a :b :a :b :a))
  (is (= 2 (length s)))
  (is (== @[:a :b] (elements s))))

(deftest set-with-elements-across-multiple-levels
  (def expect (numbers 1100))
  (def s (persimmon/set ;expect))
  (is (= 1100 (length s)))
  (is (== expect (elements s))))

(deftest set-refuses-a-nil-element
  (is (thrown? (persimmon/set nil)))
  (is (thrown? (persimmon/conj (persimmon/set) nil))))

(deftest into-a-set-from-a-collection
  (is (== @[:a :b] (elements (persimmon/into (persimmon/set) @[:a :b]))))
  (is (== @[:a :b] (elements (persimmon/into (persimmon/set) (persimmon/vec :a :b))))))

(deftest into-a-set-unions-with-what-it-already-holds
  (def s1 (persimmon/set :a))
  (def s2 (persimmon/into s1 @[:b :a]))
  (is (== @[:a :b] (elements s2)))
  (is (== @[:a] (elements s1))))

(deftest into-a-set-refuses-a-nil-element
  (is (thrown? (persimmon/into (persimmon/set) @[nil]))))

# A constructor argument is an element, never something to read through, so a
# collection passed whole is one element rather than an error.
(deftest set-holds-a-collection-as-a-single-element
  (def s (persimmon/set [:a 1] @{:b 2}))
  (is (= 2 (length s)))
  (is (= true (persimmon/has-key? s [:a 1]))))

# `length` reads the abstract type's length slot while `:length` goes through
# the method table. Both are supported and must agree.
(deftest length-of-a-set
  (is (= 0 (length (persimmon/set))))
  (is (= 0 (:length (persimmon/set))))
  (def s (persimmon/set ;(numbers 1100)))
  (is (= 1100 (length s)))
  (is (= 1100 (:length s))))

# As in Clojure, looking an element up in a set answers with the element.
(deftest get-with-a-set
  (def s (persimmon/set :a :b))
  (is (= :a (get s :a)))
  (is (= nil (get s :c)))
  (is (= nil (get s nil)))
  (is (= :fallback (get s :c :fallback))))

(deftest get-prefers-an-element-over-a-method
  (is (= :length (get (persimmon/set :length) :length)))
  (is (= 1 (:length (persimmon/set :a)))))

# A set answers with the element it holds, so calling one is a membership test
# that reads as a predicate wherever a function is wanted.
(deftest calling-a-set
  (def s (persimmon/set :a :b))
  (is (= :a (s :a)))
  (is (= nil (s :c)))
  (is (= nil (s nil)))
  (is (= false (s :c false)))
  (is (= :a (s :a false)))
  (is (== @[:a :b] (filter s [:a :c :b]))))

(deftest calling-a-set-with-the-wrong-number-of-arguments
  (def s (persimmon/set :a))
  (is (thrown? (s)))
  (is (thrown? (s :a false :extra))))

(deftest conj-with-a-new-element
  (def s1 (persimmon/set :a))
  (def s2 (persimmon/conj s1 :b))
  (is (= 1 (length s1)))
  (is (= 2 (length s2)))
  (is (= false (persimmon/has-key? s1 :b)))
  (is (= true (persimmon/has-key? s2 :b))))

(deftest conj-with-an-existing-element
  (def s1 (persimmon/set :a :b))
  (def s2 (persimmon/conj s1 :a))
  (is (= 2 (length s2)))
  (is (== @[:a :b] (elements s2))))

(deftest conj-across-multiple-levels
  (var s (persimmon/set))
  (for i 0 1100
    (set s (persimmon/conj s i)))
  (is (= 1100 (length s)))
  (is (== (numbers 1100) (elements s))))

(deftest disj-with-an-existing-element
  (def s1 (persimmon/set :a :b))
  (def s2 (persimmon/disj s1 :a))
  (is (= 2 (length s1)))
  (is (= 1 (length s2)))
  (is (= true (persimmon/has-key? s1 :a)))
  (is (= false (persimmon/has-key? s2 :a))))

(deftest disj-with-a-missing-element
  (def s1 (persimmon/set :a))
  (def s2 (persimmon/disj s1 :b))
  (is (= 1 (length s2)))
  (is (= true (persimmon/has-key? s2 :a))))

(deftest disj-back-to-empty
  (var s (persimmon/set ;(numbers 1100)))
  (for i 0 1100
    (set s (persimmon/disj s i)))
  (is (= 0 (length s)))
  (is (== @[] (persimmon/to-array s))))

(deftest disj-leaves-the-remaining-elements-reachable
  (var s (persimmon/set ;(numbers 1100)))
  (for i 0 1100
    (when (even? i) (set s (persimmon/disj s i))))
  (is (= 550 (length s)))
  (for i 0 1100
    (is (= (odd? i) (persimmon/has-key? s i)))))

(deftest next-with-an-empty-set
  (is (= nil (next (persimmon/set)))))

(deftest next-with-a-missing-element
  (is (= nil (next (persimmon/set :a) :absent))))

(deftest iterating-a-set
  (def s (persimmon/set :a :b :c))
  (is (== @[:a :b :c] (sorted (keys s))))
  (is (== @[:a :b :c] (sorted (values s))))
  (is (== @[:a :b :c] (sorted (seq [v :in s] v)))))

(deftest iterating-a-set-across-multiple-levels
  (def s (persimmon/set ;(numbers 1100)))
  (is (== (numbers 1100) (sorted (seq [v :in s] v)))))

(deftest iteration-order-does-not-depend-on-how-a-set-was-built
  (def direct (persimmon/set ;(numbers 100)))
  (var grown (persimmon/set))
  (for i 0 100
    (set grown (persimmon/conj grown i)))
  (var pruned (persimmon/set ;(numbers 200)))
  (for i 100 200
    (set pruned (persimmon/disj pruned i)))
  (is (== (persimmon/to-array direct) (persimmon/to-array grown)))
  (is (== (persimmon/to-array direct) (persimmon/to-array pruned))))

(deftest stringifying-a-set
  (is (= "#{}" (string (persimmon/set))))
  (is (= "#{a}" (string (persimmon/set :a)))))

(deftest hashing-with-equivalent-sets
  (def h1 (hash (persimmon/set :a :b)))
  (def h2 (hash (persimmon/set :b :a)))
  (is (= h1 h2)))

(deftest hashing-does-not-depend-on-how-a-set-was-built
  (def direct (persimmon/set ;(numbers 100)))
  (var grown (persimmon/set))
  (for i 0 100
    (set grown (persimmon/conj grown i)))
  (is (= (hash direct) (hash grown))))

(deftest hashing-with-different-sets
  (def h1 (hash (persimmon/set :a :b)))
  (def h2 (hash (persimmon/set :a :c)))
  (is (not (= h1 h2))))

(deftest elements-of-every-type
  (def s (persimmon/set :kw "str" 'sym 42 true [1 2]))
  (is (= 6 (length s)))
  (is (= true (persimmon/has-key? s :kw)))
  (is (= true (persimmon/has-key? s "str")))
  (is (= true (persimmon/has-key? s 'sym)))
  (is (= true (persimmon/has-key? s 42)))
  (is (= true (persimmon/has-key? s true)))
  (is (= true (persimmon/has-key? s [1 2]))))

# The collector reaches a set's elements only through its mark callback, so
# anything it misses is freed while the set still holds it.
(deftest a-set-survives-a-collection
  (def s (persimmon/set ;(numbers 1100)))
  (gccollect)
  (is (= 1100 (length s)))
  (is (== (numbers 1100) (elements s))))

(deftest a-shared-set-survives-its-original
  (var s1 (persimmon/set ;(numbers 1100)))
  (def s2 (persimmon/conj s1 2000))
  (set s1 nil)
  (gccollect)
  (is (= 1101 (length s2)))
  (is (= true (persimmon/has-key? s2 1099))))

(deftest comparing-equivalent-sets
  (is (= (persimmon/set) (persimmon/set)))
  (is (= (persimmon/set :a :b) (persimmon/set :b :a)))
  (is (deep= (persimmon/set :a) (persimmon/set :a))))

(deftest comparing-sets-built-differently
  (def direct (persimmon/set ;(numbers 100)))
  (var grown (persimmon/set))
  (for i 0 100
    (set grown (persimmon/conj grown i)))
  (var pruned (persimmon/set ;(numbers 200)))
  (for i 100 200
    (set pruned (persimmon/disj pruned i)))
  (is (= direct grown))
  (is (= direct pruned)))

(deftest comparing-different-sets
  (is (not (= (persimmon/set :a :b) (persimmon/set :a :c))))
  (is (not (= (persimmon/set :a) (persimmon/set :a :b)))))

(deftest comparing-sets-across-multiple-levels
  (def set1 (persimmon/set ;(numbers 1100)))
  (is (= set1 (persimmon/set ;(numbers 1100))))
  (is (not (= set1 (persimmon/disj set1 1099)))))

(deftest comparing-a-set-with-another-kind-of-structure
  (is (not (= (persimmon/set) (persimmon/map))))
  (is (not (= (persimmon/set) (persimmon/list)))))

(deftest a-set-as-a-table-key
  (def t @{})
  (put t (persimmon/set :a) :found)
  (is (= :found (get t (persimmon/set :a))))
  (is (= nil (get t (persimmon/set :b)))))

(deftest marshalling-a-set
  (def set1 (persimmon/set :a :b))
  (def set2 (unmarshal (marshal set1)))
  (is (= set1 set2))
  (is (= true (persimmon/has-key? set2 :a)))
  (is (== @[:a :b] (elements set2))))

(deftest marshalling-an-empty-set
  (is (= (persimmon/set) (unmarshal (marshal (persimmon/set)))))
  (is (= 0 (length (unmarshal (marshal (persimmon/set)))))))

(deftest marshalling-a-set-across-multiple-levels
  (def set1 (persimmon/set ;(numbers 1100)))
  (def set2 (unmarshal (marshal set1)))
  (is (= set1 set2))
  (is (= (hash set1) (hash set2)))
  (is (== (persimmon/to-array set1) (persimmon/to-array set2))))

(deftest a-set-read-back-survives-a-collection
  (def s (unmarshal (marshal (persimmon/set ;(numbers 1100)))))
  (gccollect)
  (is (= 1100 (length s)))
  (is (== (numbers 1100) (elements s))))

(run-tests!)
