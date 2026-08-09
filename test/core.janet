(use ../deps/testament)

# The core is host-agnostic, so its own tests are written in C and live in
# test/core.c. This compiles them against the core alone, with no Janet header
# in sight, and runs what comes out. A link error here means something in the
# core has reached for the host.
#
# Set PERSIMMON_SANITISE to build with the address and undefined behaviour
# sanitisers, which is worth doing before a release and too slow for every run.

# The path this file was reached by may be relative or absolute, so the root
# is named as the test directory's parent rather than assembled from parts.
(def- root
  (let [file (dyn :current-file)
        cut (max (or (last (string/find-all "/" file)) -1)
                 (or (last (string/find-all "\\" file)) -1))]
    (string (if (neg? cut) "." (string/slice file 0 cut)) "/..")))

(defn- path [& bits] (string root "/" (string/join bits "/")))

(def- exe (path "_build" (if (= :windows (os/which))
                           "persimmon-core-test.exe"
                           "persimmon-core-test")))

(defn- sources
  ``Reads the core's source files out of the info file, so that adding one to
    the build cannot leave it untested. The core is what lives under src; a
    wrapper is written against a host language and must not be linked in.``
  []
  (def info (-> (path "info.jdn") slurp parse))
  (def natives (get-in info [:artifacts :natives] []))
  (seq [nat :in natives
        file :in (get nat :files [])
        :when (string/has-prefix? "src/" file)]
    (path file)))

(defn- runnable?
  [cmd]
  (def devnull (os/open (if (= :windows (os/which)) "NUL" "/dev/null") :w))
  (defer (:close devnull)
    (def [ok? status] (protect (os/execute cmd :p {:out devnull :err devnull})))
    (and ok? (zero? status))))

(defn- compiler
  ``Answers with the first C compiler that runs, or nil. Whatever CC names is
    tried first, so a caller can pick one without editing this.``
  []
  (find |(runnable? [$ "--version"])
        (filter identity [(os/getenv "CC") "cc" "gcc" "clang"])))

(defn- compile
  [cc]
  (os/mkdir (path "_build"))
  (def sanitise? (os/getenv "PERSIMMON_SANITISE"))
  (def cmd (array/concat
             @[cc "-std=c99" "-Wall" "-Wextra" "-DPERSIMM_TEST_ALLOC"]
             (if sanitise?
               # Without no-sanitize-recover the undefined behaviour sanitiser
               # reports and carries on, which would let a finding pass as green.
               ["-g" "-O1" "-fsanitize=address,undefined" "-fno-sanitize-recover=all"]
               ["-O1"])
             ["-I" (path "src") "-o" exe (path "test" "core.c")]
             (sources)))
  (os/execute cmd :p))

(deftest core-checks
  (if-let [cc (compiler)]
    (do
      (print "  building the core checks with " cc
             (if (os/getenv "PERSIMMON_SANITISE") " and the sanitisers" ""))
      # The compiler and the checks write straight to the file descriptor, so
      # anything Janet is still holding has to go out first to stay in order.
      (flush)
      (is (zero? (compile cc)))
      (flush)
      (is (zero? (os/execute [exe] :p))))
    (do
      (print "  no C compiler found, so the core checks did not run")
      (print "  set CC to name one")
      (is true))))

(run-tests!)
