# FreeTDS db-lib for MSSQL support (static linking)
#
# Sets: SYBDB_LIBRARY, SYBDB_INCLUDE_DIR, SYBDB_DEPS

# db-lib drops the login's CA file when it builds the connection (tds_config_login,
# FreeTDS 1.5.14), so a "ca file" set on the login never reaches the TLS setup.
# patch the pinned submodule once; drop this when upstream copies cafile there
find_package(Git REQUIRED)
set(_FREETDS_PATCH "${CMAKE_CURRENT_LIST_DIR}/freetds-login-cafile.patch")
set(_FREETDS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/external/freetds")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_FREETDS_PATCH}"
    WORKING_DIRECTORY "${_FREETDS_SRC}"
    RESULT_VARIABLE _FREETDS_PATCHED
    OUTPUT_QUIET ERROR_QUIET)
if(NOT _FREETDS_PATCHED EQUAL 0)
  execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply "${_FREETDS_PATCH}"
      WORKING_DIRECTORY "${_FREETDS_SRC}"
      RESULT_VARIABLE _FREETDS_PATCH_RC)
  if(NOT _FREETDS_PATCH_RC EQUAL 0)
    message(FATAL_ERROR "could not apply ${_FREETDS_PATCH}")
  endif()
endif()

if(LINUX)
  # build FreeTDS db-lib from the submodule against vcpkg's OpenSSL: distro
  # packages lag (ubuntu 22.04 ships 1.3.6, which has no encryption setting)
  include(ExternalProject)
  set(FREETDS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/freetds")
  set(FREETDS_BINARY_DIR "${CMAKE_BINARY_DIR}/freetds-build")
  get_target_property(_OPENSSL_SSL_LOC OpenSSL::SSL IMPORTED_LOCATION)
  if(NOT _OPENSSL_SSL_LOC)
    get_target_property(_OPENSSL_SSL_LOC OpenSSL::SSL IMPORTED_LOCATION_RELEASE)
  endif()
  get_filename_component(_VCPKG_OPENSSL_LIB_DIR "${_OPENSSL_SSL_LOC}" DIRECTORY)
  get_filename_component(_VCPKG_OPENSSL_ROOT "${_VCPKG_OPENSSL_LIB_DIR}" DIRECTORY)
  set(_FREETDS_LIBS
        "${FREETDS_BINARY_DIR}/src/dblib/libdb-lib.a"
        "${FREETDS_BINARY_DIR}/src/tds/libtds.a"
        "${FREETDS_BINARY_DIR}/src/replacements/libreplacements.a"
        "${FREETDS_BINARY_DIR}/src/utils/libtdsutils.a"
    )
  ExternalProject_Add(
        freetds_external
        SOURCE_DIR "${FREETDS_SOURCE_DIR}"
        BINARY_DIR "${FREETDS_BINARY_DIR}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DOPENSSL_ROOT_DIR=${_VCPKG_OPENSSL_ROOT}
            -DOPENSSL_USE_STATIC_LIBS=ON
        # only the static db-lib and what it depends on
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target db-lib
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${_FREETDS_LIBS}
    )
  add_library(freetds_sybdb INTERFACE)
  add_dependencies(freetds_sybdb freetds_external)
  # db-lib first: the linker resolves left to right
  target_link_libraries(freetds_sybdb INTERFACE ${_FREETDS_LIBS} OpenSSL::SSL OpenSSL::Crypto)
  set(SYBDB_LIBRARY freetds_sybdb)
  # public headers from the source tree, plus the generated tds_sysdep_public.h
  set(SYBDB_INCLUDE_DIR "${FREETDS_SOURCE_DIR}/include" "${FREETDS_BINARY_DIR}/include")
  set(SYBDB_DEPS "")
elseif(WIN32)
  # build FreeTDS db-lib from submodule (vcpkg freetds conflicts with
  # libmariadb)
  include(ExternalProject)
  set(FREETDS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/freetds")
  set(FREETDS_INSTALL_DIR "${CMAKE_BINARY_DIR}/freetds-install")
  set(FREETDS_ICONV_DIR "${FREETDS_SOURCE_DIR}/iconv")
  # derive vcpkg OpenSSL root from OPENSSL_INCLUDE_DIR (set by
  # find_package(OpenSSL))
  get_filename_component(
        _VCPKG_OPENSSL_ROOT
        "${OPENSSL_INCLUDE_DIR}"
        DIRECTORY
    )
  # find vcpkg libiconv and set up the iconv/ dir structure FreeTDS expects
  find_library(_ICONV_LIB NAMES iconv libiconv REQUIRED)
  find_path(_ICONV_INC iconv.h REQUIRED)
  file(MAKE_DIRECTORY "${FREETDS_ICONV_DIR}/lib")
  file(MAKE_DIRECTORY "${FREETDS_ICONV_DIR}/include")
  file(
        COPY_FILE "${_ICONV_LIB}"
        "${FREETDS_ICONV_DIR}/lib/iconv.lib"
        ONLY_IF_DIFFERENT
    )
  file(
        COPY "${_ICONV_INC}/iconv.h"
        DESTINATION "${FREETDS_ICONV_DIR}/include"
    )
  ExternalProject_Add(
        freetds_external
        SOURCE_DIR "${FREETDS_SOURCE_DIR}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${FREETDS_INSTALL_DIR}
            -DOPENSSL_ROOT_DIR=${_VCPKG_OPENSSL_ROOT}
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
            -DCMAKE_C_FLAGS_RELEASE=/MT -DCMAKE_C_FLAGS_DEBUG=/MTd
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        BUILD_COMMAND
            ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target
            db-lib tds replacements tdsutils
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/lib
        # merge all FreeTDS static libs into one (db-lib depends on tds,
        # replacements, tdsutils)
        COMMAND
            lib /NOLOGO /OUT:${FREETDS_INSTALL_DIR}/lib/sybdb.lib
            <BINARY_DIR>/src/dblib/Release/db-lib.lib
            <BINARY_DIR>/src/tds/Release/tds.lib
            <BINARY_DIR>/src/replacements/Release/replacements.lib
            <BINARY_DIR>/src/utils/Release/tdsutils.lib
        COMMAND
            ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/include
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory ${FREETDS_SOURCE_DIR}/include
            ${FREETDS_INSTALL_DIR}/include
        COMMAND
            ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/config.h
            ${FREETDS_INSTALL_DIR}/include/config.h
        COMMAND
            ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/tds_sysdep_public.h
            ${FREETDS_INSTALL_DIR}/include/tds_sysdep_public.h
        BUILD_BYPRODUCTS "${FREETDS_INSTALL_DIR}/lib/sybdb.lib"
    )
  add_library(freetds_sybdb STATIC IMPORTED)
  set_target_properties(
        freetds_sybdb
        PROPERTIES IMPORTED_LOCATION "${FREETDS_INSTALL_DIR}/lib/sybdb.lib"
    )
  add_dependencies(freetds_sybdb freetds_external)
  set(SYBDB_LIBRARY freetds_sybdb)
  set(SYBDB_INCLUDE_DIR "${FREETDS_INSTALL_DIR}/include")
  set(SYBDB_DEPS ws2_32 crypt32 ${_ICONV_LIB} legacy_stdio_definitions.lib)
elseif(APPLE)
  # build FreeTDS db-lib from submodule with correct deployment target
  include(ExternalProject)
  set(FREETDS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/freetds")
  set(FREETDS_INSTALL_DIR "${CMAKE_BINARY_DIR}/freetds-install")
  # point FreeTDS at vcpkg's OpenSSL (not Homebrew's)
  get_target_property(_OPENSSL_SSL_LOC OpenSSL::SSL IMPORTED_LOCATION)
  get_filename_component(
        _VCPKG_OPENSSL_LIB_DIR
        "${_OPENSSL_SSL_LOC}"
        DIRECTORY
    )
  get_filename_component(
        _VCPKG_OPENSSL_ROOT
        "${_VCPKG_OPENSSL_LIB_DIR}"
        DIRECTORY
    )
  ExternalProject_Add(
        freetds_external
        SOURCE_DIR "${FREETDS_SOURCE_DIR}"
        CMAKE_ARGS
            -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${FREETDS_INSTALL_DIR}
            -DCMAKE_C_FLAGS=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -DOPENSSL_ROOT_DIR=${_VCPKG_OPENSSL_ROOT}
            -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
            -DCMAKE_SYSTEM_IGNORE_PREFIX_PATH=/opt/homebrew
        # only build the static db-lib target (skip shared libs that fail to link)
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target db-lib
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/lib
        # merge all FreeTDS static libs into one (db-lib depends on tds,
        # replacements, tdsutils)
        COMMAND
            libtool -static -o ${FREETDS_INSTALL_DIR}/lib/libsybdb.a
            <BINARY_DIR>/src/dblib/libdb-lib.a <BINARY_DIR>/src/tds/libtds.a
            <BINARY_DIR>/src/replacements/libreplacements.a
            <BINARY_DIR>/src/utils/libtdsutils.a
        COMMAND
            ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/include
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory ${FREETDS_SOURCE_DIR}/include
            ${FREETDS_INSTALL_DIR}/include
        COMMAND
            ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/config.h
            ${FREETDS_INSTALL_DIR}/include/config.h
        COMMAND
            ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/tds_sysdep_public.h
            ${FREETDS_INSTALL_DIR}/include/tds_sysdep_public.h
        BUILD_BYPRODUCTS "${FREETDS_INSTALL_DIR}/lib/libsybdb.a"
    )
  add_library(freetds_sybdb STATIC IMPORTED)
  set_target_properties(
        freetds_sybdb
        PROPERTIES IMPORTED_LOCATION "${FREETDS_INSTALL_DIR}/lib/libsybdb.a"
    )
  add_dependencies(freetds_sybdb freetds_external)
  set(SYBDB_LIBRARY freetds_sybdb)
  set(SYBDB_INCLUDE_DIR "${FREETDS_INSTALL_DIR}/include")
  find_library(KERBEROS_FRAMEWORK Kerberos)
  find_library(ICONV_LIBRARY iconv)
  set(SYBDB_DEPS ${KERBEROS_FRAMEWORK} ${ICONV_LIBRARY})
endif()
