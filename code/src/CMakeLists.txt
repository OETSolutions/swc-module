# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# `lib` is code/lib/ -- the pure-logic component (see ../lib/CMakeLists.txt).
# IDF names a component after its DIRECTORY, so the component registered by
# ../lib/CMakeLists.txt is `lib`, not `swc`; naming it `swc` here fails with
# "Failed to resolve component 'swc' required by component 'src': unknown name."
#
# It is named in REQUIRES because src/main.c includes its headers
# ("System/SystemOrchestrator.h"), and an IDF component only sees another
# component's INCLUDE_DIRS if it requires it. Omitting this is why the device
# build looked fine while linking none of lib/.
idf_component_register(SRCS ${app_sources}
                       REQUIRES lib)

