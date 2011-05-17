set(PYGRESQL_TAR_VERSION "4.0")
set(PYGRESQL_TAR_MD5 1aca50e59ff4cc56abe9452a9a49c5ff)

set(PYGRESQL_TAR "PyGreSQL.tgz")
set(PYGRESQL_URL "ftp://ftp.pygresql.org/pub/distrib/${PYGRESQL_TAR}")

set(EXTERNALPROJECT_LIST_SEPARATOR "~!~")
join_strings(_CMAKE_MODULE_PATH
    ${EXTERNALPROJECT_LIST_SEPARATOR} "${CMAKE_MODULE_PATH}")

join_strings(_CMAKE_PREFIX_PATH
    ${EXTERNALPROJECT_LIST_SEPARATOR} "${CMAKE_PREFIX_PATH}")

if(NOT PYGRESQL_TAR_SOURCE)
    find_file(PYGRESQL_TAR_SOURCE ${PYGRESQL_TAR}
        PATHS ${MAD_THIRD_PARTY}/downloads)
endif(NOT PYGRESQL_TAR_SOURCE)

if(NOT PYGRESQL_TAR_SOURCE)
    set(PYGRESQL_TAR_SOURCE ${PYGRESQL_URL})
endif (NOT PYGRESQL_TAR_SOURCE)

ExternalProject_Add(EP_pygresql
    PREFIX ${MAD_THIRD_PARTY}
    LIST_SEPARATOR ${EXTERNALPROJECT_LIST_SEPARATOR}
    DOWNLOAD_DIR ${MAD_THIRD_PARTY}/downloads
    URL ${PYGRESQL_TAR_SOURCE}
    URL_MD5 ${PYGRESQL_TAR_MD5}
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists_PyGreSQL.txt
        ${MAD_THIRD_PARTY}/src/EP_pygresql/CMakeLists.txt
    CMAKE_ARGS
        "-DCMAKE_MODULE_PATH=${_CMAKE_MODULE_PATH}"
        "-DCMAKE_PREFIX_PATH=${_CMAKE_PREFIX_PATH}"
        "-DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}"
    BINARY_DIR ${MAD_THIRD_PARTY}/src/EP_pygresql
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/pygresql
    DESTINATION ports/postgres
    REGEX ".*/\\.DS_Store\$" EXCLUDE
)