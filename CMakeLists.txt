cmake_minimum_required(VERSION 3.22)

# Software version. The specification document has its own number (SPEC v1.6).
project(ZeroFbSuppressor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ZEROFB_BUILD_TESTS "Build the DSP core tests" ON)

# DSP core: header only, no JUCE dependency. The plug-in targets (VST3 / AU)
# are added in milestone Step 4.
add_library(zerofb_core INTERFACE)
target_include_directories(zerofb_core INTERFACE plugin/Source)

if(MSVC)
    # /fp:precise keeps IEEE evaluation order and does not contract a*b+c.
    target_compile_options(zerofb_core INTERFACE /O2 /fp:precise /W4 /permissive- /utf-8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        target_compile_options(zerofb_core INTERFACE /arch:AVX2)
    endif()
else()
    # No fused multiply-add contraction: the AVX2, NEON and scalar paths and
    # the NumPy reference must perform the same IEEE operations.
    target_compile_options(zerofb_core INTERFACE -O3 -ffp-contract=off -Wall -Wextra -Wconversion -Wshadow)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        # -mfma is used only by the explicit intrinsics in nn/SimdKernels.h.
        target_compile_options(zerofb_core INTERFACE -mavx2 -mfma)
    endif()
endif()

if(ZEROFB_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(ZEROFB_FIXTURE_DIR "${CMAKE_BINARY_DIR}/fixtures")
    set(ZEROFB_FIXTURE_STAMP "${ZEROFB_FIXTURE_DIR}/sha256.txt")
    add_custom_command(
        OUTPUT "${ZEROFB_FIXTURE_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/training/gen_golden_fixtures.py" "${ZEROFB_FIXTURE_DIR}"
        DEPENDS "${CMAKE_SOURCE_DIR}/training/gen_golden_fixtures.py" "${CMAKE_SOURCE_DIR}/training/erb_dsp.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/training"
        COMMENT "Generating golden fixtures (NumPy reference)"
        VERBATIM)
    add_custom_target(zerofb_fixtures ALL DEPENDS "${ZEROFB_FIXTURE_STAMP}")

    # EmbeddedWeights.h with the fixed test weights (seed 42, not a trained
    # model). The trained weights are committed to plugin/Source/nn in Step 3.
    set(ZEROFB_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
    set(ZEROFB_TEST_WEIGHTS "${ZEROFB_GENERATED_DIR}/nn/EmbeddedWeights.h")
    add_custom_command(
        OUTPUT "${ZEROFB_TEST_WEIGHTS}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/training/export_weights.py" --random-test
                --swmodel "${ZEROFB_GENERATED_DIR}/test_model.swmodel" --header "${ZEROFB_TEST_WEIGHTS}"
        DEPENDS "${CMAKE_SOURCE_DIR}/training/export_weights.py"
        COMMENT "Generating EmbeddedWeights.h (test weights)"
        VERBATIM)
    add_custom_target(zerofb_test_weights DEPENDS "${ZEROFB_TEST_WEIGHTS}")

    function(zerofb_add_test_executable name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE zerofb_core)
        target_include_directories(${name} PRIVATE "${ZEROFB_GENERATED_DIR}")
        add_dependencies(${name} zerofb_test_weights)
    endfunction()

    zerofb_add_test_executable(ZeroLatencyTest plugin/tests/ZeroLatencyTest.cpp)
    add_test(NAME ZeroLatencyTest COMMAND ZeroLatencyTest)

    # Same test, std:: math instead of the fast approximations.
    zerofb_add_test_executable(ZeroLatencyTest_exact plugin/tests/ZeroLatencyTest.cpp)
    target_compile_definitions(ZeroLatencyTest_exact PRIVATE ZEROFB_EXACT_MATH=1)
    add_test(NAME ZeroLatencyTest_exact COMMAND ZeroLatencyTest_exact)

    # GoldenParityTest in three builds: production (fast math, SIMD),
    # exact math (tight tolerance), and the scalar fallback path.
    foreach(variant fast exact scalar)
        set(target GoldenParityTest_${variant})
        zerofb_add_test_executable(${target} plugin/tests/GoldenParityTest.cpp)
        add_dependencies(${target} zerofb_fixtures)
        add_test(NAME ${target} COMMAND ${target} "${ZEROFB_FIXTURE_DIR}")
    endforeach()
    target_compile_definitions(GoldenParityTest_exact PRIVATE ZEROFB_EXACT_MATH=1)

    # CPU load measurement (SPEC 1.2). Informational: it prints the numbers
    # and only fails when the engine allocates or produces non-finite output.
    zerofb_add_test_executable(EngineBench plugin/tests/EngineBench.cpp)
    add_test(NAME EngineBench COMMAND EngineBench)

    # Closed-loop bench skeleton (SPEC 7.1). Verifies its own machinery; the
    # GBF figures are informational until a trained model exists (Step 3).
    zerofb_add_test_executable(ClosedLoopBench plugin/tests/ClosedLoopBench.cpp)
    add_test(NAME ClosedLoopBench COMMAND ClosedLoopBench)
    set_tests_properties(ClosedLoopBench PROPERTIES TIMEOUT 1800)
    target_compile_definitions(GoldenParityTest_scalar PRIVATE ZEROFB_EXACT_MATH=1 ZEROFB_FORCE_SCALAR=1)
endif()
