cmake_minimum_required(VERSION 3.23)

# vcpkg toolchain - must be set before project() call.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    elseif(EXISTS "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    endif()
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(APPLE)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(ARCH STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "vcpkg triplet" FORCE)
        else()
            set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "vcpkg triplet" FORCE)
        endif()
    elseif(UNIX)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(ARCH STREQUAL "x86_64")
            set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg triplet" FORCE)
        else()
            set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "vcpkg triplet" FORCE)
        endif()
    elseif(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet" FORCE)
    endif()
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

option(DEARSQL_LIB_BUILD_TESTS "Build libdearsql tests" ON)

project(libdearsql CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    add_compile_options(-fexperimental-library)
    add_link_options(-fexperimental-library)
endif()

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

# ---------- Find dependencies ----------
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(hiredis_ssl CONFIG QUIET)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)

include(cmake/FreeTDS.cmake)
include(cmake/OracleOCI.cmake)
include(cmake/CassandraDriver.cmake)

# ---------- Library ----------
add_library(dearsql STATIC
    src/types.cpp
    src/connection_info.cpp
    src/factory.cpp
    src/sqlite_connection.cpp
    src/postgres_connection.cpp
    src/mysql_connection.cpp
    src/mongodb_connection.cpp
    src/redis_connection.cpp
    src/mssql_connection.cpp
    src/oracle_connection.cpp
    src/cassandra_connection.cpp
)
add_library(dearsql::dearsql ALIAS dearsql)

target_include_directories(dearsql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(dearsql PUBLIC cxx_std_20)

target_link_libraries(dearsql
    PRIVATE
        unofficial::sqlite3::sqlite3
        PostgreSQL::PostgreSQL
        unofficial::libmariadb
        hiredis::hiredis
        $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
        $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
        odpi
        cassandra_static
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(TARGET hiredis::hiredis_ssl)
    target_link_libraries(dearsql PRIVATE hiredis::hiredis_ssl)
endif()

if(SYBDB_INCLUDE_DIR)
    target_include_directories(dearsql SYSTEM PRIVATE ${SYBDB_INCLUDE_DIR})
endif()
if(SYBDB_LIBRARY)
    target_link_libraries(dearsql PRIVATE ${SYBDB_LIBRARY} ${SYBDB_DEPS})
endif()

# ---------- Tests ----------
if(DEARSQL_LIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------- Install ----------
include(GNUInstallDirs)
install(TARGETS dearsql
    EXPORT dearsqlTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/dearsql DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
