(use ../deps/testament)
(import ../_build/release/persimmon :as persimmon)

(defn- numbers
  "Builds an array of the integers from 0 to `n`, exclusive"
  [n]
  (def result @[])
  (for i 0 n (array/push result i))
  result)

# A vector holds up to 32 items in its tail before any of them reach the trie,
# and the trie gains a level at 32 and again at 1056 items. Sizes either side
# of those boundaries are what the tests below reach for.

(deftest vec-with-no-items
  (is (= 0 (length (persimmon/vec)))))

(deftest vec-with-one-item
  (def vec (persimmon/vec :a))
  (is (= 1 (length vec)))
  (is (= :a (get vec 0)))
  (is (= nil (get vec 1))))

(deftest vec-with-items-across-multiple-levels
  (def expect (numbers 1100))
  (def vec (persimmon/into (persimmon/vec) expect))
  (is (= 1100 (length vec)))
  (is (= 0 (get vec 0)))
  (is (= 31 (get vec 31)))
  (is (= 32 (get vec 32)))
  (is (= 1055 (get vec 1055)))
  (is (= 1099 (get vec 1099)))
  (is (== expect (persimmon/to-array vec))))

# A constructor argument is an element, never something to read through, so a
# collection passed whole is one element rather than its contents.
(deftest vec-holds-a-collection-as-a-single-element
  (def vec (persimmon/vec [1 2 3]))
  (is (= 1 (length vec)))
  (is (== [1 2 3] (get vec 0))))

(deftest vec-from-a-spliced-collection
  (is (== @[1 2 3] (persimmon/to-array (persimmon/vec ;[1 2 3])))))

(deftest into-a-vector-from-an-indexed-collection
  (def vec (persimmon/into (persimmon/vec) @[:a :b]))
  (is (= 2 (length vec)))
  (is (= :a (get vec 0)))
  (is (= :b (get vec 1)))
  (is (== @[:a] (persimmon/to-array (persimmon/into (persimmon/vec) [:a])))))

(deftest into-a-vector-appends-to-what-it-already-holds
  (def vec1 (persimmon/vec 0))
  (def vec2 (persimmon/into vec1 @[1 2]))
  (is (== @[0 1 2] (persimmon/to-array vec2)))
  (is (== @[0] (persimmon/to-array vec1))))

(deftest into-a-vector-from-an-empty-collection
  (is (= 0 (length (persimmon/into (persimmon/vec) @[]))))
  (is (== @[1] (persimmon/to-array (persimmon/into (persimmon/vec 1) @[])))))

(deftest into-a-vector-from-a-persistent-collection
  (is (== @[1 2] (persimmon/to-array (persimmon/into (persimmon/vec) (persimmon/list 1 2)))))
  (is (== @[:a] (persimmon/to-array (persimmon/into (persimmon/vec) (persimmon/set :a))))))

# A dictionary reads as its entries wherever elements are wanted, which is how
# `to-array` hands a map over too.
(deftest into-a-vector-from-a-dictionary
  (is (== @[[:a 1]] (persimmon/to-array (persimmon/into (persimmon/vec) @{:a 1}))))
  (is (== @[[:a 1]] (persimmon/to-array (persimmon/into (persimmon/vec)
                                                        (persimmon/map :a 1))))))

(deftest into-refuses-a-source-it-cannot-read
  (is (thrown? (persimmon/into (persimmon/vec) :a)))
  (is (thrown? (persimmon/into (persimmon/vec) 5))))

(deftest into-refuses-a-target-that-is-not-persistent
  (is (thrown? (persimmon/into @[] @[1])))
  (is (thrown? (persimmon/into (persimmon/transient (persimmon/vec)) @[1]))))

# `length` reads the abstract type's length slot while `:length` goes through
# the method table. Both are supported and must agree.
(deftest length-of-a-vector
  (is (= 0 (length (persimmon/vec))))
  (is (= 0 (:length (persimmon/vec))))
  (def vec (persimmon/into (persimmon/vec) (numbers 1100)))
  (is (= 1100 (length vec)))
  (is (= 1100 (:length vec))))

(deftest get-with-negative-index
  (def vec (persimmon/vec :foo :bar :qux))
  (is (= :qux (get vec -1)))
  (is (= :foo (get vec -3)))
  (is (= nil (get vec -4)))
  (is (= nil (get vec -2147483648))))

