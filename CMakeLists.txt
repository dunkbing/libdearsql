cmake_minimum_required(VERSION 3.23)

# toolchain, triplet and platform flags only when built standalone; as a
# subdirectory the host project owns them
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_DEARSQL_TOP_LEVEL ON)
else()
    set(_DEARSQL_TOP_LEVEL OFF)
endif()

if(_DEARSQL_TOP_LEVEL)
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
endif()

option(DEARSQL_LIB_BUILD_TESTS "Build libdearsql tests" ${_DEARSQL_TOP_LEVEL})

project(libdearsql CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(_DEARSQL_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    if(APPLE)
        add_compile_options(-fexperimental-library)
        add_link_options(-fexperimental-library)
    endif()
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    endif()
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
find_package(httplib CONFIG REQUIRED)

# vendored drivers: freetds_sybdb (or system libsybdb on linux), odpi, cassandra_static
include(cmake/FreeTDS.cmake)
include(cmake/OracleOCI.cmake)
include(cmake/CassandraDriver.cmake)

# ---------- Library ----------
add_library(libdearsql STATIC
    src/types.cpp
    src/connection_info.cpp
    src/sql_builder.cpp
    src/factory.cpp
    src/sqlite_connection.cpp
    src/postgres_connection.cpp
    src/mysql_connection.cpp
    src/mongodb_connection.cpp
    src/redis_connection.cpp
    src/mssql_connection.cpp
    src/oracle_connection.cpp
    src/oracle_installer.cpp
    src/cassandra_connection.cpp
)
add_library(dearsql::dearsql ALIAS libdearsql)
# target named libdearsql so a host can keep an executable called dearsql
set_target_properties(libdearsql PROPERTIES OUTPUT_NAME dearsql)

target_include_directories(libdearsql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(libdearsql PUBLIC cxx_std_20)

# driver targets are PUBLIC so a host that still talks to the native client
# libraries directly (DearSQL during its migration) gets headers + link for free
target_link_libraries(libdearsql
    PUBLIC
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
    PRIVATE
        httplib::httplib
)

if(TARGET hiredis::hiredis_ssl)
    target_link_libraries(libdearsql PUBLIC hiredis::hiredis_ssl)
endif()

# SYSTEM + AFTER so FreeTDS headers never shadow vcpkg MySQL/OpenSSL ones
if(SYBDB_INCLUDE_DIR)
    target_include_directories(libdearsql SYSTEM AFTER PUBLIC ${SYBDB_INCLUDE_DIR})
endif()
if(SYBDB_LIBRARY)
    target_link_libraries(libdearsql PUBLIC ${SYBDB_LIBRARY} ${SYBDB_DEPS})
endif()

# ---------- Tests ----------
if(DEARSQL_LIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------- Install ----------
include(GNUInstallDirs)
install(TARGETS libdearsql
    EXPORT dearsqlTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/dearsql DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
