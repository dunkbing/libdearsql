# libpg_query: PostgreSQL's own parser as a library, for syntax checks (src/syntax.cpp).
#
# Built from the submodule (external/libpg_query), with its vendored protobuf-c and
# xxhash. It carries copies of postgres internals (palloc, pg_snprintf, list_free...)
# that libpq's pgcommon/pgport and libmariadb also define, so every global symbol
# except the pg_query_* api is renamed by a forced include (pg_query_prefix.h,
# generated: see that file). Static archives can't hide symbols portably; this can.

set(PGQ external/libpg_query)
file(GLOB PGQ_SOURCES ${PGQ}/src/*.c ${PGQ}/src/postgres/*.c)
add_library(pg_query STATIC ${PGQ_SOURCES} ${PGQ}/vendor/protobuf-c/protobuf-c.c
                            ${PGQ}/vendor/xxhash/xxhash.c ${PGQ}/protobuf/pg_query.pb-c.c)
target_include_directories(pg_query PRIVATE ${PGQ} ${PGQ}/vendor ${PGQ}/src/include
                                            ${PGQ}/src/postgres/include)
target_include_directories(pg_query INTERFACE ${PGQ})
set_target_properties(pg_query PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_include_directories(pg_query PRIVATE ${PGQ}/src/postgres/include/port/win32)
    if(MSVC)
        target_include_directories(pg_query PRIVATE ${PGQ}/src/postgres/include/port/win32_msvc)
    endif()
endif()
if(MSVC)
    target_compile_options(pg_query PRIVATE /w /FI${CMAKE_CURRENT_LIST_DIR}/pg_query_prefix.h)
else()
    target_compile_options(pg_query PRIVATE -w -fno-strict-aliasing -fwrapv
                                            -include ${CMAKE_CURRENT_LIST_DIR}/pg_query_prefix.h)
endif()
