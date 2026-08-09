(use ../deps/testament)
(import ../_build/release/persimmon :as persimmon)

(deftest vector-transient
  (def original (persimmon/vec 0 1 2))
  (def trans (persimmon/transient original))
  (persimmon/conj! trans 3)
  (persimmon/assoc! trans 1 :one)
  (is (= 4 (length trans)))
  (is (= :one (get trans 1)))
  (is (== @[0 :one 2 3] (seq [x :in trans] x)))
  (is (thrown? (hash trans)))
  (is (thrown? (marshal trans)))
  (def result (persimmon/persistent! trans))
  (is (== @[0 1 2] (persimmon/to-array original)))
  (is (== @[0 :one 2 3] (persimmon/to-array result)))
  (is (thrown? (length trans)))
  (is (thrown? (persimmon/conj! trans 4)))
  (is (thrown? (persimmon/persistent! trans))))

(deftest map-transient
  (def original (persimmon/map :a 1 :b 2))
  (def trans (persimmon/transient original))
  (persimmon/assoc! trans :a 10)
  (persimmon/assoc! trans :c 3)
  (persimmon/dissoc! trans :b)
  (is (= 2 (length trans)))
  (is (= 10 (get trans :a)))
  (is (= :missing (get trans :b :missing)))
  (is (== @[:a :c] (sorted (keys trans))))
  (def result (persimmon/persistent! trans))
  (is (== @{:a 1 :b 2} (persimmon/to-table original)))
  (is (== @{:a 10 :c 3} (persimmon/to-table result)))
  (is (thrown? (get trans :a)))
  (is (thrown? (persimmon/assoc! trans :d 4))))

(deftest assoc-nil-removes-from-map-transient
  (def trans (persimmon/transient (persimmon/map :a 1 :b 2)))
  (persimmon/assoc! trans :a nil)
  (is (== @{:b 2} (persimmon/to-table (persimmon/persistent! trans)))))

(deftest set-transient
  (def original (persimmon/set :a :b))
  (def trans (persimmon/transient original))
  (persimmon/conj! trans :c)
  (persimmon/disj! trans :a)
  (is (= 2 (length trans)))
  (is (= :c (get trans :c)))
  (is (== @[:b :c] (sorted (keys trans))))
  (def result (persimmon/persistent! trans))
  (is (== @[:a :b] (sorted (persimmon/to-array original))))
  (is (== @[:b :c] (sorted (persimmon/to-array result))))
  (is (thrown? (persimmon/disj! trans :b))))

(deftest only-editable-collections-become-transient
  (is (thrown? (persimmon/transient (persimmon/list 1 2)))))

(run-tests!)
