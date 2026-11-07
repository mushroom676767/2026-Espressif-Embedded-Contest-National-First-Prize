include(${COMPONENT_DIR}/cmake/utilities.cmake)

set(src_dirs .)
set(include_dirs .)
set(requires esp-dl)

idf_component_register(
    SRC_DIRS
        ${src_dirs}
    INCLUDE_DIRS
        ${include_dirs}
    REQUIRES
        ${requires})

if (IDF_TARGET STREQUAL "esp32p4")
    set(ppe_model ${COMPONENT_DIR}/models/p4/ppe_qat_v3_416.espdl)
else()
    message(FATAL_ERROR "PPE model is currently provided only for ESP32-P4")
endif()

target_add_aligned_binary_data(
    ${COMPONENT_LIB}
    ${ppe_model}
    BINARY
    RENAME_TO ppe_qat_v3_416_espdl)
