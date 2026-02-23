# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-src")
  file(MAKE_DIRECTORY "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-src")
endif()
file(MAKE_DIRECTORY
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-build"
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix"
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/tmp"
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp"
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/src"
  "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/1.- Personal Proyects/Firmware Walkman/APP/Lyra/simulator/build2/_deps/lvgl-subbuild/lvgl-populate-prefix/src/lvgl-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
