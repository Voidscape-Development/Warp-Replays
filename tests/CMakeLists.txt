# The headless render tests for the Warp overlay sources. Built on its own,
# against the libobs installed on the system, by tests/run-render-tests.sh; see
# tests/README.md.
cmake_minimum_required(VERSION 3.28)

project(warp-render-tests LANGUAGES C)

find_package(libobs REQUIRED)
find_package(X11 REQUIRED)

add_executable(warp-render-tests warp-render-tests.c)
target_link_libraries(warp-render-tests PRIVATE OBS::libobs X11::X11 m)
set_target_properties(warp-render-tests PROPERTIES C_STANDARD 11)
