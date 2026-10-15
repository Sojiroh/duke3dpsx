# RAM budget check, run as `cmake -DNM=<mipsel-none-elf-nm> -DELF=<duke3d.elf> -P ramcheck.cmake`
# (the `ramcheck` target in CMakeLists.txt).
#
# The stack grows down from 0x801FFFF0; keep 32 KB between the end of .bss (_end) and the stack top.
# That reserve also holds the SDK heap (InitHeap at _end, a few KB used by CdSearchFile) and the stack.
# Prints "RAM: _end=0x801xxxxx, stack reserve N KB" and fails when _end > 0x801F8000.

set(STACK_TOP 801FFFF0)
set(END_LIMIT 801F8000)

if(NOT NM OR NOT ELF)
	message(FATAL_ERROR "ramcheck.cmake: pass -DNM=<nm> -DELF=<elf>")
endif()

execute_process(
	COMMAND "${NM}" "${ELF}"
	OUTPUT_VARIABLE _nm_out
	ERROR_VARIABLE  _nm_err
	RESULT_VARIABLE _nm_rc
)
if(NOT _nm_rc EQUAL 0)
	message(FATAL_ERROR "error: ${NM} ${ELF} failed (${_nm_rc}): ${_nm_err}")
endif()

# Lines look like "801f1f80 B _end" (LF or CRLF); take the last 8 hex digits like the old awk did.
string(REGEX MATCH "(^|[\r\n])([0-9a-fA-F]+) [A-Za-z] _end[\r\n]" _m "${_nm_out}\n")
if(NOT _m)
	message(FATAL_ERROR "error: could not read _end from ${ELF}")
endif()
string(LENGTH "${CMAKE_MATCH_2}" _len)
if(_len GREATER 8)
	math(EXPR _off "${_len} - 8")
	string(SUBSTRING "${CMAKE_MATCH_2}" ${_off} 8 _end_hex)
else()
	set(_end_hex "${CMAKE_MATCH_2}")
endif()

math(EXPR _end   "0x${_end_hex}"   OUTPUT_FORMAT DECIMAL)
math(EXPR _limit "0x${END_LIMIT}"  OUTPUT_FORMAT DECIMAL)
math(EXPR _top   "0x${STACK_TOP}"  OUTPUT_FORMAT DECIMAL)

if(_end GREATER _limit)
	message(FATAL_ERROR "error: RAM budget exceeded: _end=0x${_end_hex} > 0x${END_LIMIT} (need 32 KB stack)")
endif()

math(EXPR _reserve "(${_top} - ${_end}) / 1024")
message("RAM: _end=0x${_end_hex}, stack reserve ${_reserve} KB")
