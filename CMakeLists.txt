# It's recommended to set a minimum CMake version.
# If you use CMake features from higher versions, update this to match.
cmake_minimum_required(VERSION 3.21)

# Set your project name. This will be the name of your SKSE .dll file.
project(NarrativeEngine VERSION 0.0.1 LANGUAGES CXX)

# #
# YOU DO NOT NEED TO EDIT ANYTHING BELOW HERE
# #

# If you're not using a mod manager, you probably want the SKSE plugin to go
# inside of your Skyrim "Data" folder.
#
# To do this automatically, set the `SKYRIM_FOLDER` environment variable
# to the path of your Skyrim Special Edition folder
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

# If you're using Mod Organizer 2 or Vortex, you might want this to go inside
# of your "mods" folder, inside of a subfolder named "<your mod>".
#
# To do this automatically, set the `SKYRIM_MODS_FOLDER` environment variable
# to the path of your "mods" folder
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

# Otherwise, you can set OUTPUT_FOLDER to any place you'd like :)
# set(OUTPUT_FOLDER "C:/path/to/any/folder")

# Get the .cpp files from your src/ directory
# to be passed into add_commonlibsse_plugin
#
# This helper automatically finds all of your .cpp files for you
#
# Please note: the official recommendation is to list our every .cpp
# file individually after SOURCES
file(GLOB_RECURSE source_files CONFIGURE_DEPENDS src/*.cpp)

find_package(directxtk CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# Setup your SKSE plugin as an SKSE plugin!
find_package(CommonLibSSE CONFIG REQUIRED)
# simpleini ships as a header-only library — no CMake config, just an include path.
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${source_files} plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h) # <--- PCH.h is required!
target_include_directories(${PROJECT_NAME} PRIVATE include "${SIMPLEINI_INCLUDE_DIRS}")
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# SkyrimNet integration
# ---------------------------------------------------------------------------
#
# NarrativeEngine is a SkyrimNet plugin and compiles against SkyrimNet's
# public C++ API. The API is delivered as a single header (`PublicAPI.h`)
# that contains an inline `FindFunctions()` which dynamically loads
# `SkyrimNet.dll` at runtime via `LoadLibraryA` + `GetProcAddress` — there
# is no import library (`.lib`) shipped alongside the DLL, and none is
# needed. The Windows loader resolves `SkyrimNet.dll` at runtime from the
# `Data/SKSE/Plugins/` folder, where SkyrimNet must already be installed
# for our plugin to function.
#
# So the only build-time integration is making the header visible to our
# sources. Point `SKYRIMNET_DIR` at SkyrimNet's mod folder via either:
#   - `SKYRIMNET_DIR` env var (explicit override), or
#   - `SKYRIM_MODS_FOLDER` env var (assumes the standard "SkyrimNet" subdir).
if(DEFINED ENV{SKYRIMNET_DIR} AND IS_DIRECTORY "$ENV{SKYRIMNET_DIR}/CppAPI")
    set(SKYRIMNET_DIR "$ENV{SKYRIMNET_DIR}")
elseif(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet/CppAPI")
    set(SKYRIMNET_DIR "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet")
endif()

if(NOT DEFINED SKYRIMNET_DIR)
    message(FATAL_ERROR
        "Could not locate SkyrimNet. Set the SKYRIMNET_DIR environment variable to "
        "the SkyrimNet mod folder (containing CppAPI/PublicAPI.h), or set "
        "SKYRIM_MODS_FOLDER so SkyrimNet is found at \${SKYRIM_MODS_FOLDER}/SkyrimNet.")
endif()

message(STATUS "SkyrimNet found at: ${SKYRIMNET_DIR}")
target_include_directories(${PROJECT_NAME} PRIVATE "${SKYRIMNET_DIR}/CppAPI")
# No `target_link_libraries(... SkyrimNet)` — `PublicAPI.h`'s `FindFunctions()`
# loads `SkyrimNet.dll` at runtime via `LoadLibraryA`. Call it from
# `kDataLoaded` to populate the function pointers.

# ---------------------------------------------------------------------------
# PrismaUI integration
# ---------------------------------------------------------------------------
#
# PrismaUI is a runtime soft dependency — NarrativeEngine's Director loop
# runs without it; only the in-game dashboard overlay needs it. At build
# time, however, we still need the PrismaUI public API header
# (`PrismaUI_API.h`) on the include path so our wrapper TU
# (`src/PrismaUI_API.cpp`) can compile. PrismaUI does not ship its public
# header alongside the DLL, so the header lives in a separate location.
#
# Point `PRISMA_UI_INCLUDE` at the directory containing `PrismaUI_API.h`.
if(DEFINED ENV{PRISMA_UI_INCLUDE} AND EXISTS "$ENV{PRISMA_UI_INCLUDE}/PrismaUI_API.h")
    set(PRISMA_UI_INCLUDE "$ENV{PRISMA_UI_INCLUDE}")
endif()

if(NOT DEFINED PRISMA_UI_INCLUDE)
    message(FATAL_ERROR
        "Could not locate the PrismaUI API header. Set the PRISMA_UI_INCLUDE "
        "environment variable to the directory containing PrismaUI_API.h.")
endif()

message(STATUS "PrismaUI API header found at: ${PRISMA_UI_INCLUDE}")
target_include_directories(${PROJECT_NAME} PRIVATE "${PRISMA_UI_INCLUDE}")
# No `target_link_libraries(... PrismaUI)` — our `src/PrismaUI_API.cpp` will
# soft-link `PrismaUI.dll` at runtime via `GetModuleHandle` + the upstream
# `RequestPluginAPI()` helper, degrading gracefully when PrismaUI is absent.

# When your SKSE .dll is compiled, this will automatically copy the .dll into your mods folder.
# Only works if you configure DEPLOY_ROOT above (or set the SKYRIM_MODS_FOLDER environment variable)
if(DEFINED OUTPUT_FOLDER)
    # If you specify an <OUTPUT_FOLDER> (including via environment variables)
    # then we'll copy your mod files into Skyrim or a mod manager for you!

    # Copy the SKSE plugin .dll files into the SKSE/Plugins/ folder
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    # If you perform a "Debug" build, also copy .pdb file (for debug symbols)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()

    # Deploy everything under statics/ verbatim into the mod folder,
    # preserving directory structure. The repo-side layout mirrors the mod
    # folder layout exactly — so e.g. `statics/SKSE/Plugins/NarrativeEngine.ini`
    # lands at `<OUTPUT_FOLDER>/SKSE/Plugins/NarrativeEngine.ini`.
    #
    # We enumerate at configure time via GLOB_RECURSE (CONFIGURE_DEPENDS makes
    # the glob re-run at build time and re-trigger configure if the file list
    # changed). Each file is wired as the OUTPUT of a custom command whose
    # input is the source-side file, then aggregated into a `deploy_statics`
    # target that's added to ALL. Two consequences:
    #
    #   * Editing a static file alone (no C++ touched) still triggers
    #     deployment — Ninja sees the OUTPUT is stale relative to its DEPENDS
    #     input and runs the copy. The previous POST_BUILD-of-main-target
    #     pattern only fired when the DLL actually re-linked, so prompt-only
    #     edits would silently skip the deploy.
    #   * Adding/removing a file is picked up by CONFIGURE_DEPENDS on the next
    #     build. (Removed files leave their previous deploy behind in the mod
    #     folder — we don't currently sweep stale outputs.)
    #
    # `make_directory` is idempotent, so multiple files in the same destination
    # subdir cause no issues. `copy_if_different` skips no-op writes.
    set(STATICS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/statics")
    if(IS_DIRECTORY "${STATICS_SOURCE_DIR}")
        file(GLOB_RECURSE STATICS_FILES CONFIGURE_DEPENDS
            RELATIVE "${STATICS_SOURCE_DIR}"
            "${STATICS_SOURCE_DIR}/*")
        if(STATICS_FILES)
            message(STATUS "Statics deploy: ${STATICS_SOURCE_DIR} -> ${OUTPUT_FOLDER}")
            set(STATICS_DEPLOY_OUTPUTS "")
            foreach(rel IN LISTS STATICS_FILES)
                set(src "${STATICS_SOURCE_DIR}/${rel}")
                set(dst "${OUTPUT_FOLDER}/${rel}")
                get_filename_component(dst_dir "${dst}" DIRECTORY)
                add_custom_command(
                    OUTPUT "${dst}"
                    DEPENDS "${src}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dst_dir}"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
                    VERBATIM
                )
                list(APPEND STATICS_DEPLOY_OUTPUTS "${dst}")
            endforeach()
            add_custom_target(deploy_statics ALL DEPENDS ${STATICS_DEPLOY_OUTPUTS})
        endif()
    endif()

    # Copy the Papyrus source folder (Source/Scripts/*.psc) into the deployed
    # mod folder. We enumerate the .psc files at configure time so the build
    # tracks them — adding a new .psc requires re-running CMake (or it'll be
    # picked up on the next configure). Each file uses copy_if_different so
    # touched files are skipped.
    set(PAPYRUS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/Source/Scripts")
    set(PAPYRUS_DEPLOY_DIR "${OUTPUT_FOLDER}/Source/Scripts")
    file(GLOB PAPYRUS_SOURCE_FILES CONFIGURE_DEPENDS "${PAPYRUS_SOURCE_DIR}/*.psc")
    if(PAPYRUS_SOURCE_FILES)
        message(STATUS "Papyrus source deploy folder: ${PAPYRUS_DEPLOY_DIR}")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${PAPYRUS_DEPLOY_DIR}"
            VERBATIM
        )
        foreach(psc IN LISTS PAPYRUS_SOURCE_FILES)
            get_filename_component(psc_name "${psc}" NAME)
            add_custom_command(
                TARGET "${PROJECT_NAME}"
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${psc}" "${PAPYRUS_DEPLOY_DIR}/${psc_name}"
                VERBATIM
            )
        endforeach()
    endif()
endif()
