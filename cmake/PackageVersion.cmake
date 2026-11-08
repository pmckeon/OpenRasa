# The package's version, worked out by CPack when the package is made (CPACK_PROJECT_CONFIG_FILE), so it always matches the commit
# packaged - not the one the build folder was configured at.
#   on a tag v<x.y.z>            <x.y.z>
#   commits after the tag        <x.y.z>-<commits since>-g<commit>      (git describe)
#   no v* tag in the history     <project version>-<commits>-g<commit>
#   uncommitted changes          ... -dirty
#   no git (a source copy)       <project version>
# Inputs from CMakeLists.txt: CPACK_PACKAGE_NAME, CPACK_PACKAGE_VERSION (the project version), CPACK_VERSION_SOURCE_DIR,
# CPACK_VERSION_PLATFORM.

set(_package_version "${CPACK_PACKAGE_VERSION}")
find_program(_package_git NAMES git)
if (_package_git AND EXISTS "${CPACK_VERSION_SOURCE_DIR}/.git")
    execute_process(COMMAND "${_package_git}" describe --tags --match "v[0-9]*" --dirty
                    WORKING_DIRECTORY "${CPACK_VERSION_SOURCE_DIR}"
                    OUTPUT_VARIABLE _describe RESULT_VARIABLE _describe_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (_describe_result EQUAL 0 AND _describe)
        string(REGEX REPLACE "^v" "" _package_version "${_describe}")
    else()
        execute_process(COMMAND "${_package_git}" rev-list --count HEAD
                        WORKING_DIRECTORY "${CPACK_VERSION_SOURCE_DIR}"
                        OUTPUT_VARIABLE _commits OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${_package_git}" rev-parse --short HEAD
                        WORKING_DIRECTORY "${CPACK_VERSION_SOURCE_DIR}"
                        OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${_package_git}" status --porcelain --untracked-files=no
                        WORKING_DIRECTORY "${CPACK_VERSION_SOURCE_DIR}"
                        OUTPUT_VARIABLE _changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (_commits AND _commit)
            set(_package_version "${CPACK_PACKAGE_VERSION}-${_commits}-g${_commit}")
            if (_changes)
                string(APPEND _package_version "-dirty")
            endif()
        endif()
    endif()
endif()

set(CPACK_PACKAGE_VERSION "${_package_version}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${_package_version}-${CPACK_VERSION_PLATFORM}")
message(STATUS "Package version: ${_package_version}")
