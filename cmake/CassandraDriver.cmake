# DataStax C++ driver for Apache Cassandra.
#
# Builds static `cassandra_static` from external/cassandra-cpp-driver. Depends
# on libuv (vcpkg), OpenSSL, and zlib.

set(CASS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CASS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_INTEGRATION_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_USE_OPENSSL ON CACHE BOOL "" FORCE)
if(WIN32)
    set(CASS_USE_ZLIB OFF CACHE BOOL "" FORCE)
else()
    set(CASS_USE_ZLIB ON CACHE BOOL "" FORCE)
endif()
set(CASS_USE_KERBEROS OFF CACHE BOOL "" FORCE)
set(CASS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CASS_INSTALL_HEADER OFF CACHE BOOL "" FORCE)
set(CASS_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(CASS_MULTICORE_COMPILATION ON CACHE BOOL "" FORCE)

find_package(libuv CONFIG QUIET)
if(TARGET libuv::uv_a)
    get_target_property(_LIBUV_INCLUDE libuv::uv_a INTERFACE_INCLUDE_DIRECTORIES)
    set(LIBUV_ROOT_DIR "${_LIBUV_INCLUDE}/.." CACHE PATH "" FORCE)
endif()

# cassandra-cpp-driver's CMakeLists requires CXX_COMPILER_ID to be one of
# Clang/GNU/MSVC; on macOS it's "AppleClang".
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_CXX_COMPILER_ID "Clang")
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(external/cassandra-cpp-driver EXCLUDE_FROM_ALL)

# GCC 13+ false-positive on operator<=>
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(cassandra_static
        PRIVATE -Wno-error=stringop-overread -Wno-stringop-overread)
endif()

target_compile_definitions(cassandra_static INTERFACE CASS_STATIC)
target_include_directories(cassandra_static
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/cassandra-cpp-driver/include)
