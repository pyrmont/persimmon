# Build files are vendored from Spork by `jeep prep build`, which copies
# them from Jeep's own deps, patches included.
(import ./spork/build-rules :as br)
(import ./spork/cc :as cc)
(import ./spork/declare-cc :as declare)

(def- build-dir "_build")
(def- seps {:windows "\\" :mingw "\\" :cygwin "\\"})
(def- s (get seps (os/which) "/"))

# Native targets are written to <build-dir>/<build-type>. Derive the path
# from the same value the build uses so build and install cannot drift.
(def- build-type :release)
(def- native-dir (string build-dir s build-type))

(def- windows? (not (nil? (index-of (os/which) [:windows :mingw :cygwin]))))

# declare-cc picks artifact extensions from the toolchain, defaulting to
# MSVC on Windows and a GCC-like compiler elsewhere. Mirror that here.
(def- msvc? (= :windows (os/which)))
(def- lib-ext (if msvc? ".dll" ".so"))
(def- ar-ext (if msvc? ".lib" ".a"))

(defn- platform-flags
  ``Flatten a platform-keyed flag table into a flag array. Flags under
    :common apply everywhere; :windows and :posix are selected by host.``
  [flags]
  (array/concat @[]
                (get flags :common [])
                (get flags (if windows? :windows :posix) [])))

# used for splitting POSIX paths
(def- posix-pathg ~{:main     (* (+ :abspath :relpath) (? :sep) -1)
                    :abspath  (* :root (any :relpath))
                    :relpath  (* :part (any (* :sep :part)))
                    :root     (* "/" (constant ""))
                    :sep      (some "/")
                    :part     '(some (* (! :sep) 1))})

(defn- split-posix-path [path]
  (peg/match posix-pathg path))

(defn- add-path [paths dest &opt src]
  (def bits (split-posix-path dest))
  (assert bits "invalid path")
  (def ks @[])
  (each b bits
    (array/push ks b)
    (unless (get-in paths ks)
      (put-in paths ks @{})))
  (when src
    (put-in paths ks (-> (split-posix-path src)
                         (string/join s)))))

(defn- build-exes [manifest &]
  (def exes (get-in manifest [:info :artifacts :executables] []))
  (unless (empty? exes)
    (print "building native executables is not implemented"))
  (each exe exes
    (cond
      # uncomment when building quickbins
      # (get exe :quickbin?)
      # (declare/quickbin (get exe :entry) (string build-dir s (get exe :name)))
      # default
      nil)))

(defn- build-nats [manifest &]
  (def nats (get-in manifest [:info :artifacts :natives] []))
  (unless (empty? nats)
    (def rules @{})
    (with-dyns [cc/*rules* rules
                cc/*build-type* build-type
                declare/*build-root* build-dir]
      (declare/declare-project :name (get-in manifest [:info :name]))
      # declare-project registers a pre-build hook that creates these, but
      # create them here too so the build does not depend on hook ordering.
      (os/mkdir build-dir)
      (os/mkdir native-dir)
      (os/mkdir (string native-dir s "static"))
      (each nat nats
        (declare/declare-native
          :name (get nat :name)
          :source (get nat :files)
          :embedded (get nat :embedded)
          :cflags (platform-flags (get nat :cflags {}))
          :lflags (platform-flags (get nat :lflags {}))))
      (br/build-run rules "build"))))

(defn build [manifest &]
  (os/mkdir build-dir)
  (build-nats manifest)
  (build-exes manifest))

(defn- install-exes [manifest &]
  (def exes (get-in manifest [:info :artifacts :executables] []))
  (each exe exes
    (bundle/add-bin manifest (string build-dir s (get exe :name)))))

(defn- install-libs [manifest &]
  (def to-make @{})
  (def libs (get-in manifest [:info :artifacts :libraries] []))
  (each lib libs
    (def ks @[])
    (def prefix (get lib :prefix))
    (when prefix (add-path to-make prefix))
    (def paths (get lib :paths))
    (each p paths
      # use POSIX path separator to match info file
      (add-path to-make (string (when prefix (string prefix "/")) p) p)))
  (defn add-tree [tree path]
    (eachp [k v] tree
      (if (table? v)
        (do
          (def new-path (if (empty? path) k (string path s k)))
          (bundle/add-directory manifest new-path)
          (add-tree v new-path))
        (bundle/add manifest v (string path s k)))))
  (add-tree to-make ""))

(defn- install-mans [manifest &]
  (def mans (get-in manifest [:info :artifacts :manpages] []))
  (each m mans
    (def bits (split-posix-path m))
    (var dir (dyn :syspath))
    (each b (array/slice bits 0 -2)
      (set dir (string dir s b))
      (os/mkdir dir))
    (bundle/add-file manifest (string/join bits s))))

(defn- install-nats [manifest &]
  (def nats (get-in manifest [:info :artifacts :natives] []))
  (each nat nats
    (def prefix (get nat :prefix))
    (when prefix (bundle/add-directory manifest prefix))
    # The shared library is what `import` loads; the archive and meta file
    # are what `jeep quickbin` needs to link the module statically.
    (each ext [ar-ext ".meta.janet" lib-ext]
      (def filename (string (get nat :name) ext))
      (def src (string native-dir s filename))
      (def dest (if prefix (string prefix s filename) filename))
      (bundle/add-file manifest src dest))))

# based on code from spork/declare-cc.janet
(defn- add-bat-shim [manifest bin-name &opt chmod-mode]
  (def binpath (string (dyn :syspath) s "bin"))
  (def bin-dest (string binpath s bin-name))
  (assert (= :file (os/stat bin-dest :mode)) "must call bundle/add-bin first")
  (def bat-name (string bin-name ".bat"))
  (def files (get manifest :files)) # guaranteed to be non-nil
  (def dest (string binpath s bat-name))
  (when (os/stat dest :mode)
    (errorf "collision at %s, file already exists" dest))
  (def bat (string "@echo off\r\n"
                   "goto #_undefined_# 2>NUL || title %COMSPEC% & janet \""
                   bin-dest
                   "\" %*"))
  (spit dest bat)
  (def absdest (os/realpath dest))
  (array/push files absdest)
  (when chmod-mode
    (os/chmod absdest chmod-mode))
  (print "add " absdest))

(defn- install-scrs [manifest &]
  (def scrs (get-in manifest [:info :artifacts :scripts] []))
  (each scr scrs
    (def path (-> (get scr :path)
                  (split-posix-path)
                  (string/join s)))
    (bundle/add-bin manifest path)
    (when (= "\\" s)
      (def bin-name (last (string/split s path)))
      (add-bat-shim manifest bin-name))))

(defn- set-version [manifest]
  (def bundle-ver (get-in manifest [:info :version]))
  (if (not= "DEVEL" bundle-ver)
    (put manifest :version bundle-ver)
    (do
      # An unreleased bundle records the commit it was installed from.
      (def src (get manifest :local-source))
      (def [r w] (os/pipe))
      (def [ok? _] (protect (os/execute ["git" "-C" src "describe" "--always" "--dirty"]
                                        :px {:out w :err w})))
      (:close w)
      (put manifest
           :version
           (if ok?
             (string bundle-ver "-" (string/trim (ev/read r :all)))
             bundle-ver)))))

(defn install [manifest &]
  (install-exes manifest)
  (install-libs manifest)
  (install-mans manifest)
  (install-nats manifest)
  (install-scrs manifest)
  (set-version manifest))
