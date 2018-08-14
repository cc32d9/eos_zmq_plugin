project(EOSIO_zmq_plugin)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules")

## load in pkg-config support
find_package(PkgConfig REQUIRED)
## use pkg-config to get hints for 0mq locations
pkg_check_modules(PC_ZeroMQ REQUIRED libzmq)

## use the hint from above to find where 'zmq.hpp' is located
find_path(ZeroMQ_INCLUDE_DIR
  NAMES zmq.hpp
  PATHS ${PC_ZeroMQ_INCLUDE_DIRS}
  )

## use the hint from about to find the location of libzmq
find_library(ZeroMQ_LIBRARY
  NAMES zmq
  PATHS ${PC_ZeroMQ_LIBRARY_DIRS}
  )

message(STATUS "[Additional Plugin] EOSIO ZeroMQ plugin enabled")

include_directories(${CMAKE_CURRENT_SOURCE_DIR} include)

add_library( zmq_plugin
             zmq_plugin.cpp
             ${HEADERS} )

target_link_libraries( zmq_plugin chain_plugin eosio_chain ${ZeroMQ_LIBRARY} )

eosio_additional_plugin(zmq_plugin)
