# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Derive version from git tag (v0.1.0 -> 0.1.0), fallback to 0.0.0-dev
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v(.+)$")
    set(PROJECT_VER "${CMAKE_MATCH_1}")
else()
    set(PROJECT_VER "0.0.0-dev")
endif()

# ── Per-board sdkconfig ───────────────────────────────────────
# Common defaults plus an optional per-board overlay living in the board
# folder (main/hardware/boards/<board>/sdkconfig.defaults), letting a board
# change flash size / PSRAM / partitions without touching the shared
# defaults. sdkconfig fragments cannot include one another, so this list is
# the composition point: the board file overlays the common one and wins on
# conflicts. BOARD is the same cache var consumed by main/CMakeLists.txt.
if(NOT DEFINED BOARD)
    set(BOARD "wt_sc01_plus")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/main/hardware/boards/${BOARD}/sdkconfig.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "main/hardware/boards/${BOARD}/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(CombleMaster)

# ── sdkconfig drift guard ────────────────────────────
# A generated sdkconfig is loaded AFTER the defaults and wins every conflict, and an
# option sitting at its default is still recorded there — as "# CONFIG_FOO is not set"
# — so adding CONFIG_FOO=y to sdkconfig.defaults changes NOTHING in a tree that already
# has a sdkconfig. It fails silently: the build succeeds and runs the old configuration,
# which is how a config change can look landed and do nothing. The CONFIG_IDF_TARGET
# case in main/CMakeLists.txt is this same mechanism, and the reason a wrong-toolchain
# build is possible at all.
#
# So every assertion in the composed defaults is checked against what was actually
# generated, and a mismatch is a build error naming the options that did not take. The
# fix it prints is the only fix: delete the generated file — it is gitignored and
# reproducible — and let set-target write a new one.
#
# For a deliberate local override of something the defaults assert:
#   idf.py -DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON build
#
# Only lines of the form CONFIG_X=... are checked, which is every line either defaults
# file uses. A default whose *value* contained a semicolon would be misread, because
# file(STRINGS) hands back a CMake list; none do, and if one ever does it is this loop
# that needs fixing, not the value.
#
# ── What a board overlay is allowed to do to a root assertion ──
# Two cases, and only the second needs a mechanism.
#
# SAME KEY, different value — a board setting
# CONFIG_PARTITION_TABLE_CUSTOM_FILENAME to its own table. The overlay wins in the
# generated file, so the root's line is simply not an assertion any more, and the
# check below honours that: it reduces the composed files to one line per key, last
# writer wins, and tests only those. (It did not always. It checked every file's line
# independently, and the first board to override a root VALUE — the SC01 Plus, with its
# own partition table — failed a tree that was configured exactly as asked. The guard
# was wrong, not the board.)
#
# DIFFERENT KEY, same meaning — a Kconfig *choice*, where picking another arm means
# setting a different symbol. A board asking for CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y does
# not shadow the root's CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y, it falsifies it, and no
# amount of last-writer-wins can see the connection. So a file may retract a key it is
# leaving behind:
#     # STRUX_DRIFT_EXEMPT: CONFIG_ESPTOOLPY_FLASHSIZE_4MB
# The key is then checked in none of the composed files. Retractions are collected
# before a single assertion is tested, so a board can retract a key the root asserts
# even though the root is read first.
#
# The hole is meant to stay narrow: it silences the guard for one named key. Retract
# the arm you are LEAVING — never the key you are actually setting, which still has to
# show up in the generated file or the guard has stopped guarding anything.
if(NOT STRUX_ALLOW_SDKCONFIG_DRIFT)
    idf_build_get_property(_strux_sdkconfig SDKCONFIG)
    file(READ "${_strux_sdkconfig}" _strux_actual)
    string(REPLACE "\r" "" _strux_actual "${_strux_actual}")  # generated file is CRLF
    set(_strux_actual "\n${_strux_actual}\n")              # so every line has both

    # ── Pass one: reduce the composed defaults to what they actually ASSERT ──
    # A key asserted in several files is only asserted once, by the LAST file to
    # mention it — that is what composition means, and it is the whole point of a
    # board overlay. Checking each file's line independently would fail a tree
    # that is configured exactly as intended, which is how the SC01 Plus's
    # partition table first broke this guard.
    #
    # Each key is kept in a variable named after itself, so later files simply
    # overwrite earlier ones and the last writer wins.
    set(_strux_keys "")
    set(_strux_exempt "")
    foreach(_strux_file ${SDKCONFIG_DEFAULTS})
        # project.cmake has already made these absolute; BASE_DIR keeps this working
        # either way, since the list is composed with relative paths above.
        get_filename_component(_strux_path "${_strux_file}" ABSOLUTE
                               BASE_DIR "${CMAKE_SOURCE_DIR}")
        file(RELATIVE_PATH _strux_shown "${CMAKE_SOURCE_DIR}" "${_strux_path}")
        file(STRINGS "${_strux_path}" _strux_lines)
        foreach(_strux_line ${_strux_lines})
            if(_strux_line MATCHES "^# *STRUX_DRIFT_EXEMPT: *(CONFIG_[A-Za-z0-9_]+)")
                list(APPEND _strux_exempt "${CMAKE_MATCH_1}")
                continue()
            endif()
            if(NOT _strux_line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=")
                continue()
            endif()
            set(_strux_key "${CMAKE_MATCH_1}")
            list(APPEND _strux_keys "${_strux_key}")
            set(_strux_want_${_strux_key} "${_strux_line}")
            set(_strux_from_${_strux_key} "${_strux_shown}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _strux_keys)

    # ── Pass two: check what survived against what was generated ──
    set(_strux_drift "")
    foreach(_strux_key ${_strux_keys})
        if("${_strux_key}" IN_LIST _strux_exempt)
            continue()
        endif()
        string(FIND "${_strux_actual}" "\n${_strux_want_${_strux_key}}\n" _strux_pos)
        if(NOT _strux_pos EQUAL -1)
            continue()
        endif()
        # Say what it is instead, so the message is diagnosable without a diff.
        if("${_strux_actual}" MATCHES "\n${_strux_key}=([^\n]*)\n")
            set(_strux_is "it is ${_strux_key}=${CMAKE_MATCH_1}")
        elseif("${_strux_actual}" MATCHES "\n# ${_strux_key} is not set\n")
            set(_strux_is "it is not set")
        else()
            set(_strux_is "it is absent entirely — its dependencies are unmet")
        endif()
        list(APPEND _strux_drift
             "  ${_strux_from_${_strux_key}} asks for ${_strux_want_${_strux_key}}, but ${_strux_is}")
    endforeach()

    if(_strux_drift)
        string(REPLACE ";" "\n" _strux_drift "${_strux_drift}")
        message(FATAL_ERROR
            "${_strux_sdkconfig} disagrees with the defaults it was generated from:\n"
            "${_strux_drift}\n\n"
            "A generated sdkconfig wins over sdkconfig.defaults, so those lines are not "
            "in effect and this build would silently use the old configuration. Delete "
            "the generated file and let set-target rewrite it:\n"
            "  rm ${_strux_sdkconfig}\n"
            "  idf.py -DBOARD=${BOARD} set-target <chip>\n\n"
            "If a difference is a deliberate local override, build with "
            "-DSTRUX_ALLOW_SDKCONFIG_DRIFT=ON.")
    endif()
endif()

# ── Frontend ──────────────────────────────────────────────────
# Nothing to do here any more. The frontend is built and embedded by
# components/web_assets, which owns the pnpm invocation, the packer and the
# EMBED_FILES entry. There is no FAT image to generate and no `www` partition
# to flash — see partitions.csv.