(deftest get-with-out-of-bounds-index
  (def vec (persimmon/vec :foo :bar))
  (is (= nil (get vec 2)))
  (is (= nil (get vec 2147483647))))

# A structure in the operator position answers as in does, so an index outside
# a vector is a bad key there and raises, where get answers nil.
(deftest calling-a-vector
  (def vec (persimmon/vec :foo :bar))
  (is (= :foo (vec 0)))
  (is (= :bar (vec -1)))
  (is (thrown? (vec 2)))
  (is (= nil (get vec 2)))
  (is (== @[:foo :bar] (map vec [0 1]))))

# A bad key raises the one message Janet gives for its own indexed types,
# whatever is wrong with the key, naming the range this vector accepts.
(deftest calling-a-vector-with-a-bad-key
  (def vec (persimmon/vec :foo :bar))
  (defn message [key]
    (def [ok err] (protect (vec key)))
    (is (= false ok))
    (string err))
  (def expect "expected integer key for persimmon/vector in range [-2, 2), got ")
  (is (= (string expect "2") (message 2)))
  (is (= (string expect "-3") (message -3)))
  (is (= (string expect "nil") (message nil)))
  (is (= (string expect "0.5") (message 0.5)))
  (is (= (string expect ":bogus") (message :bogus))))

# Janet's own structures take exactly one argument in that position.
(deftest calling-a-vector-with-the-wrong-number-of-arguments
  (def vec (persimmon/vec :foo))
  (is (thrown? (vec)))
  (is (thrown? (vec 0 :fallback))))

(deftest conj-with-vector-with-space-in-tail
  (def vec1 (persimmon/vec :foo :bar))
  (def vec2 (persimmon/conj vec1 :qux))
  (is (= :foo (get vec1 0)))
  (is (= :qux (get vec2 2))))

(deftest conj-with-vector-with-no-space-in-tail
  (def nums (numbers 32))
  (def vec1 (persimmon/into (persimmon/vec) nums))
  (def vec2 (persimmon/conj vec1 32))
  (def expect (array/concat @[] ;nums 32))
  (def actual (persimmon/to-array vec2))
  (is (== expect actual)))

(deftest conj-across-multiple-levels
  (var vec (persimmon/vec))
  (for i 0 1100
    (set vec (persimmon/conj vec i)))
  (is (= 1100 (length vec)))
  (is (== (numbers 1100) (persimmon/to-array vec))))

(deftest conj-does-not-modify-original
  (def vec1 (persimmon/into (persimmon/vec) (numbers 1000)))
  (def vec2 (persimmon/conj vec1 1000))
  (is (= 1000 (length vec1)))
  (is (= 1001 (length vec2)))
  (is (= nil (get vec1 1000)))
  (is (= 1000 (get vec2 1000))))

(deftest next-with-empty-vector
  (def vec (persimmon/vec))
  (is (= nil (next vec))))

(deftest next-with-non-empty-vector
  (def vec (persimmon/vec :foo :bar))
  (is (= 0   (next vec)))
  (is (= 1   (next vec 0)))
  (is (= nil (next vec 1))))

(deftest assoc-with-valid-index
  (def vec1 (persimmon/vec :foo :bar))
  (def vec2 (persimmon/assoc vec1 0 :qux))
  (is (== [:foo :bar] (persimmon/to-array vec1)))
  (is (== [:qux :bar] (persimmon/to-array vec2))))

(deftest assoc-with-index-in-tail
  (def vec1 (persimmon/into (persimmon/vec) (numbers 40)))
  (def vec2 (persimmon/assoc vec1 35 :qux))
  (is (= 35 (get vec1 35)))
  (is (= :qux (get vec2 35)))
  (is (= 34 (get vec2 34)))
  (is (= 36 (get vec2 36)))
  (is (= 40 (length vec2))))

(deftest assoc-with-index-in-trie
  (def vec1 (persimmon/into (persimmon/vec) (numbers 1100)))
  (def vec2 (persimmon/assoc vec1 5 :qux))
  (def vec3 (persimmon/assoc vec1 1055 :quux))
  (is (= 5 (get vec1 5)))
  (is (= 1055 (get vec1 1055)))
  (is (= :qux (get vec2 5)))
  (is (= 1055 (get vec2 1055)))
  (is (= 5 (get vec3 5)))
  (is (= :quux (get vec3 1055))))

