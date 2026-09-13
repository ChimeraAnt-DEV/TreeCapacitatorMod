cmake_minimum_required(VERSION 3.22)
project(TreeCapacitor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android NDK cross build (arm64-v8a). Configure with:
#   cmake -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
if(NOT ANDROID)
    message(WARNING
        "This project targets Android (LeviLauncher). Configure with an "
        "Android NDK toolchain file and -DANDROID_ABI=arm64-v8a.")
endif()

add_library(tree_capacitor SHARED
    src/Main.cpp
    src/tree_capacitor.cpp
    src/tree_feller.cpp
    src/weather_hooks.cpp
    src/hud.cpp
)

target_include_directories(tree_capacitor PRIVATE
    src
    preloader_headers
    android_stubs
)

# Build-time stub of libpreloader.so (see preloader_stub/). It is never shipped:
# the launcher injects the real libpreloader.so at runtime. This target exists
# only so the mod links with DT_NEEDED libpreloader.so and correctly-formed
# undefined pl:: symbols.
add_library(preloader SHARED
    preloader_stub/preloader_stub.cpp
)
target_include_directories(preloader PRIVATE preloader_headers)
set_target_properties(preloader PROPERTIES
    OUTPUT_NAME "preloader"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
)

target_link_libraries(tree_capacitor PRIVATE
    dl
    log
    preloader
)

# The levipack ships our .so; libpreloader.so is provided by the launcher at
# runtime, so do NOT bundle it. Keep the shared library lean.
set_target_properties(tree_capacitor PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections"
    OUTPUT_NAME "tree_capacitor"
)
