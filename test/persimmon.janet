(import testament :prefix "" :exit true)
(import ../build/persimmon :as persimmon)


(deftest vec-with-no-items
  (is (= 0 (length (persimmon/vec)))))


(deftest vec-with-one-item
  (def vec (persimmon/vec [:a]))
  (is (= 1 (length vec)))
  (is (= :a (get vec 0)))
  (is (= nil (get vec 1))))


(deftest conj-with-vector
  (def vec1 (persimmon/vec [:foo :bar]))
  (def vec2 (persimmon/conj vec1 :qux))
  (is (= :foo (get vec1 0)))
  (is (= :qux (get vec2 2))))


(run-tests!)
