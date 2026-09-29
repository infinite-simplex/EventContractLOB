cmake_minimum_required(VERSION 3.20)
project(EventContractLOB CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -Wall -Wextra -pthread")
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# Pinned to main for now; once ToyExchange starts cutting releases, pin to
# a version tag instead so a Core change can never silently move this repo.
FetchContent_Declare(
  Core
  GIT_REPOSITORY https://github.com/infinite-simplex/ToyExchange.git
  GIT_TAG main
  SOURCE_SUBDIR Core
)
FetchContent_MakeAvailable(Core)

add_subdirectory(ExchangeCommon)
add_subdirectory(MatchingService)
add_subdirectory(NetworkService)
add_subdirectory(Tools/OuchTestClient)

add_executable(ExchangeMain main.cpp)
target_link_libraries(ExchangeMain
    PRIVATE
        NetworkService
        MatchingService
        ExchangeCommon
        Core
        Threads::Threads
)

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
