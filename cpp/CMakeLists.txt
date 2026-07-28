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

get_filename_component(_mkf_libdir "${MKF_LIBRARY}" DIRECTORY)

if(HERTZ_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_hertz tests/test_hertz.cpp tests/test_comb_network.cpp)
    add_executable(deck_dump tools/deck_dump.cpp)
    target_link_libraries(deck_dump PRIVATE hertz)
    target_link_libraries(test_hertz PRIVATE hertz Catch2::Catch2WithMain)
    set_target_properties(test_hertz PROPERTIES BUILD_RPATH "${_mkf_libdir}")
endif()

# --- PyHertz: pybind11 module exposing the engine to Python (mirrors PyKirchhoff) ---
# The Python reference implementation's test suite runs against this module
# (tests/conftest.py --backend=cpp), so its API mirrors src/hertz/*.py.
option(HERTZ_BUILD_PYBIND "Build the PyHertz pybind11 module" ON)
if(HERTZ_BUILD_PYBIND AND NOT EMSCRIPTEN)
    # Take the `python3` on PATH, not the highest version installed: a newer
    # interpreter without its -dev headers would otherwise win and silently
    # disable the module (3.13 vs the 3.12 that has Python.h here).
    set(Python_FIND_STRATEGY LOCATION)
    set(Python_FIND_UNVERSIONED_NAMES FIRST)
    set(Python3_FIND_STRATEGY LOCATION)
    set(Python3_FIND_UNVERSIONED_NAMES FIRST)
    find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
    if(Python3_FOUND)
        # Prefer the pybind11 the interpreter already has; fall back to a fetch.
        execute_process(COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
                        OUTPUT_VARIABLE _pybind11_cmakedir
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
        if(_pybind11_cmakedir)
            find_package(pybind11 CONFIG REQUIRED PATHS "${_pybind11_cmakedir}" NO_DEFAULT_PATH)
        else()
            include(FetchContent)
            FetchContent_Declare(pybind11
                GIT_REPOSITORY https://github.com/pybind/pybind11.git
                GIT_TAG v2.13.6)
            FetchContent_MakeAvailable(pybind11)
        endif()
        pybind11_add_module(PyHertz bindings/py_bindings.cpp)
        target_link_libraries(PyHertz PRIVATE hertz)
        set_target_properties(PyHertz PROPERTIES BUILD_RPATH "${_mkf_libdir}")
    else()
        message(STATUS "PyHertz skipped — no Python development module found")
    endif()
endif()
