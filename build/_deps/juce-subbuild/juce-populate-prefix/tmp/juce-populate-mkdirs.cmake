# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-src"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-build"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/tmp"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/src/juce-populate-stamp"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/src"
  "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/src/juce-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/src/juce-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "F:/PLSMStudio/Atomic DAW/build/_deps/juce-subbuild/juce-populate-prefix/src/juce-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