(deftest assoc-with-negative-index
  (def vec1 (persimmon/vec :foo :bar))
  (def vec2 (persimmon/assoc vec1 -1 :qux))
  (is (== [:foo :qux] (persimmon/to-array vec2))))

(deftest assoc-with-out-of-bounds-index
  (def vec (persimmon/vec :foo :bar))
  (is (thrown? (persimmon/assoc vec 2 :qux))))

(deftest stringifying-a-vector
  (is (= "[]" (string (persimmon/vec))))
  (is (= "[foo bar]" (string (persimmon/vec :foo :bar)))))

(deftest hashing-with-equivalent-vectors
  (def h1 (hash (persimmon/vec :foo :bar)))
  (def h2 (hash (persimmon/vec :foo :bar)))
  (is (= h1 h2)))

(deftest hashing-with-different-vectors
  (def h1 (hash (persimmon/vec :foo :bar)))
  (def h2 (hash (persimmon/vec :bar :foo)))
  (is (not (= h1 h2))))

# Janet has no separate equality slot for an abstract type, so `=`, `deep=`
# and use as a table key all arrive through the comparison method.
(deftest comparing-equivalent-vectors
  (is (= (persimmon/vec) (persimmon/vec)))
  (is (= (persimmon/vec :foo :bar) (persimmon/vec :foo :bar)))
  (is (deep= (persimmon/vec :foo :bar) (persimmon/vec :foo :bar))))

(deftest comparing-different-vectors
  (is (not (= (persimmon/vec :foo :bar) (persimmon/vec :bar :foo))))
  (is (not (= (persimmon/vec :foo) (persimmon/vec :foo :bar)))))

(deftest comparing-vectors-across-multiple-levels
  (def vec1 (persimmon/into (persimmon/vec) (numbers 1100)))
  (is (= vec1 (persimmon/into (persimmon/vec) (numbers 1100))))
  (is (not (= vec1 (persimmon/assoc vec1 1099 :qux)))))

# A vector's elements carry an order, so a vector does too.
(deftest ordering-vectors
  (def vecs (sort @[(persimmon/vec 2) (persimmon/vec 1 9) (persimmon/vec 1)]))
  (is (== @[@[1] @[1 9] @[2]] (map |(persimmon/to-array $) vecs))))

(deftest comparing-a-vector-with-another-kind-of-structure
  (is (not (= (persimmon/vec 1) (persimmon/list 1))))
  (is (not (= (persimmon/vec) (persimmon/map))))
  (is (not (= (persimmon/vec) (persimmon/set)))))

(deftest a-vector-as-a-table-key
  (def t @{})
  (put t (persimmon/vec :foo) :found)
  (is (= :found (get t (persimmon/vec :foo))))
  (is (= nil (get t (persimmon/vec :bar)))))

(deftest comparing-nested-structures
  (is (= (persimmon/vec (persimmon/vec 1)) (persimmon/vec (persimmon/vec 1))))
  (is (not (= (persimmon/vec (persimmon/vec 1)) (persimmon/vec (persimmon/vec 2))))))

(deftest marshalling-a-vector
  (def vec1 (persimmon/vec :foo :bar))
  (def vec2 (unmarshal (marshal vec1)))
  (is (= vec1 vec2))
  (is (= :bar (get vec2 1)))
  (is (== @[:foo :bar] (persimmon/to-array vec2))))

(deftest marshalling-an-empty-vector
  (is (= (persimmon/vec) (unmarshal (marshal (persimmon/vec)))))
  (is (= 0 (length (unmarshal (marshal (persimmon/vec)))))))

# Small integers marshal to a byte apiece, which leaves a vector of them with
# nothing to spare between its length and the end of the input.
(deftest marshalling-a-vector-of-small-integers
  (is (= (persimmon/vec 1) (unmarshal (marshal (persimmon/vec 1)))))
  (is (= (persimmon/vec 1 2 3) (unmarshal (marshal (persimmon/vec 1 2 3))))))

(deftest marshalling-a-vector-across-multiple-levels
  (def vec1 (persimmon/into (persimmon/vec) (numbers 1100)))
  (def vec2 (unmarshal (marshal vec1)))
  (is (= vec1 vec2))
  (is (== (numbers 1100) (persimmon/to-array vec2))))

(deftest marshalling-nested-structures
  (def nested (persimmon/vec (persimmon/map :a 1)
                             (persimmon/set :b)
                             (persimmon/list 1 2)))
  (is (= nested (unmarshal (marshal nested)))))

(run-tests!)
