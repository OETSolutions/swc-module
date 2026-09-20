cmake_minimum_required(VERSION 3.16.0)

# `lib/` must be registered as a component directory BEFORE project.cmake is
# included, because IDF resolves its component list at include time.
#
# IDF's default search is IDF_PATH/components plus ${CMAKE_SOURCE_DIR}/components.
# This project keeps its logic in `lib/` (the layout the plan and the host-test
# `build_src_filter` already assume), so `lib/` is not found by default -- and
# the failure mode is silent: the build SUCCEEDS with none of lib/ linked, which
# is what hid this for eleven tasks (see lib/CMakeLists.txt).
# APPEND, never assign: PlatformIO's IDF integration sets EXTRA_COMPONENT_DIRS
# itself to inject its own components, and `set()` clobbers that list -- which
# makes the build fail with "Couldn't find the main target of the project!"
# because the app component PlatformIO relies on is no longer discoverable.
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/lib)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(code)
