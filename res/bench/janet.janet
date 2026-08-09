(import ../../_build/release/persimmon :as persimmon)

# These runs include the Janet abstract allocations that the C benchmark
# deliberately leaves out.

(defn- report [name operations thunk]
  (gccollect)
  (def before (os/clock))
  (def result (thunk))
  (def elapsed (- (os/clock) before))
  (def ns-per-op (/ (* elapsed 1000000000) operations))
  (printf "%-30s %10d ops  %9.2f ns/op" name operations ns-per-op)
  result)

(def vector-count 150000)

(def persistent-vector
  (report "vector push (persistent)" vector-count
    (fn []
      (var vector (persimmon/vec))
      (for i 0 vector-count
        (set vector (persimmon/conj vector i)))
      vector)))

(def transient-vector
  (report "vector push (transient)" vector-count
    (fn []
      (def transient (persimmon/transient (persimmon/vec)))
      (for i 0 vector-count
        (persimmon/conj! transient i))
      (persimmon/persistent! transient))))

(def map-count 50000)

(def persistent-map
  (report "map assoc (persistent)" map-count
    (fn []
      (var map (persimmon/map))
      (for i 0 map-count
        (set map (persimmon/assoc map i (* i 3))))
      map)))

(def transient-map
  (report "map assoc (transient)" map-count
    (fn []
      (def transient (persimmon/transient (persimmon/map)))
      (for i 0 map-count
        (persimmon/assoc! transient i (* i 3)))
      (persimmon/persistent! transient))))

(def set-count 50000)

(def persistent-set
  (report "set conj (persistent)" set-count
    (fn []
      (var set (persimmon/set))
      (for i 0 set-count
        (set set (persimmon/conj set i)))
      set)))

(def transient-set
  (report "set conj (transient)" set-count
    (fn []
      (def transient (persimmon/transient (persimmon/set)))
      (for i 0 set-count
        (persimmon/conj! transient i))
      (persimmon/persistent! transient))))

(print "checksum: "
       (+ (length persistent-vector)
          (length transient-vector)
          (length persistent-map)
          (length transient-map)
          (length persistent-set)
          (length transient-set)))
