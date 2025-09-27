# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-src"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-build"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/tmp"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/src"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
