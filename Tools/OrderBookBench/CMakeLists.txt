add_executable(OrderBookBench main.cpp)
target_link_libraries(OrderBookBench PRIVATE MatchingService ExchangeCommon Core Threads::Threads)

# The same benchmark built against a copy of OrderBook.hpp without the telemetry counter
# increments, to measure what they cost (see Tools/bench_counters.sh). Configuring fails if a
# line this removes is no longer in OrderBook.hpp, so the copy can never silently be identical.
set(book_header "${CMAKE_SOURCE_DIR}/MatchingService/OrderBook.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${book_header}")
file(READ "${book_header}" book_source)
macro(remove_from_book text)
  string(FIND "${book_source}" "${text}" found_at)
  if(found_at EQUAL -1)
    message(FATAL_ERROR "OrderBookBench: not found in OrderBook.hpp: ${text}")
  endif()
  string(REPLACE "${text}" "" book_source "${book_source}")
endmacro()
remove_from_book("OrderTrace& trace = g_telemetry_arena[trace_index(order.m_telemetry_idx)]")
remove_from_book("++trace.ext_counters[static_cast<size_t>(LobExtSlot::PriceLevelsTouched)]")
remove_from_book("++trace.ext_counters[static_cast<size_t>(LobExtSlot::RestingOrdersTouched)]")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nocounters/OrderBook.hpp" "${book_source}")

add_executable(OrderBookBenchNoCounters main.cpp)
target_include_directories(OrderBookBenchNoCounters BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nocounters")
target_link_libraries(OrderBookBenchNoCounters PRIVATE MatchingService ExchangeCommon Core Threads::Threads)
