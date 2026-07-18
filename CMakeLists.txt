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
# MinHook — battle-tested function-entry detour library. Used by the
# LetterPool hooks (TESDescription::GetDescription, BookMenu::OpenBookMenu);
# we'd otherwise have to hand-roll prologue disassembly + trampoline
# construction.
find_package(minhook CONFIG REQUIRED)
# simpleini ships as a header-only library — no CMake config, just an include path.
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${source_files} plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h) # <--- PCH.h is required!
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE include "${SIMPLEINI_INCLUDE_DIRS}"
)
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE nlohmann_json::nlohmann_json minhook::minhook
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
    # compares <mod-folder>/NarrativeEngine.esp against
    # <repo>/esp/NarrativeEngine.esp and copies whichever is newer over the
    # older. CK edits in the mod folder propagate into the repo; pulled
    # repo changes propagate out to the mod folder. See sync-esp.ps1 for
    # the full decision matrix.
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
