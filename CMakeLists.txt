cmake_minimum_required(VERSION 3.0.0)

# Files
file(
    GLOB_RECURSE
    SOURCES
    "./*.h"
    "./*.cpp"
)

# Target 
add_executable(sticky-mouse-trap ${SOURCES})
target_include_directories(sticky-mouse-trap PUBLIC "./dependencies/MUtilize")
target_link_libraries(sticky-mouse-trap PUBLIC "X11" "Xi" "Xrandr")

# Install
install(
    TARGETS sticky-mouse-trap
    RUNTIME DESTINATION bin
)