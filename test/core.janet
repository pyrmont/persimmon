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

(defn- run
  ``Runs cmd with its output captured rather than let loose on the terminal.
    Answers with the exit status and everything the command wrote, error
    output folded in where it was written so that a diagnostic still sits
    beside the line it belongs to.``
  [cmd]
  (def proc (os/spawn cmd :p {:out :pipe :err :out}))
  # Reading to the end of the stream before waiting keeps a command whose
  # output outgrows the pipe buffer from blocking on a reader that is itself
  # blocked waiting for the command to exit.
  (def out (ev/read (proc :out) :all))
  # Windows hands a command a stdout in text mode, so what arrives here ends
  # its lines with a carriage return and a newline. Settling that once, where
  # the bytes come in, keeps it out of everything downstream: a caller
  # matching against the output needs no second spelling of each line, and one
  # echoed back to a terminal is not translated a second time on the way out.
  [(os/proc-wait proc) (if out (string/replace-all "\r\n" "\n" (string out)) "")])

(defn- report
  ``Puts a command's output on the terminal verbatim, since a compiler lines
    its diagnostics up against the source and indenting them here would only
    move the carets away from what they point at.``
  [output]
  (unless (empty? output)
    (prin output)
    (unless (string/has-suffix? "\n" output) (print))))

(defn- runnable?
  [cmd]
  (def [ok? status] (protect (first (run cmd))))
  (and ok? (zero? status)))

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
  (run cmd))

(defn- amalgamate
  [mk]
  (run [mk "-C" root "amalgamation"]))

(defn- compile-amalgam
  ``Builds the same checks against the amalgamation instead of the separate
    sources. The sanitisers are left off: the code is the same either way, so
    the core build above already covers it and running them twice only makes a
    slow pre-release run slower. What is under test here is that merging the
    sources into one translation unit still compiles and still behaves.``
  [cc]
  (run [cc "-std=c99" "-Wall" "-Wextra" "-DPERSIMM_TEST_ALLOC" "-O1"
        "-I" amal-dir "-o" amal-exe
        (path "test" "core.c")
        (string amal-dir "/persimmon.c")]))

(defn- checks-passed
  ``Answers with what the checks found about atomic reference counts, or nil
    if the output was not what a passing run writes. A run that holds prints
    the two lines below and nothing else, so matching the whole of it catches
    a binary that fell over partway through and still managed to exit zero,
    which counting the failures it announced would not.``
  [output]
  (def lines (string/split "\n" (string/trimr output)))
  (when (and (= 2 (length lines))
             (= "core checks passed" (first lines)))
    (first (or (peg/match ~(* "atomic reference counts: " '(+ "yes" "no") -1)
                          (in lines 1))
               []))))

(defn- checked?
  ``Runs a built check program, swallowing its output when that output is
    what a passing run writes and putting it on the terminal otherwise, so a
    failing run still says which checks failed and why. The answer about
    atomics is reported either way: it is a property of the build rather than
    of the code, and the run is the only place it is known.``
  [prog]
  (def [status output] (run [prog]))
  (def atomics (checks-passed output))
  (if atomics
    (print "  the checks passed, with atomic reference counts: " atomics)
    (report output))
  (and (zero? status) (truthy? atomics)))

(deftest core-checks
  (if-let [cc (compiler)]
    (do
      (print "  building the core checks with " cc
             (if (os/getenv "PERSIMMON_SANITISE") " and the sanitisers" ""))
      (def [status output] (compile cc))
      # Reported whatever the status, since a warning the compiler was content
      # enough to exit zero on is still worth reading.
      (report output)
      (is (zero? status))
      (when (zero? status)
        (is (checked? exe))))
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
      # A recipe that runs says only which commands it ran, which is worth
      # reading when one of them failed and is noise when none did.
      (def [made made-output] (amalgamate mk))
      (unless (zero? made) (report made-output))
      (is (zero? made))
      (def [built build-output] (compile-amalgam cc))
      (report build-output)
      (is (zero? built))
      (when (and (zero? made) (zero? built))
        (is (checked? amal-exe))))))

(run-tests!)
