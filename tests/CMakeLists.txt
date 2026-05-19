find_package(GTest CONFIG REQUIRED)

add_executable(dearsql_lib_tests
    common_tests.cpp
    sqlite_tests.cpp
    postgres_tests.cpp
    mysql_tests.cpp
    mongodb_tests.cpp
    redis_tests.cpp
    mssql_tests.cpp
    oracle_tests.cpp
    cassandra_tests.cpp
    redshift_tests.cpp
)

target_link_libraries(dearsql_lib_tests
    PRIVATE
        dearsql::dearsql
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(dearsql_lib_tests)

# convenience binary for installing Oracle Instant Client locally.
# not a gtest — run manually: ./build/tests/dearsql_oracle_install
add_executable(dearsql_oracle_install oracle_install_smoke.cpp)
target_link_libraries(dearsql_oracle_install PRIVATE dearsql::dearsql)
