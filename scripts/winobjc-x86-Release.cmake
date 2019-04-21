set (CL_COMPILER ON)

list (APPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/scripts")
include (CommonVariables)

file (MAKE_DIRECTORY "${RELEASE_WINOBJC_CMAKE_DIR}")
execute_process (
    COMMAND "${CMAKE_COMMAND}" -G Ninja
        "-DSOURCE_DIR=${SOURCE_DIR}"
        "-DBINARY_DIR=${BINARY_DIR}"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=On
        -DCMAKE_BUILD_TYPE=Release
        "${SOURCE_DIR}/deps/WinObjC"
    WORKING_DIRECTORY "${RELEASE_WINOBJC_CMAKE_DIR}")