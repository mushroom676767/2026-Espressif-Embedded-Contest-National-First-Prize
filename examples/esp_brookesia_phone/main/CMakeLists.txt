idf_component_register(
    SRCS
        main.cpp
        auth_camera_preview.c
        face_recognition_service.cpp
        ppe_detection_service.cpp
        usb_uvc_camera_service.c
        tool_detection_service.cpp
        face_model_storage.c
        lvgl_adapter_init.c
        register_pin_gate.c
        tool_cabinet.c
        tool_record_store.c
        cabinet_snapshot_camera.c
        cloud_uploader.c
        cabinet_core.c
        cabinet_door_signal.c
        cabinet_imu.c
        pn532_uart.c
        nfc_manager.c
        user_manager.c
        wifi_manager.c
        fonts/register_cjk_font_20.c
        fonts/ui_cjk_font_22.c
        fonts/ui_cjk_font_26.c
        fonts/ui_cjk_font_32.c
        ui/ui_theme.c
        ui/ui_touch_layout.c
        ui/ui_manager.c
        screens/screen_standby.c
        screens/screen_auth.c
        screens/screen_usb_preview.c
        screens/screen_operation.c
        screens/screen_result.c
        screens/screen_wifi.c
        screens/screen_register.c
        screens/screen_admin.c
        screens/screen_tool_records.c
    INCLUDE_DIRS
        .
        ui
        screens
    PRIV_REQUIRES
        app_update
        bsp_extra
        camera_support
        driver
        esp_event
        esp_http_client
        esp_netif
        espressif__esp_new_jpeg
        esp_timer
        esp_video
        esp_wifi
        human_face_detect
        human_face_recognition
        json
        ppe_detect
        tool_detect
        usb
        usb_host_uvc
        nvs_flash)

idf_build_get_property(PYTHON_EXECUTABLE PYTHON)
add_custom_target(check_cjk_font_coverage
    COMMAND
        "${PYTHON_EXECUTABLE}" -B
        "${CMAKE_CURRENT_LIST_DIR}/fonts/check_font_coverage.py"
        --project-root "${PROJECT_DIR}"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Checking bundled CJK font coverage"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} check_cjk_font_coverage)

add_custom_target(check_tool_inventory_assets
    COMMAND
        "${PYTHON_EXECUTABLE}" -B
        "${PROJECT_DIR}/tools/verify_tool_inventory_assets.py"
        "${PROJECT_DIR}"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Checking v14 tool model and embedded slot template"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} check_tool_inventory_assets)

add_custom_target(check_tool_inventory_policy
    COMMAND
        "${PYTHON_EXECUTABLE}" -B
        "${PROJECT_DIR}/tools/test_tool_inventory_policy.py"
        "${PROJECT_DIR}"
    WORKING_DIRECTORY "${PROJECT_DIR}"
    COMMENT "Checking class-aware NMS and slot matching policy"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} check_tool_inventory_policy)

set(FACE_MODEL_FILES
    ${PROJECT_DIR}/components/human_face_detect/models/p4/human_face_detect_msr_s8_v1.espdl
    ${PROJECT_DIR}/components/human_face_detect/models/p4/human_face_detect_mnp_s8_v1.espdl
    ${PROJECT_DIR}/components/human_face_recognition/models/p4/human_face_feat_mfn_s8_v1.espdl)

foreach(FACE_MODEL_FILE ${FACE_MODEL_FILES})
    target_add_binary_data(${COMPONENT_LIB} ${FACE_MODEL_FILE} BINARY)
endforeach()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-error=incompatible-pointer-types
)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "esp-brookesia" OR COMPONENT MATCHES "espressif__esp-brookesia")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component 'esp-brookesia' not found.")
    else()
        idf_component_get_property(ESP_BROOKESIA_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"  "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )
endif()

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB}
        PRIVATE
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
