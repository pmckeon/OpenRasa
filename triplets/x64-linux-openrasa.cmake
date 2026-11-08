# vcpkg triplet for OpenRasa on Linux: x64-linux (static libraries). LGPL libraries the client may link later are listed here to be
# built as shared objects and shipped beside the executable (LGPL 2.1 section 6). CMakeLists.txt selects this triplet on Linux hosts
# unless VCPKG_TARGET_TRIPLET is given.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if (PORT MATCHES "^(mpg123)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
