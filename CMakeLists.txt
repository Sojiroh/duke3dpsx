# Duke Nukem 3D PS1 port - build script (PSn00bSDK)
cmake_minimum_required(VERSION 3.21)

project(
	duke3d-ps1
	LANGUAGES    C ASM
	VERSION      0.1.0
	DESCRIPTION  "Duke Nukem 3D for the PlayStation"
)

# Default GRP location: $HOME on Linux/macOS, %USERPROFILE% on Windows (HOME is normally unset there).
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
	set(_duke_home "$ENV{HOME}")
else()
	set(_duke_home "$ENV{USERPROFILE}")
endif()
file(TO_CMAKE_PATH "${_duke_home}" _duke_home)
set(DUKE_GRP "${_duke_home}/Downloads/DUKE3D/DUKE3D.GRP" CACHE FILEPATH "Path to DUKE3D.GRP (v1.3D)")
# The asset pack is built by tools/mkdata.py (needs numpy + Pillow). Pass -DPython3_EXECUTABLE=<python> to
# pick a specific interpreter (build.sh / build.ps1: DUKE_PYTHON / -Python); `python3` does not exist on a
# python.org Windows install, so it is located with FindPython3 instead of being hard-coded.
find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
# Music instrument bank source. Empty (default) = built-in synthesised bank. Set to a SoundFont (.sf2)
# to build the bank from it; it is never searched for, and its samples end up in DUKE.DAT.
set(DUKE_SF2 "" CACHE FILEPATH "Optional SoundFont (.sf2) for the music instrument bank (default: built-in synthesis)")
set(DATA_DIR ${PROJECT_BINARY_DIR}/data)

set(ASSET_ARGS ${DUKE_GRP} ${DATA_DIR})
set(ASSET_DEPS
	${PROJECT_SOURCE_DIR}/tools/mkdata.py ${PROJECT_SOURCE_DIR}/tools/grp.py
	${PROJECT_SOURCE_DIR}/tools/spuadpcm.py ${PROJECT_SOURCE_DIR}/tools/mkmusic.py ${DUKE_GRP})
if(DUKE_SF2)
	list(APPEND ASSET_ARGS --sf2 ${DUKE_SF2})
	list(APPEND ASSET_DEPS ${DUKE_SF2})
endif()

# Asset pack: DUKE.DAT + data_index.h generated from the GRP
add_custom_command(
	OUTPUT  ${DATA_DIR}/DUKE.DAT ${DATA_DIR}/data_index.h
	COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/mkdata.py ${ASSET_ARGS}
	DEPENDS ${ASSET_DEPS}
	COMMENT "Building DUKE.DAT asset pack"
)
add_custom_target(assets DEPENDS ${DATA_DIR}/DUKE.DAT ${DATA_DIR}/data_index.h)

option(DUKE_VIEWER "Build the engine map viewer instead of the game" OFF)

file(GLOB PS1_SRC    CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/ps1/*.c)
file(GLOB ENGINE_SRC CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/engine/*.c)
file(GLOB GAME_SRC   CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/game/*.c)
if(DUKE_VIEWER)
	list(FILTER PS1_SRC EXCLUDE REGEX "/main\\.c$")
	set(GAME_SRC ${PROJECT_SOURCE_DIR}/tools/viewer/viewer_main.c)
endif()

psn00bsdk_add_executable(duke3d GPREL ${PS1_SRC} ${ENGINE_SRC} ${GAME_SRC})
add_dependencies(duke3d assets)

target_include_directories(duke3d PRIVATE
	${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/src/ps1
	${PROJECT_SOURCE_DIR}/src/engine
	${PROJECT_SOURCE_DIR}/src/game
	${DATA_DIR}
)
target_compile_options(duke3d PRIVATE
	-Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses
	-Wno-missing-braces -Wno-char-subscripts -Wno-implicit-fallthrough
	-fno-strict-aliasing
)
# Watcom treats plain char as unsigned and the Build/Duke sources rely on it; the SDK
# appends -fsigned-char to target options, so apply ours at source level (comes last).
set_source_files_properties(${PS1_SRC} ${ENGINE_SRC} ${GAME_SRC} PROPERTIES COMPILE_OPTIONS "-funsigned-char")
# PS1: the game logic (src/game/*.c) is built with -Os: it is cold code compared to the engine, and -O2
# inlines the huge game.c/actors.c/player.c/sector.c functions heavily (-64 KB of text / RAM for ~0% frame
# time).  actors.c (the CON interpreter and actor AI, the hottest game code) keeps the SDK's -O2, as do the
# engine and platform sources; ps1_control.c / ps1_platform.c always do (tiny).  Override from build.sh:
# DUKE_GAME_OPT="" restores -O2 everywhere; DUKE_GAME_KEEP lists the game sources (base names) kept at -O2.
set(DUKE_GAME_OPT "-Os" CACHE STRING "Optimisation flag for src/game/*.c (empty = SDK default, -O2)")
set(DUKE_GAME_KEEP "actors" CACHE STRING "src/game sources (base names, no .c) kept at the default optimisation level")
if(DUKE_GAME_OPT)
	set(GAME_OPT_SRC ${GAME_SRC})
	foreach(_k ps1_control ps1_platform ${DUKE_GAME_KEEP})
		list(FILTER GAME_OPT_SRC EXCLUDE REGEX "/${_k}\\.c$")
	endforeach()
	set_property(SOURCE ${GAME_OPT_SRC} APPEND PROPERTY COMPILE_OPTIONS ${DUKE_GAME_OPT})
endif()
target_compile_definitions(duke3d PRIVATE PS1=1)
set(EXTRA_CFLAGS "" CACHE STRING "Extra compiler flags (debug switches)")
if(EXTRA_CFLAGS)
	separate_arguments(_extra UNIX_COMMAND "${EXTRA_CFLAGS}")
	target_compile_options(duke3d PRIVATE ${_extra})
	# PS1: profiling builds hook video_flip() for the stack high-water probe in src/ps1/main.c
	if("${EXTRA_CFLAGS}" MATCHES "PS1_PROF([= ]|$)" AND NOT DUKE_VIEWER)
		target_link_options(duke3d PRIVATE -Wl,--wrap=video_flip)
	endif()
endif()

psn00bsdk_add_cd_image(
	iso duke3d
	${PROJECT_SOURCE_DIR}/iso.xml
	DEPENDS duke3d ${PROJECT_SOURCE_DIR}/system.cnf ${DATA_DIR}/DUKE.DAT
)

# RAM budget check (always runs; the iso target depends on it). The stack grows down from 0x801FFFF0; keep
# 32 KB between the end of .bss (_end) and the stack top. That reserve also holds the SDK heap (InitHeap at
# _end, a few KB used by CdSearchFile) and the stack. tools/ramcheck.cmake reads _end with the toolchain's nm,
# prints "RAM: _end=0x..., stack reserve N KB" and fails the build when _end > 0x801F8000.
# CMAKE_C_COMPILER is <toolchain>/bin/mipsel-none-elf-gcc[.exe]; nm sits next to it.
string(REGEX REPLACE "-gcc(\\.[eE][xX][eE])?$" "-nm\\1" DUKE_NM "${CMAKE_C_COMPILER}")
add_custom_target(
	ramcheck
	COMMAND ${CMAKE_COMMAND} -DNM=${DUKE_NM} -DELF=$<TARGET_FILE:duke3d> -P ${PROJECT_SOURCE_DIR}/tools/ramcheck.cmake
	DEPENDS duke3d
	COMMENT "Checking the RAM budget"
	VERBATIM
)
add_dependencies(iso ramcheck)
