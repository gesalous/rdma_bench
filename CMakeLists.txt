include(cmake_utils/PreventInSouceBuilds.cmake.in)
include(FetchContent)
cmake_minimum_required(VERSION 3.11.0 FATAL_ERROR)
project(
  nida_bench
  VERSION 1.0.0
  DESCRIPTION "NIDA rdma benchmark"
  LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# string(APPEND CMAKE_C_FLAGS " -Werror")
string(APPEND CMAKE_C_FLAGS "-fPIC")
# string( APPEND CMAKE_C_FLAGS " -Wstrict-prototypes -Wall -Wextra
# -fno-omit-frame-pointer -Wunused -Wuninitialized" ) string( APPEND
# CMAKE_C_FLAGS " -Wimplicit-function-declaration -Wshadow -pedantic
# -pedantic-errors -Wmisleading-indentation -pipe" )

# Figure out the build type of the plugin
set(default_build_type "Debug")
string(APPEND CMAKE_C_FLAGS_RELEASE " -O2 -DNDEBUG -finline-functions")
string(APPEND CMAKE_C_FLAGS_DEBUG " -g -ggdb3 -Og -gz")

if(CMAKE_BUILD_TYPE)
  message(STATUS "Setting build type to '${CMAKE_BUILD_TYPE}' as specified.")
else()
  message(
    STATUS
      "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}")
endif()

message(STATUS "Source dir = '${CMAKE_SOURCE_DIR}'")

message(STATUS "Release flags are '${CMAKE_C_FLAGS_RELEASE}'")
message(STATUS "DEBUG flags are '${CMAKE_C_FLAGS_DEBUG}'")
message(STATUSLIB_TEST_ "CMAKE install libdir is '${CMAKE_INSTALL_LIBDIR}'")

# Look for the verbs.h library
find_path(VERBS_INCLUDE_DIR infiniband/verbs.h
          PATHS /usr/include/infiniband /usr/local/include/infiniband
                /usr/include/rdma /usr/local/include/rdma)
# Check if the library was found
if(VERBS_INCLUDE_DIR)
  message(STATUS "Verbs library found at '${VERBS_INCLUDE_DIR}'")
  # Include the directory containing verbs.h
  include_directories(${VERBS_INCLUDE_DIR})
else()
  # Handle the case when the library is not found
  message(FATAL_ERROR "verbs.h library not found!")
endif()

# Look for the verbs.h library
find_path(RDMA_CM_INCLUDE_DIR rdma_cma.h
          PATHS /usr/include/infiniband /usr/local/include/infiniband
                /usr/include/rdma /usr/local/include/rdma)
# Check if the library was found
if(RDMA_CM_INCLUDE_DIR)
  message(STATUS "RDMA CM library found at '${RDMA_CM_INCLUDE_DIR}'")
  # Include the directory containing verbs.h
  include_directories(${RDMA_CM_INCLUDE_DIR})
else()
  # Handle the case when the library is not found
  message(FATAL_ERROR "rdma_cma.h library not found!")
endif()

# Look for the numa.h library
find_path(NUMA_INCLUDE_DIR numa.h PATHS /usr/include /usr/local/include)
# Check if the library was found
if(NUMA_INCLUDE_DIR)
  message(STATUS "NUMA library found at '${NUMA_INCLUDE_DIR}'")
  # Include the directory containing verbs.h
  include_directories(${NUMA_INCLUDE_DIR})
else()
  # Handle the case when the library is not found
  message(FATAL_ERROR "numa.h library not found!")
endif()

FetchContent_Declare(
  uthash
  GIT_REPOSITORY https://github.com/troydhanson/uthash
  GIT_PROGRESS TRUE)

FetchContent_GetProperties(uthash)

# Do not add uthash in the DEPENDENCIES variable. Uthash is a special case
# because it is a header only library & does not support cmake.
if(NOT uthash_POPULATED)
  FetchContent_Populate(uthash)
  # add_subdirectory(${uthash_SOURCE_DIR}/include)
  include_directories(${uthash_SOURCE_DIR}/src ${HDF5_INCLUDE_DIR})
  FetchContent_MakeAvailable(uthash)
endif()

set(LINK_LIBS ${LINK_LIBS} ${HDF5_C_${LIB_TYPE}_LIBRARY})

# It's really easy to pick up the wrong HDF5 library if you set the path wrong.
# Turn this on for added confirmation that you got it right. message
# (DEPRECATION "Include: ${HDF5_INCLUDE_DIR}")

add_subdirectory(nida_server)
add_subdirectory(nida_client)

set(FETCHCONTENT_QUIET FALSE)
FetchContent_Declare(
  log
  GIT_REPOSITORY https://github.com/innerout/log.c.git
  GIT_PROGRESS TRUE)

FetchContent_GetProperties(log)

if(NOT log_POPULATED)
  FetchContent_Populate(log)
  add_subdirectory(${log_SOURCE_DIR} ${log_BINARY_DIR})
  FetchContent_MakeAvailable(log)
endif()
