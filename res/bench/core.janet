# Compiles and runs the host-independent C benchmark. Timing is deliberately
# kept out of the correctness suite: run this several times on an idle machine
# when comparing two revisions.

(def- root
  (let [file (dyn :current-file)
        cut (max (or (last (string/find-all "/" file)) -1)
                 (or (last (string/find-all "\\" file)) -1))]
    (string (if (neg? cut) "." (string/slice file 0 cut)) "/../..")))

(defn- path [& bits] (string root "/" (string/join bits "/")))

(def- exe (path "_build" (if (= :windows (os/which))
                           "persimmon-core-bench.exe"
                           "persimmon-core-bench")))

(defn- sources []
  (def info (-> (path "info.jdn") slurp parse))
  (def natives (get-in info [:artifacts :natives] []))
  (seq [nat :in natives
        file :in (get nat :files [])
        :when (and (string/has-prefix? "src/" file)
                   (= 1 (length (string/find-all "/" file))))]
    (path file)))

(defn- runnable? [cmd]
  (def devnull (os/open (if (= :windows (os/which)) "NUL" "/dev/null") :w))
  (defer (:close devnull)
    (def [ok? status] (protect (os/execute cmd :p {:out devnull :err devnull})))
    (and ok? (zero? status))))

(defn- compiler []
  (find |(runnable? [$ "--version"])
        (filter identity [(os/getenv "CC") "cc" "gcc" "clang"])))

(if-let [cc (compiler)]
  (do
    (os/mkdir (path "_build"))
    (def command
      (array/concat
        @[cc "-std=c99" "-Wall" "-Wextra" "-O3"
          "-I" (path "inc") "-o" exe (path "res" "bench" "core.c")]
        (sources)))
    (unless (zero? (os/execute command :p))
      (error "could not build the core benchmark"))
    (os/exit (os/execute [exe] :p)))
  (error "no C compiler found; set CC to name one"))
