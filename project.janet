(declare-project
  :name "Persimmon"
  :description "Persistent immutable data structures"
  :author "Michael Camilleri"
  :license "MIT"
  :url "https://github.com/pyrmont/persimmon"
  :repo "git+https://github.com/pyrmont/persimmon"
  :dependencies ["https://github.com/pyrmont/testament"])


(def cflags
  (case (os/which)
    :windows ["/Iutf8.h"]
    ["-Iutf8.h" "-std=c99" "-Wall" "-Wextra" "-O3"]))


(def lflags
  [])


(declare-native
  :name "persimmon"
  :cflags cflags
  :lflags lflags
  :headers ["src/persimmon.h"]
  :source ["src/persimmon.c"])
