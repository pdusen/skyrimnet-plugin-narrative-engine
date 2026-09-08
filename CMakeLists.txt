# It's recommended to set a minimum CMake version.
# If you use CMake features from higher versions, update this to match.
cmake_minimum_required(VERSION 3.24)

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

# ---------------------------------------------------------------------------
# Source layout: production, core, and co-located tests
# ---------------------------------------------------------------------------
#
# Four kinds of .cpp live under src/, and every one of them is classified by
# this block alone. Adding a file requires no edit here unless it belongs to
# the core list below.
#
#   src/Foo.cpp       production. Compiled into the SKSE plugin DLL, with
#                     PCH.h (RE/Skyrim.h + SKSE/SKSE.h) force-included, so it
#                     may touch the engine freely.
#
#   src/Foo.test.cpp  unit test. Compiled ONLY into the NarrativeEngineTests
#                     executable, never into the DLL — the EXCLUDE REGEX below
#                     is what keeps a co-located test out of the shipped
#                     plugin. Tests live beside the code they test on purpose;
#                     there is no tests/ tree.
#
#   core sources      production code listed in NARRATIVEENGINE_CORE_SOURCES.
#                     Compiled once into the NarrativeEngineCore static library
#                     and linked into BOTH the DLL and the test executable, so
#                     a test can exercise the real production object file
#                     rather than a recompiled copy.
#
#   src/Foo.engine.test.cpp
#                     unit test for engine-coupled code. Also matches the
#                     `*.test.cpp` glob, so the same exclusion keeps it out of
#                     the DLL, but it builds into a SECOND executable that
#                     compiles production sources against the real CommonLibSSE
#                     headers and links mocks in place of CommonLibSSE.lib. See
#                     the mocked-engine section at the bottom of this file.
#
# Membership in the core list is not a quality judgement — it is a hard
# technical constraint: a core source must compile and run with no Skyrim
# process, no SKSE, no CommonLibSSE, and no PCH. Anything that touches RE::,
# SKSE::, relocation addresses, hooks, or engine globals cannot go in the list.
# Such code is not untestable, though — it belongs to the mocked-engine target
# instead, which stands in for the engine rather than doing without it.
file(GLOB_RECURSE source_files CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS src/*.test.cpp)
file(GLOB_RECURSE engine_test_sources CONFIGURE_DEPENDS src/*.engine.test.cpp)

# `*.engine.test.cpp` also matches the `*.test.cpp` glob, so split them apart:
# whatever is left in test_sources is a pure-core test.
if(engine_test_sources)
    list(REMOVE_ITEM test_sources ${engine_test_sources})
endif()

# Engine-free production sources shared by the DLL and the unit tests.
#
# SenderCooldownTable qualifies because its two engine dependencies were pushed
# out to its call boundaries: the clock is a `nowGameHours` parameter, and the
# co-save arrives as an `ICosaveIO&` that the DLL binds to SKSE via
# src/SKSECosaveIO.cpp (which stays out of this list, being the adapter).
set(NARRATIVEENGINE_CORE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/BookTextRenderer.cpp"
    "${CMAKE_SOURCE_DIR}/src/JsonUtils.cpp"
    "${CMAKE_SOURCE_DIR}/src/LLMTextSanitizer.cpp"
    "${CMAKE_SOURCE_DIR}/src/SenderCooldownTable.cpp"
    "${CMAKE_SOURCE_DIR}/src/SkyrimNetEvents.cpp"
    "${CMAKE_SOURCE_DIR}/src/ThreadRole.cpp"
)

# Everything the plugin DLL compiles directly: all of src/ minus the tests
# (which belong to the test executable) and minus the core sources (which
# arrive via the NarrativeEngineCore link instead).
list(FILTER source_files EXCLUDE REGEX "\\.test\\.cpp$")
list(REMOVE_ITEM source_files ${NARRATIVEENGINE_CORE_SOURCES})

# REMOVE_ITEM matches strings, not files, so a core entry written in a
# different path form than the glob produces would silently stay in the DLL's
# list and get compiled twice. Fail the configure instead of shipping that.
foreach(coreSource IN LISTS NARRATIVEENGINE_CORE_SOURCES)
    if(NOT EXISTS "${coreSource}")
        message(
            FATAL_ERROR
            "NARRATIVEENGINE_CORE_SOURCES names a file that does not exist: ${coreSource}"
        )
    endif()
    if(coreSource IN_LIST source_files)
        message(
            FATAL_ERROR
            "Core source '${coreSource}' was not removed from the plugin source list. "
            "Write it as \${CMAKE_SOURCE_DIR}/src/<name>.cpp so it matches the glob."
        )
    endif()
endforeach()

find_package(directxtk CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# Setup your SKSE plugin as an SKSE plugin!
find_package(CommonLibSSE CONFIG REQUIRED)
# MinHook — battle-tested function-entry detour library. Used by the
# LetterPool hooks (TESDescription::GetDescription, BookMenu::OpenBookMenu);
# we'd otherwise have to hand-roll prologue disassembly + trampoline
# construction.
find_package(minhook CONFIG REQUIRED)
# simpleini ships as a header-only library — no CMake config, just an include path.
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)
# ---------------------------------------------------------------------------
# Warnings are errors
# ---------------------------------------------------------------------------
#
# An option rather than a hard-coded ON, because -Werror / /WX bakes THIS
# toolchain's warning set into everyone's build. A newer MSVC that adds a
# warning, or a different compiler entirely, then fails to compile code that
# is perfectly fine — and the person it fails for is usually someone who just
# wants to build the mod, not someone in a position to fix our diagnostics.
# Defaulting ON keeps our own builds honest; -DNARRATIVEENGINE_WARNINGS_AS_ERRORS=OFF
# is the way out, and CMake's own `--compile-no-warning-as-error` turns it off
# for a single build without reconfiguring.
#
# CMAKE_COMPILE_WARNING_AS_ERROR is read when a target is CREATED, so it has
# to be set before add_commonlibsse_plugin below. It needs CMake 3.24, which
# is why the minimum at the top of this file is 3.24 — on an older CMake the
# variable is silently ignored, which would look enabled while doing nothing.
#
# The compiler-agnostic property rather than a bare /WX so the flag stays
# correct if this ever builds under anything but MSVC. Warning LEVEL is not
# set here: add_commonlibsse_plugin already applies /W4 plus CommonLibSSE's
# own suppression list, and that is the level these errors are judged at.
option(
    NARRATIVEENGINE_WARNINGS_AS_ERRORS
    "Treat compiler warnings as errors"
    ON
)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${NARRATIVEENGINE_WARNINGS_AS_ERRORS})

# ---------------------------------------------------------------------------
# NarrativeEngineCore — engine-free production code, shared with the tests
# ---------------------------------------------------------------------------
#
# A plain static library so the sources in NARRATIVEENGINE_CORE_SOURCES are
# compiled once and linked into both the SKSE plugin DLL and the unit-test
# executable. Without it there is nothing for a test to link against: every
# production object file would live inside the DLL target, and a test
# executable would have to recompile the .cpp itself — which tests a copy of
# the code rather than the artifact that ships.
#
# Deliberately minimal. It gets `include/` and C++23 and nothing else: no PCH
# (PCH.h pulls in RE/Skyrim.h), no CommonLibSSE, no SkyrimNet or PrismaUI
# headers. That is what makes the library linkable into a console executable
# that never loads the game, and the constraint is enforced by the compiler —
# a `RE::` reference in a core source simply fails to build here.
add_library(NarrativeEngineCore STATIC ${NARRATIVEENGINE_CORE_SOURCES})
target_compile_features(NarrativeEngineCore PUBLIC cxx_std_23)
target_include_directories(NarrativeEngineCore PUBLIC include)
# nlohmann_json is header-only and carries nothing engine-specific, so a core
# source may parse JSON without breaking the "runs with no Skyrim" rule.
target_link_libraries(NarrativeEngineCore PUBLIC nlohmann_json::nlohmann_json)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${source_files} plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h) # <--- PCH.h is required!
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE include "${SIMPLEINI_INCLUDE_DIRS}"
)
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE nlohmann_json::nlohmann_json minhook::minhook NarrativeEngineCore
)

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
elseif(
    DEFINED ENV{SKYRIM_MODS_FOLDER}
    AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet/CppAPI"
)
    set(SKYRIMNET_DIR "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet")
endif()

if(NOT DEFINED SKYRIMNET_DIR)
    message(
        FATAL_ERROR
        "Could not locate SkyrimNet. Set the SKYRIMNET_DIR environment variable to "
        "the SkyrimNet mod folder (containing CppAPI/PublicAPI.h), or set "
        "SKYRIM_MODS_FOLDER so SkyrimNet is found at \${SKYRIM_MODS_FOLDER}/SkyrimNet."
    )
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
if(
    DEFINED ENV{PRISMA_UI_INCLUDE}
    AND EXISTS "$ENV{PRISMA_UI_INCLUDE}/PrismaUI_API.h"
)
    set(PRISMA_UI_INCLUDE "$ENV{PRISMA_UI_INCLUDE}")
endif()

if(NOT DEFINED PRISMA_UI_INCLUDE)
    message(
        FATAL_ERROR
        "Could not locate the PrismaUI API header. Set the PRISMA_UI_INCLUDE "
        "environment variable to the directory containing PrismaUI_API.h."
    )
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
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    # If you perform a "Debug" build, also copy .pdb file (for debug symbols)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
                "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
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
        file(
            GLOB_RECURSE STATICS_FILES
            CONFIGURE_DEPENDS
            RELATIVE "${STATICS_SOURCE_DIR}"
            "${STATICS_SOURCE_DIR}/*"
        )
        if(STATICS_FILES)
            message(
                STATUS
                "Statics deploy: ${STATICS_SOURCE_DIR} -> ${OUTPUT_FOLDER}"
            )
            set(STATICS_DEPLOY_OUTPUTS "")
            foreach(rel IN LISTS STATICS_FILES)
                set(src "${STATICS_SOURCE_DIR}/${rel}")
                set(dst "${OUTPUT_FOLDER}/${rel}")
                get_filename_component(dst_dir "${dst}" DIRECTORY)
                add_custom_command(
                    OUTPUT "${dst}"
                    DEPENDS "${src}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dst_dir}"
                    COMMAND
                        "${CMAKE_COMMAND}" -E copy_if_different "${src}"
                        "${dst}"
                    VERBATIM
                )
                list(APPEND STATICS_DEPLOY_OUTPUTS "${dst}")
            endforeach()
            add_custom_target(
                deploy_statics
                ALL
                DEPENDS ${STATICS_DEPLOY_OUTPUTS}
            )
        endif()
    endif()

    # ---------------------------------------------------------------------
    # Dashboard (TypeScript + React + Rollup) build & deploy
    # ---------------------------------------------------------------------
    #
    # The dashboard JS bundle is part of the unified build. Editing any
    # `dashboard/` source triggers a Rollup re-bundle on the next
    # `pwsh -File build.ps1 build` and the dist outputs deploy into the mod
    # folder's PrismaUI views path alongside the DLL and statics. CMake
    # doesn't run Rollup itself — it shells out to npm — but tracks the
    # input/output graph so Ninja skips unchanged work in both directions
    # (touch only .tsx → only the bundle rebuilds; touch only .cpp → only
    # the DLL relinks).
    #
    # Required: `npm` on PATH (Node.js install). Fatal if missing — the
    # dashboard is part of the build, not an optional add-on.
    find_program(NPM_EXECUTABLE npm REQUIRED)

    set(DASHBOARD_DIR "${CMAKE_SOURCE_DIR}/dashboard")
    set(DASHBOARD_DIST_DIR "${DASHBOARD_DIR}/dist")
    set(DASHBOARD_INSTALL_STAMP
        "${DASHBOARD_DIR}/node_modules/.cmake-install-stamp"
    )
    set(DASHBOARD_DEPLOY_DIR
        "${OUTPUT_FOLDER}/PrismaUI/views/NarrativeEngine/dashboard"
    )

    # 1. `npm install` — gated on package*.json so the slow install only
    # runs when dependencies actually change. The stamp file is what Ninja
    # tracks; `cmake -E touch` writes it after a successful install.
    file(
        GLOB DASHBOARD_NPM_MANIFEST
        CONFIGURE_DEPENDS
        "${DASHBOARD_DIR}/package.json"
        "${DASHBOARD_DIR}/package-lock.json"
    )
    add_custom_command(
        OUTPUT "${DASHBOARD_INSTALL_STAMP}"
        DEPENDS ${DASHBOARD_NPM_MANIFEST}
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory "${DASHBOARD_DIR}/node_modules"
        COMMAND "${NPM_EXECUTABLE}" install --no-audit --no-fund --silent
        COMMAND "${CMAKE_COMMAND}" -E touch "${DASHBOARD_INSTALL_STAMP}"
        WORKING_DIRECTORY "${DASHBOARD_DIR}"
        COMMENT "Installing dashboard npm dependencies"
        VERBATIM
    )

    # 2. Rollup bundle build — depends on every .ts/.tsx source plus the
    # static shell, the configs, and the install stamp.
    file(
        GLOB_RECURSE DASHBOARD_SOURCES
        CONFIGURE_DEPENDS
        "${DASHBOARD_DIR}/src/*.ts"
        "${DASHBOARD_DIR}/src/*.tsx"
    )
    set(DASHBOARD_BUILD_INPUTS
        ${DASHBOARD_SOURCES}
        "${DASHBOARD_DIR}/index.html"
        "${DASHBOARD_DIR}/styles.css"
        "${DASHBOARD_DIR}/tsconfig.json"
        "${DASHBOARD_DIR}/rollup.config.mjs"
        "${DASHBOARD_INSTALL_STAMP}"
    )
    set(DASHBOARD_BUILD_OUTPUTS
        "${DASHBOARD_DIST_DIR}/dashboard.js"
        "${DASHBOARD_DIST_DIR}/index.html"
        "${DASHBOARD_DIST_DIR}/dashboard.css"
    )
    add_custom_command(
        OUTPUT ${DASHBOARD_BUILD_OUTPUTS}
        DEPENDS ${DASHBOARD_BUILD_INPUTS}
        COMMAND "${NPM_EXECUTABLE}" run build --silent
        WORKING_DIRECTORY "${DASHBOARD_DIR}"
        COMMENT "Bundling dashboard via Rollup"
        VERBATIM
    )

    # 3. Deploy each dist file into the PrismaUI views path.
    set(DASHBOARD_DEPLOY_OUTPUTS "")
    foreach(distFile IN LISTS DASHBOARD_BUILD_OUTPUTS)
        get_filename_component(distName "${distFile}" NAME)
        set(deployed "${DASHBOARD_DEPLOY_DIR}/${distName}")
        add_custom_command(
            OUTPUT "${deployed}"
            DEPENDS "${distFile}"
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory "${DASHBOARD_DEPLOY_DIR}"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different "${distFile}"
                "${deployed}"
            VERBATIM
        )
        list(APPEND DASHBOARD_DEPLOY_OUTPUTS "${deployed}")
    endforeach()

    # 4. ALL-target aggregator so a default `cmake --build` builds the
    # dashboard alongside the C++ DLL.
    add_custom_target(deploy_dashboard ALL DEPENDS ${DASHBOARD_DEPLOY_OUTPUTS})
    message(
        STATUS
        "Dashboard deploy: ${DASHBOARD_DIST_DIR} -> ${DASHBOARD_DEPLOY_DIR}"
    )

    # -------------------------------------------------------------------
    # ESP sync (bidirectional, via sync-esp.ps1)
    # -------------------------------------------------------------------
    #
    # Pre-build target that runs `sync-esp.ps1` on every build. The script
    # syncs <mod-folder>/NarrativeEngine.esp against the Spriggit-serialized
    # YAML tree at <repo>/esp/plugin/, using content hashes recorded in
    # <repo>/esp/.sync-state.json (machine-local, gitignored) to detect which
    # side has changed since the last sync. CK edits in the mod folder
    # propagate into the repo via `spriggit serialize`; pulled repo changes
    # propagate out to the mod folder via `spriggit deserialize`. See
    # sync-esp.ps1 for the full decision matrix.
    #
    # Wired as an ALL custom target plus add_dependencies so it runs before
    # the DLL link step. Uses pwsh from PATH; if you want a different
    # PowerShell, override via NE_PWSH_EXECUTABLE.
    #
    # `add_custom_command(... PRE_BUILD ...)` is NOT used here because the
    # PRE_BUILD timing only fires for the Visual Studio generator, not
    # Ninja; our setup is Ninja.
    option(NE_SKIP_ESP_SYNC "Skip the bidirectional ESP sync on each build" OFF)
    if(NOT NE_SKIP_ESP_SYNC)
        if(NOT DEFINED NE_PWSH_EXECUTABLE)
            find_program(NE_PWSH_EXECUTABLE pwsh REQUIRED)
        endif()
        add_custom_target(
            sync_esp
            ALL
            COMMAND
                "${NE_PWSH_EXECUTABLE}" -NoProfile -File
                "${CMAKE_SOURCE_DIR}/sync-esp.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT
                "Syncing NarrativeEngine.esp from mod folder to repo (if newer)"
            VERBATIM
        )
        add_dependencies("${PROJECT_NAME}" sync_esp)
        message(STATUS "ESP sync: enabled (sync_esp pre-build target wired)")
    else()
        message(STATUS "ESP sync: SKIPPED (NE_SKIP_ESP_SYNC=ON)")
    endif()

    # -------------------------------------------------------------------
    # Papyrus compile (esp/Source/Scripts/*.psc -> <mod-folder>/Scripts/*.pex)
    # -------------------------------------------------------------------
    #
    # Conditional on .psc files existing. Until the user authors the first
    # script (Step 7), this whole block is dormant. Once a .psc appears,
    # CONFIGURE_DEPENDS picks it up on the next build, this block activates,
    # and Bethesda's PapyrusCompiler.exe is invoked against the project's
    # generated .ppj file (configured below from NarrativeEngine.ppj.in).
    file(
        GLOB PAPYRUS_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/esp/Source/Scripts/*.psc"
    )
    if(PAPYRUS_SOURCES)
        # Detect PapyrusCompiler.exe. Required once .psc files exist.
        if(NOT DEFINED NE_PAPYRUS_COMPILER)
            if(
                DEFINED ENV{PAPYRUS_COMPILER}
                AND EXISTS "$ENV{PAPYRUS_COMPILER}"
            )
                set(NE_PAPYRUS_COMPILER "$ENV{PAPYRUS_COMPILER}")
            endif()
        endif()
        if(NOT NE_PAPYRUS_COMPILER)
            message(
                FATAL_ERROR
                "Papyrus sources found under esp/Source/Scripts/ but PAPYRUS_COMPILER "
                "is not set. Point the PAPYRUS_COMPILER environment variable at your "
                "PapyrusCompiler.exe (typically <CK_DIR>/Papyrus Compiler/PapyrusCompiler.exe)."
            )
        endif()

        # Default vanilla / SKSE Papyrus source import paths. Override either
        # via NE_PAPYRUS_IMPORT_SKYRIM / NE_PAPYRUS_IMPORT_SKSE env vars; the
        # detection below picks the common defaults relative to SKYRIM_FOLDER
        # and SKSE_DIR if present.
        if(NOT DEFINED NE_PAPYRUS_IMPORT_SKYRIM)
            if(DEFINED ENV{NE_PAPYRUS_IMPORT_SKYRIM})
                set(NE_PAPYRUS_IMPORT_SKYRIM "$ENV{NE_PAPYRUS_IMPORT_SKYRIM}")
            elseif(
                DEFINED ENV{SKYRIM_FOLDER}
                AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data/Source/Scripts"
            )
                set(NE_PAPYRUS_IMPORT_SKYRIM
                    "$ENV{SKYRIM_FOLDER}/Data/Source/Scripts"
                )
            endif()
        endif()
        if(NOT DEFINED NE_PAPYRUS_IMPORT_SKSE)
            if(DEFINED ENV{NE_PAPYRUS_IMPORT_SKSE})
                set(NE_PAPYRUS_IMPORT_SKSE "$ENV{NE_PAPYRUS_IMPORT_SKSE}")
            endif()
        endif()
        if(NOT NE_PAPYRUS_IMPORT_SKYRIM OR NOT NE_PAPYRUS_IMPORT_SKSE)
            message(
                FATAL_ERROR
                "Papyrus sources found but vanilla / SKSE import paths aren't set. "
                "Set NE_PAPYRUS_IMPORT_SKYRIM and NE_PAPYRUS_IMPORT_SKSE env vars "
                "(or define them as CMake cache vars). Vanilla is typically "
                "<SkyrimSE>/Data/Source/Scripts; SKSE source ships in the SKSE archive."
            )
        endif()

        # Generate NarrativeEngine.ppj at the repo root from the template,
        # substituting the absolute machine-specific paths. The generated
        # .ppj is gitignored; VS Code's Papyrus extension picks it up from
        # the repo root automatically.
        set(NE_PAPYRUS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/esp/Source/Scripts")
        set(NE_PAPYRUS_OUTPUT_DIR "${OUTPUT_FOLDER}/Scripts")
        # SkyrimNet Papyrus API (SkyrimNetApi.psc etc.) is exposed via a
        # junction created by setup-mod-folder.ps1 that points at the
        # SkyrimNet mod folder's Source/Scripts directory.
        set(NE_PAPYRUS_IMPORT_SKYRIMNET
            "${CMAKE_SOURCE_DIR}/external/SkyrimNet/Source/Scripts"
        )
        # MCM SDK (MCM_ConfigBase.psc + bundled SKI_ConfigBase.psc /
        # SKI_QuestBase.psc) copied by setup-mod-folder.ps1 from the MCM
        # SDK mod folder. Needed at compile time by _ne_MCM.psc, which
        # extends MCM_ConfigBase.
        set(NE_PAPYRUS_IMPORT_MCM
            "${CMAKE_SOURCE_DIR}/external/MCM/Source/Scripts"
        )
        configure_file(
            "${CMAKE_SOURCE_DIR}/NarrativeEngine.ppj.in"
            "${CMAKE_SOURCE_DIR}/NarrativeEngine.ppj"
            @ONLY
        )

        # Compile target — Bethesda's stock PapyrusCompiler.exe doesn't parse
        # .ppj files (that's a Caprica / VS Code extension convention; the
        # stock compiler treats any non-script argument as a script name and
        # bails with "unable to locate script"). Invoke it via its native
        # CLI: <folder> -all -f=<flags> -i=<imports> -o=<output>, derived from
        # the same variables that get baked into the .ppj for IDE use.
        # Imports list the project's own source folder first so cross-script
        # references resolve before any vanilla / SKSE fallbacks.
        add_custom_target(
            compile_papyrus
            ALL
            COMMAND
                "${NE_PAPYRUS_COMPILER}" "${NE_PAPYRUS_SOURCE_DIR}" -all
                "-f=TESV_Papyrus_Flags.flg"
                # SKSE's Papyrus scripts extend vanilla ones (e.g. Form.psc
                # adds SendModEvent); they must come BEFORE the vanilla
                # import so the compiler resolves the augmented versions.
                "-i=${NE_PAPYRUS_SOURCE_DIR};${NE_PAPYRUS_IMPORT_SKYRIMNET};${NE_PAPYRUS_IMPORT_MCM};${NE_PAPYRUS_IMPORT_SKSE};${NE_PAPYRUS_IMPORT_SKYRIM}"
                "-o=${NE_PAPYRUS_OUTPUT_DIR}" -op
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS ${PAPYRUS_SOURCES}
            COMMENT "Compiling Papyrus scripts to ${NE_PAPYRUS_OUTPUT_DIR}"
            VERBATIM
        )
        message(
            STATUS
            "Papyrus compile: ${NE_PAPYRUS_SOURCE_DIR} -> ${NE_PAPYRUS_OUTPUT_DIR}"
        )
    else()
        message(STATUS "Papyrus compile: no .psc files yet; step dormant")
    endif()
endif()

# ---------------------------------------------------------------------------
# Unit tests (Catch2 v3 + CTest)
# ---------------------------------------------------------------------------
#
# Tests are co-located: `src/Foo.test.cpp` sits beside `src/Foo.cpp`, and the
# glob at the top of this file is what routes it here instead of into the
# plugin DLL. There is no tests/ directory and there should not be one.
#
# The test executable links NarrativeEngineCore and NOT CommonLibSSE, so it
# runs as an ordinary x64 console program with no Skyrim, no SKSE load order,
# and no relocation database. That boundary is the point: whatever a test can
# reach is by construction code that works outside the game.
#
# Catch2 comes from vcpkg (see vcpkg.json) — the same manifest that already
# provides CommonLibSSE, nlohmann_json and the rest — rather than FetchContent,
# so there is one dependency mechanism in this project and not two.
option(NARRATIVEENGINE_BUILD_TESTS "Build the Catch2 unit tests" ON)

if(NARRATIVEENGINE_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    enable_testing()

    # Catch.cmake (which defines catch_discover_tests) ships beside the
    # package config rather than on the module path, so point CMake at it.
    list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    include(Catch)

    if(NOT test_sources)
        message(
            FATAL_ERROR
            "NARRATIVEENGINE_BUILD_TESTS is ON but no src/*.test.cpp files were found."
        )
    endif()

    add_executable(NarrativeEngineTests ${test_sources})
    target_compile_features(NarrativeEngineTests PRIVATE cxx_std_23)
    target_link_libraries(
        NarrativeEngineTests
        PRIVATE NarrativeEngineCore Catch2::Catch2WithMain
    )

    # Register each TEST_CASE with CTest individually, so `ctest -R` can
    # select one and a failure names the case rather than "the test binary".
    catch_discover_tests(NarrativeEngineTests)

    list(LENGTH test_sources NARRATIVEENGINE_TEST_SOURCE_COUNT)
    message(
        STATUS
        "Unit tests: enabled (${NARRATIVEENGINE_TEST_SOURCE_COUNT} co-located *.test.cpp file(s))"
    )

    # -----------------------------------------------------------------------
    # Mocked-engine tests
    # -----------------------------------------------------------------------
    #
    # A second executable, for production code that genuinely calls
    # CommonLibSSE and should not be reshaped to avoid it.
    #
    # The trick is that CommonLibSSE is mostly headers. Struct layouts, member
    # offsets and inline helpers compile straight into our object files; only
    # some functions are out-of-line, and those live in CommonLibSSE.lib where
    # each one resolves an address inside a running SkyrimSE.exe. Those are the
    # functions a test process cannot execute -- and they are exactly the ones
    # the linker will take from somewhere else if somewhere else defines them.
    #
    # So this target compiles the production sources named in
    # NARRATIVEENGINE_MOCKED_SOURCES against the REAL CommonLibSSE headers,
    # with the same defines and the same PCH as the DLL, and then simply does
    # NOT link CommonLibSSE.lib. testsupport/EngineMock.cpp supplies the engine
    # functions instead. Production code is compiled unmodified and still calls
    # `RE::Calendar::GetSingleton()`; it just reaches our definition.
    #
    # Consequences worth knowing:
    #
    #   * Any engine function the code under test touches that nobody has
    #     mocked is an unresolved external, so the build fails by name. There
    #     is no silent fallthrough into real engine code.
    #   * Sources listed here are compiled TWICE -- once into the DLL, once
    #     here -- because the two need different link closures. That is the
    #     cost of not rewriting production code, and it is the right trade.
    #   * spdlog and fmt are linked for real: they are ordinary libraries with
    #     nothing Skyrim-specific in them, and logging calls in the code under
    #     test should behave.
    set(NARRATIVEENGINE_MOCKED_SOURCES
        "${CMAKE_SOURCE_DIR}/src/DecisionLog.cpp"
        "${CMAKE_SOURCE_DIR}/src/EngineUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/PhaseTracker.cpp"
        "${CMAKE_SOURCE_DIR}/src/Region.cpp"
        "${CMAKE_SOURCE_DIR}/src/QuestUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/Settings.cpp"
        "${CMAKE_SOURCE_DIR}/src/AlphaCanon.cpp"
        "${CMAKE_SOURCE_DIR}/src/AsyncDispatch.cpp"
        "${CMAKE_SOURCE_DIR}/src/BeatWorkDispatch.cpp"
        "${CMAKE_SOURCE_DIR}/src/BeatParamHelpers.cpp"
        "${CMAKE_SOURCE_DIR}/src/BeatRegistry.cpp"
        "${CMAKE_SOURCE_DIR}/src/ConsoleCommand.cpp"
        "${CMAKE_SOURCE_DIR}/src/CourierUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/Decorators.cpp"
        "${CMAKE_SOURCE_DIR}/src/EvalDispatch.cpp"
        "${CMAKE_SOURCE_DIR}/src/PrismaUI.cpp"
        "${CMAKE_SOURCE_DIR}/src/SkyrimNetAPI.cpp"
        "${CMAKE_SOURCE_DIR}/src/GossipDispatch.cpp"
        "${CMAKE_SOURCE_DIR}/src/EventLogUtil.cpp"
        "${CMAKE_SOURCE_DIR}/src/FactionDesignationUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/LocationKeywords.cpp"
        "${CMAKE_SOURCE_DIR}/src/MainThread.cpp"
        "${CMAKE_SOURCE_DIR}/src/MainThreadEngine.cpp"
        "${CMAKE_SOURCE_DIR}/src/MCMEventSink.cpp"
        "${CMAKE_SOURCE_DIR}/src/SKSECosaveIO.cpp"
    )

    if(engine_test_sources)
        find_package(spdlog CONFIG REQUIRED)

        add_executable(
            NarrativeEngineEngineMockTests
            ${engine_test_sources}
            ${NARRATIVEENGINE_MOCKED_SOURCES}
            testsupport/EngineMock.cpp
            testsupport/CommonLibSSERuntimeStubs.cpp
            testsupport/DispatcherContract.cpp
            testsupport/FakeVTable.cpp
            testsupport/RelocationMocks.cpp
            testsupport/RelocationMocks.test.cpp
        )
        target_compile_features(
            NarrativeEngineEngineMockTests
            PRIVATE cxx_std_23
        )
        target_include_directories(
            NarrativeEngineEngineMockTests
            # PRISMA_UI_INCLUDE because src/PrismaUI.cpp includes the upstream
            # header; the wrapper is compiled here exactly as it is in the DLL.
            # PRISMA_UI_INCLUDE and SKYRIMNET_DIR because src/PrismaUI.cpp and
            # src/SkyrimNetAPI.cpp include their upstream headers; both wrappers
            # are compiled here exactly as they are in the DLL.
            PRIVATE
                include
                testsupport
                "${PRISMA_UI_INCLUDE}"
                "${SKYRIMNET_DIR}/CppAPI"
        )
        # CommonLibSSE's include dirs and compile definitions WITHOUT its
        # library: the headers are what we want, the .lib is what we replace.
        target_include_directories(
            NarrativeEngineEngineMockTests
            SYSTEM
            PRIVATE
                $<TARGET_PROPERTY:CommonLibSSE::CommonLibSSE,INTERFACE_INCLUDE_DIRECTORIES>
        )
        target_compile_definitions(
            NarrativeEngineEngineMockTests
            PRIVATE
                $<TARGET_PROPERTY:CommonLibSSE::CommonLibSSE,INTERFACE_COMPILE_DEFINITIONS>
                # Unlocks REL::Module::mock(), CommonLibSSE-NG's own hook for
                # running its headers with no Skyrim process behind them.
                ENABLE_COMMONLIBSSE_TESTING
        )
        # Same forced PCH as the DLL, so these sources compile exactly as they
        # do in production rather than in some near-miss configuration.
        target_precompile_headers(NarrativeEngineEngineMockTests PRIVATE PCH.h)
        # NarrativeEngineCore is engine-free, so linking it here costs nothing
        # and lets a mocked module use a core one (ThreadRole, JsonUtils, ...)
        # without listing it as a mocked source and compiling it a third time.
        target_link_libraries(
            NarrativeEngineEngineMockTests
            PRIVATE Catch2::Catch2WithMain spdlog::spdlog NarrativeEngineCore
        )

        # A stand-in PrismaUI.dll, built under the real name and dropped beside
        # the test executable. PrismaUI is found through GetModuleHandle and
        # GetProcAddress with no seam in between, so the only faithful way to
        # exercise the "PrismaUI is installed" half of the wrapper is to put a
        # DLL of that name where the loader will find it.
        add_library(
            PrismaUIFake
            SHARED
            "${CMAKE_SOURCE_DIR}/testsupport/FakePrismaUI.cpp"
        )
        target_compile_features(PrismaUIFake PRIVATE cxx_std_23)
        target_include_directories(
            PrismaUIFake
            PRIVATE testsupport "${PRISMA_UI_INCLUDE}"
        )
        set_target_properties(
            PrismaUIFake
            PROPERTIES
                OUTPUT_NAME PrismaUI
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        add_dependencies(NarrativeEngineEngineMockTests PrismaUIFake)

        # The same trick for SkyrimNet, whose header resolves itself with
        # LoadLibraryA("SkyrimNet") and a GetProcAddress per export. Building a
        # DLL of that name also reaches the version gate: the header only
        # resolves an export when the version the DLL reports is high enough,
        # so a fake reporting an old version is a genuinely older install.
        add_library(
            SkyrimNetFake
            SHARED
            "${CMAKE_SOURCE_DIR}/testsupport/FakeSkyrimNet.cpp"
        )
        target_compile_features(SkyrimNetFake PRIVATE cxx_std_23)
        target_include_directories(SkyrimNetFake PRIVATE testsupport)
        # C4190: an extern "C" function returning std::string. That is what the
        # real SkyrimNet exports do -- the API is C-linkage for undecorated
        # GetProcAddress names but C++ by value across the boundary, which works
        # only because both modules share a dynamically-linked CRT. The fake has
        # to match the real signatures exactly, so the warning is suppressed
        # here and nowhere else.
        target_compile_options(SkyrimNetFake PRIVATE /wd4190)
        set_target_properties(
            SkyrimNetFake
            PROPERTIES
                OUTPUT_NAME SkyrimNet
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        add_dependencies(NarrativeEngineEngineMockTests SkyrimNetFake)

        catch_discover_tests(NarrativeEngineEngineMockTests)

        list(LENGTH engine_test_sources NARRATIVEENGINE_ENGINE_TEST_COUNT)
        message(
            STATUS
            "Mocked-engine tests: enabled (${NARRATIVEENGINE_ENGINE_TEST_COUNT} *.engine.test.cpp file(s))"
        )
    else()
        message(STATUS "Mocked-engine tests: no *.engine.test.cpp files yet")
    endif()
else()
    message(STATUS "Unit tests: SKIPPED (NARRATIVEENGINE_BUILD_TESTS=OFF)")
endif()
