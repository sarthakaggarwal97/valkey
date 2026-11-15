project(valkeyjson)

if (VALKEY_DEBUG_BUILD)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -g -ggdb -std=gnu11 -O2 -D_GNU_SOURCE")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -W -Wall -fno-common -O3 -std=gnu11 -D_GNU_SOURCE")
endif ()

set(JSON_MODULE_SRCS
    json_module.c
    json_cmd.c
    json_path.c
    json_engine.c
    json_num.c)

add_library(valkeyjson STATIC "${JSON_MODULE_SRCS}")

# On 32-bit x86, use SSE2 for double arithmetic so every operation rounds to
# double, as on every other target (see Makefile).
include(CheckSymbolExists)
check_symbol_exists(__i386__ "" JSON_TARGET_I386)
if (JSON_TARGET_I386)
    target_compile_options(valkeyjson PRIVATE -msse2 -mfpmath=sse)
endif ()
add_dependencies(valkey-server valkeyjson)
