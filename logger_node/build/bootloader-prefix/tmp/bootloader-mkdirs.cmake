# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/miehirai/esp/v6.0.2/esp-idf/components/bootloader/subproject"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/tmp"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/src/bootloader-stamp"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/src"
  "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/miehirai/esp/stock_checker/logger_node/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
