cmake_minimum_required(VERSION 3.24)
project(hertz LANGUAGES CXX)

option(HERTZ_BUILD_TESTS "Build the Catch2 test binary" ON)

# The FFT comes from MKF (OpenMagnetics::fft, see include/hertz/MkfFft.hpp).
# House rule: always consume the latest MKF main; point HERTZ_MKF_ROOT at a
# checkout with a built libMKF (native: build/libMKF.so; WASM: a libMKF built
# with the same Emscripten toolchain).
set(HERTZ_MKF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../OpenMagnetics/MKF"
    CACHE PATH "MKF checkout providing libMKF (fft)")

add_library(hertz INTERFACE)
target_include_directories(hertz INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hertz INTERFACE cxx_std_20)

find_library(MKF_LIBRARY MKF PATHS ${HERTZ_MKF_ROOT}/build NO_DEFAULT_PATH)
if(NOT MKF_LIBRARY)
    message(FATAL_ERROR
        "libMKF not found under ${HERTZ_MKF_ROOT}/build — build MKF first or set HERTZ_MKF_ROOT")
endif()
target_link_libraries(hertz INTERFACE ${MKF_LIBRARY})

if(HERTZ_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_hertz tests/test_hertz.cpp tests/test_comb_network.cpp)
    target_link_libraries(test_hertz PRIVATE hertz Catch2::Catch2WithMain)
    get_filename_component(_mkf_libdir "${MKF_LIBRARY}" DIRECTORY)
    set_target_properties(test_hertz PROPERTIES BUILD_RPATH "${_mkf_libdir}")
endif()
