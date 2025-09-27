# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/piers/pico-sdk/tools/pioasm"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pioasm"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pioasm-install"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/tmp"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src"
  "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/piers/Documents/Pico-v1.5.1/testing/build/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
