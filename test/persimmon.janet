(import testament :prefix "" :exit true)
(import ../build/persimmon :as persimmon)


(deftest vec-with-no-items
  (is (= 0 (length (persimmon/vec)))))


(deftest vec-with-one-item
  (def vec (persimmon/vec [:a]))
  (is (= 1 (length vec)))
  (is (= :a (get vec 0)))
  (is (= nil (get vec 1))))


(deftest conj-with-vector-with-space-in-tail
  (def vec1 (persimmon/vec [:foo :bar]))
  (def vec2 (persimmon/conj vec1 :qux))
  (is (= :foo (get vec1 0)))
  (is (= :qux (get vec2 2))))


(deftest conj-with-vector-with-no-space-in-tail
  (def numbers @[0  1  2  3  4  5  6  7  8  9
                 10 11 12 13 14 15 16 17 18 19
                 20 21 22 23 24 25 26 27 28 29
                 30 31])
  (def vec1 (persimmon/vec numbers))
  (def vec2 (persimmon/conj vec1 32))
  (def expect (array/concat @[] ;numbers 32))
  (def actual (persimmon/to-array vec2))
  (is (== expect actual)))


(deftest next-with-empty-vector
  (def vec (persimmon/vec))
  (is (= nil (next vec))))


(deftest next-with-non-empty-vector
  (def vec (persimmon/vec [:foo :bar]))
  (is (= 0   (next vec)))
  (is (= 1   (next vec 0)))
  (is (= nil (next vec 1))))


(deftest assoc-with-valid-index
  (def vec1 (persimmon/vec [:foo :bar]))
  (def vec2 (persimmon/assoc vec1 0 :qux))
  (is (== [:foo :bar] (persimmon/to-array vec1)))
  (is (== [:qux :bar] (persimmon/to-array vec2))))


(deftest hashing-with-equivalent-vectors
  (def h1 (hash (persimmon/vec [:foo :bar])))
  (def h2 (hash (persimmon/vec [:foo :bar])))
  (is (= h1 h2)))


(deftest hashing-with-different-vectors
  (def h1 (hash (persimmon/vec [:foo :bar])))
  (def h2 (hash (persimmon/vec [:bar :foo])))
  (is (not (= h1 h2))))


(run-tests!)
