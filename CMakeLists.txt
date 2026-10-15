cmake_minimum_required(VERSION 3.20)
project(EventContractLOB CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -Wall -Wextra -pthread")
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# Pinned to a ToyExchange release tag so a Core change can never silently move
# this repo. To pick up newer Core work, cut a new tag there and bump this.
FetchContent_Declare(
  Core
  GIT_REPOSITORY https://github.com/infinite-simplex/ToyExchange.git
  GIT_TAG v0.2.1
  SOURCE_SUBDIR Core
)
FetchContent_MakeAvailable(Core)

add_subdirectory(ExchangeCommon)
add_subdirectory(MatchingService)
add_subdirectory(NetworkService)
add_subdirectory(Tools/OuchTestClient)
add_subdirectory(Tools/OuchLoadGenerator)
add_subdirectory(Tools/OrderBookBench)
add_subdirectory(Tools/EngineThroughput)

# Core's PerformanceServiceApp (percentiles of the telemetry the replicas forward). Core's
# own CMake builds it, but only the Core/ subdirectory is configured here, so it is opt-in:
# it needs HdrHistogram_c, which is fetched when this is ON.
option(BUILD_PERFORMANCE_SERVICE "Build Core's PerformanceServiceApp (fetches HdrHistogram_c)" OFF)
if(BUILD_PERFORMANCE_SERVICE)
  FetchContent_Declare(
    hdr_histogram
    URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.10.zip
  )
  set(HDR_LOG_REQUIRED "DISABLED" CACHE STRING "" FORCE)
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(hdr_histogram)
  add_subdirectory(${core_SOURCE_DIR}/PerformanceService PerformanceService)
endif()

enable_testing()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ProductTests ProductTesting.cpp)
target_link_libraries(ProductTests
    PRIVATE
        gtest_main
        MatchingService
        NetworkService
        ExchangeCommon
        Core
        Threads::Threads
)

include(GoogleTest)
gtest_discover_tests(ProductTests)
