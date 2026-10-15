add_executable(EngineThroughput main.cpp)
target_link_libraries(EngineThroughput PRIVATE MatchingService ExchangeCommon Core Threads::Threads)
