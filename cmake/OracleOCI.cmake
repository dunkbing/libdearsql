# ODPI-C (Oracle Database Programming Interface for C).
#
# Builds from external/odpi submodule. ODPI dynamically loads Oracle Instant
# Client at runtime, so no Oracle headers/libraries are needed at build time.

add_library(odpi STATIC external/odpi/embed/dpi.c)
target_include_directories(odpi PUBLIC external/odpi/include)
set_target_properties(odpi PROPERTIES C_STANDARD 11)

if(UNIX AND NOT APPLE)
    target_link_libraries(odpi PUBLIC dl)
endif()
