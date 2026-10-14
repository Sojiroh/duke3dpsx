# Duke Nukem 3D PS1 port - build script (PSn00bSDK)
cmake_minimum_required(VERSION 3.21)

project(
	duke3d-ps1
	LANGUAGES    C ASM
	VERSION      0.1.0
	DESCRIPTION  "Duke Nukem 3D for the PlayStation"
)

set(DUKE_GRP "$ENV{HOME}/Downloads/DUKE3D/DUKE3D.GRP" CACHE FILEPATH "Path to DUKE3D.GRP (v1.3D)")
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
	COMMAND python3 ${PROJECT_SOURCE_DIR}/tools/mkdata.py ${ASSET_ARGS}
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
target_compile_definitions(duke3d PRIVATE PS1=1)
set(EXTRA_CFLAGS "" CACHE STRING "Extra compiler flags (debug switches)")
if(EXTRA_CFLAGS)
	separate_arguments(_extra UNIX_COMMAND "${EXTRA_CFLAGS}")
	target_compile_options(duke3d PRIVATE ${_extra})
endif()

psn00bsdk_add_cd_image(
	iso duke3d
	${PROJECT_SOURCE_DIR}/iso.xml
	DEPENDS duke3d ${PROJECT_SOURCE_DIR}/system.cnf ${DATA_DIR}/DUKE.DAT
)
