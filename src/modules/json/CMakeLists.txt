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
add_dependencies(valkey-server valkeyjson)
