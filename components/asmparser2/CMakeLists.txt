# ESPLiveScript 2 (asmparser2): on-device compiler for a small C-like
# language that emits Xtensa machine code. Vendored from hpwit/new-parser at
# the commit recorded in UPSTREAM_COMMIT; same wrapper as the upstream
# exampleESPIDF/components/asmparser2 component.
# The compiler emits Xtensa code: other targets get an empty component.
if(NOT IDF_TARGET MATCHES "^esp32(s2|s3)?$")
    idf_component_register()
    return()
endif()

file(GLOB ASMPARSER2_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${ASMPARSER2_SRCS}
    INCLUDE_DIRS "src"
    REQUIRES freertos heap esp_timer xtensa
)

# The library guards its FreeRTOS/heap_caps code on ESP32, which Arduino
# defines and plain ESP-IDF does not. It only targets Xtensa ESP32/S2/S3.
# PRIVATE so the macro does not leak into every SolarOS source; the SolarOS
# translation unit that includes these headers defines it itself (the class
# layout of ScriptExecutable depends on it).
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=misleading-indentation
    -Wno-error
)
