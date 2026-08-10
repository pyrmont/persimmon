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

(def- amal-dir (path "_build" "amalgam"))

(def- amal-exe (path "_build" (if (= :windows (os/which))
                                "persimmon-amalgam-test.exe"
                                "persimmon-amalgam-test")))

(defn- sources
  ``Reads the core's source files out of the info file, so that adding one to
    the build cannot leave it untested. Core sources live directly under src;
    host bindings below src/bind must not be linked in.``
  []
  (def info (-> (path "info.jdn") slurp parse))
  (def natives (get-in info [:artifacts :natives] []))
  (seq [nat :in natives
        file :in (get nat :files [])
        :when (and (string/has-prefix? "src/" file)
                   (= 1 (length (string/find-all "/" file))))]
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

(defn- maker
  ``Answers with the first make that can read the Makefile, or nil. Whatever
    MAKE names is tried first, the way compiler treats CC. The probe is a dry
    run of the target itself, so a make that cannot parse the Makefile counts
    as no make at all.``
  []
  (find |(runnable? [$ "-C" root "-n" "amalgamation"])
        (filter identity [(os/getenv "MAKE") "make" "gmake" "bmake"])))

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
             ["-I" (path "include") "-o" exe (path "test" "core.c")]
             (sources)))
  (os/execute cmd :p))

(defn- amalgamate
  [mk]
  (os/execute [mk "-C" root "amalgamation"] :p))

(defn- compile-amalgam
  ``Builds the same checks against the amalgamation instead of the separate
    sources. The sanitisers are left off: the code is the same either way, so
    the core build above already covers it and running them twice only makes a
    slow pre-release run slower. What is under test here is that merging the
    sources into one translation unit still compiles and still behaves.``
  [cc]
  (os/execute [cc "-std=c99" "-Wall" "-Wextra" "-DPERSIMM_TEST_ALLOC" "-O1"
               "-I" amal-dir "-o" amal-exe
               (path "test" "core.c")
               (string amal-dir "/persimmon.c")]
              :p))

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

(deftest amalgamation-checks
  (def cc (compiler))
  (def mk (unless (= :windows (os/which)) (maker)))
  (cond
    (= :windows (os/which))
    (do
      (print "  the amalgamation is written by a make recipe that wants a")
      (print "  posix shell, so it is not checked here")
      (is true))

    (nil? cc)
    (do
      (print "  no C compiler found, so the amalgamation checks did not run")
      (print "  set CC to name one")
      (is true))

    (nil? mk)
    (do
      (print "  no make found, so the amalgamation checks did not run")
      (print "  set MAKE to name one")
      (is true))

    (do
      (print "  building the amalgamation with " mk " and checking it with " cc)
      (flush)
      (is (zero? (amalgamate mk)))
      (flush)
      (is (zero? (compile-amalgam cc)))
      (flush)
      (is (zero? (os/execute [amal-exe] :p))))))

(run-tests!)
