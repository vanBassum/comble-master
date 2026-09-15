# ──────────────────────────────────────────────────────────────
# The built frontend, embedded in the application binary.
#
# Chain:  frontend/src/**  ->  (pnpm build)  ->  www/
#         www/             ->  (pack_web_assets.py)  ->  web_assets.bin
#         web_assets.bin   ->  (EMBED_FILES)  ->  rodata in the app image
#
# Ported from the KC1245 gateway. It replaces the FAT `www` partition Strux
# shipped with: the UI now travels inside the app image, which is what let the
# partition table drop `www` entirely and give those 896 KB to the OTA slots.
# That matters because LVGL alone cost ~330 KB and NimBLE is still to come.
#
# It is one blob rather than one EMBED_FILES entry per asset because vite
# content-hashes its filenames, and EMBED_FILES needs its list when CMake
# configures — long before vite has run. A single fixed name sidesteps that
# entirely, and the blob's directory is the manifest a fetch protocol needs.
#
# Deliberately its own component: the frontend is a separate deliverable with a
# build of its own, and nothing here reaches into main.
# ──────────────────────────────────────────────────────────────

idf_build_get_property(python PYTHON)

set(FRONTEND_DIR    "${CMAKE_CURRENT_LIST_DIR}/../../frontend")
set(WWW_DIR         "${CMAKE_CURRENT_LIST_DIR}/../../www")
set(PACK_SCRIPT     "${CMAKE_CURRENT_LIST_DIR}/pack_web_assets.py")
set(WEB_ASSETS_BIN  "${CMAKE_CURRENT_BINARY_DIR}/web_assets.bin")
set(FRONTEND_STAMP  "${CMAKE_CURRENT_BINARY_DIR}/frontend.stamp")

idf_component_register(
    SRCS "WebAssets.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "${WEB_ASSETS_BIN}")

# Everything below runs only in the second pass — idf_component_register returns
# early while ESP-IDF is still collecting requirements, and registering these
# rules twice would be an error.

# What a frontend rebuild depends on. CONFIGURE_DEPENDS re-runs the glob when
# ninja checks the build, so a newly added .tsx is picked up without a manual
# reconfigure. node_modules is not globbed: pnpm owns it, and walking it would
# cost more than the whole build.
file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS "${FRONTEND_DIR}/src/*")
list(APPEND FRONTEND_SOURCES
    "${FRONTEND_DIR}/index.html"
    "${FRONTEND_DIR}/package.json"
    "${FRONTEND_DIR}/vite.config.ts")

find_program(PNPM pnpm)
if(PNPM)
    # --no-frozen-lockfile lets a package.json edit update the lockfile rather
    # than failing a firmware build; --config.confirm-modules-purge=false skips
    # the prompt pnpm shows when node_modules is out of sync, which would
    # otherwise hang a build with nobody watching.
    add_custom_command(OUTPUT "${FRONTEND_STAMP}"
        COMMAND "${PNPM}" install --no-frozen-lockfile --config.confirm-modules-purge=false
        COMMAND "${PNPM}" build
        COMMAND "${CMAKE_COMMAND}" -E touch "${FRONTEND_STAMP}"
        WORKING_DIRECTORY "${FRONTEND_DIR}"
        DEPENDS ${FRONTEND_SOURCES}
        COMMENT "Building frontend (pnpm install && pnpm build)"
        VERBATIM USES_TERMINAL)
else()
    # The firmware must still build on a machine without node. The packer falls
    # back to a placeholder page, so the blob is always valid and no code
    # anywhere has to handle a missing one.
    message(STATUS "pnpm not found — the frontend will not be rebuilt")
    add_custom_command(OUTPUT "${FRONTEND_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${FRONTEND_STAMP}"
        VERBATIM)
endif()

# The packer's output is what EMBED_FILES names, so this rule is what teaches
# CMake how to produce it — the embed step depends on the file, this rule makes
# it, and the stamp in between forces the frontend to build first.
add_custom_command(OUTPUT "${WEB_ASSETS_BIN}"
    COMMAND "${python}" "${PACK_SCRIPT}" --src "${WWW_DIR}" --out "${WEB_ASSETS_BIN}"
    DEPENDS "${FRONTEND_STAMP}" "${PACK_SCRIPT}"
    COMMENT "Packing web assets into web_assets.bin"
    VERBATIM)
