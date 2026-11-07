include(${COMPONENT_DIR}/cmake/utilities.cmake)

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()

set(src_dirs .)
set(include_dirs .)
set(requires esp-dl json)
set(packed_model ${BUILD_DIR}/espdl_models/tool_detect.espdl)

idf_component_register(
    SRC_DIRS
        ${src_dirs}
    INCLUDE_DIRS
        ${include_dirs}
    REQUIRES
        ${requires})

if (IDF_TARGET STREQUAL "esp32p4")
    set(models_dir ${COMPONENT_DIR}/models/p4)
else()
    message(FATAL_ERROR "Tool model is currently provided only for ESP32-P4")
endif()

file(MAKE_DIRECTORY ${BUILD_DIR}/espdl_models)
set(models ${models_dir}/espdet_pico_416_v14_qat.espdl)
set(pack_model_exe ${espdl_dir}/fbs_loader/pack_espdl_models.py)

add_custom_command(
    OUTPUT ${packed_model}
    COMMENT "Pack Tool ESP-DL model..."
    COMMAND python ${pack_model_exe} --model_path ${models} --out_file ${packed_model}
    DEPENDS ${models}
    VERBATIM)

target_add_aligned_binary_data(${COMPONENT_LIB} ${packed_model} BINARY)
target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/slot_template.json" TEXT)
