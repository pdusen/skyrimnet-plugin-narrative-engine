# MinLL/CommonLibVR, the MIT continuation of CommonLibSSE NG, pinned to the
# v4.39.5 release tag. It is the first release that loads on Skyrim SE 1.7.x:
# it reads the format-5 address library and classifies 1.7.x as an AE runtime.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MinLL/CommonLibVR
    REF 550cc4fb9114649dcf526d1f3d73d710c5d7003b
    SHA512 a053d852b682d597acc9e7684740ab95f212e79542265a3e379ff0132bb333592051ddaa2ffc81b1f85fdbda080afe360f5dc7a54d15d1aa7982a98865ce7243
    HEAD_REF ng
)

# extern/openvr is a git submodule, which a GitHub archive does not carry.
# This is the commit v4.39.5 pins it to.
vcpkg_from_github(
    OUT_SOURCE_PATH OPENVR_SOURCE_PATH
    REPO ValveSoftware/openvr
    REF 60eb187801956ad277f1cae6680e3a410ee0873b
    SHA512 bb85b4705e7095ac65df9969112b2df8930cee7917cc5f14231c5a0ffeed7a73ffa60727fd32f8786a403656f95a3ec0f80bf3ceabc5b8ede964aefb920bc718
    HEAD_REF master
)
file(COPY "${OPENVR_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/extern/openvr")

# All three runtimes stay on (the library's default), so one DLL serves SE, AE
# and VR.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME CommonLibSSE CONFIG_PATH lib/cmake/CommonLibSSE)
vcpkg_copy_pdbs()

# The installed config includes the add_commonlibsse_plugin helper beside it,
# but the library's own install rules never copy it there.
file(
    INSTALL "${SOURCE_PATH}/cmake/CommonLibSSE.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE"
)

# RE/B/BSOpenVR.h includes openvr.h, which the library's install rules leave behind.
file(
    INSTALL "${OPENVR_SOURCE_PATH}/headers/openvr.h"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
)

# The library links openvr_api.lib PUBLIC by its absolute path in the source
# tree, which the exported target would otherwise carry into every consumer.
# Install the import library and point the target at the installed copy.
file(
    INSTALL "${OPENVR_SOURCE_PATH}/lib/win64/openvr_api.lib"
    DESTINATION "${CURRENT_PACKAGES_DIR}/lib"
)
if(NOT VCPKG_BUILD_TYPE)
    file(
        INSTALL "${OPENVR_SOURCE_PATH}/lib/win64/openvr_api.lib"
        DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib"
    )
endif()
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE-targets.cmake"
    "${SOURCE_PATH}/extern/openvr/lib/win64/openvr_api.lib"
    "\${_IMPORT_PREFIX}/lib/openvr_api.lib"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
