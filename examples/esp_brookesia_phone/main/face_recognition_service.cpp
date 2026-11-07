#include "face_recognition_service.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "auth_camera_preview.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "face_model_storage.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "nvs.h"
#include "user_manager.h"

static const char *TAG = "face_recognition";

#define FACE_TASK_STACK_SIZE          (1024 * 14)
#define FACE_TASK_PRIORITY            4
#define FACE_TASK_CORE                1
#define FACE_ENROLL_SAMPLE_TARGET     3
#define FACE_AUTH_CONFIRM_COUNT       1
#define FACE_RECOGNITION_THRESHOLD    0.45f
#define FACE_MSR_SCORE_THRESHOLD      0.35f
#define FACE_MNP_SCORE_THRESHOLD      0.45f
#define FACE_DETECT_NMS_THRESHOLD     0.50f
#define FACE_USER_NVS_NAMESPACE       "face_users"
#define FACE_PHOTO_DIR                CONFIG_BSP_SD_MOUNT_POINT "/faces"
#define FACE_PHOTO_WIDTH              160
#define FACE_PHOTO_HEIGHT             120

typedef enum {
    FACE_MODE_IDLE = 0,
    FACE_MODE_ENROLL,
    FACE_MODE_AUTH,
} face_mode_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;

    face_mode_t mode;
    bool active;
    face_recognition_event_cb_t cb;
    void *user_ctx;

    uint8_t *frame_buf;
    size_t frame_cap;
    size_t frame_len;
    uint32_t frame_width;
    uint32_t frame_height;
    bool frame_ready;
    bool processing;

    char enroll_name[FACE_REC_NAME_MAX_LEN];
    char enroll_employee_id[FACE_REC_EMPLOYEE_ID_MAX_LEN];
    uint16_t enroll_feature_ids[USER_FACE_FEATURE_MAX_COUNT];
    uint8_t enroll_samples;
    uint16_t last_match_id;
    uint8_t last_match_count;
} face_service_state_t;

static face_service_state_t s_state;
static HumanFaceDetect *s_detector = nullptr;
static HumanFaceRecognizer *s_recognizer = nullptr;

static void face_recognition_task(void *arg);

static void lock_state(void)
{
    if (s_state.lock != nullptr) {
        xSemaphoreTake(s_state.lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_state.lock != nullptr) {
        xSemaphoreGive(s_state.lock);
    }
}

static void build_user_key(uint16_t feature_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "face_user_%u", feature_id);
}

static void build_user_emp_key(uint16_t feature_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "face_emp_%u", feature_id);
}

static esp_err_t save_feature_user(uint16_t feature_id, const char *employee_id, const char *name)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(FACE_USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    build_user_key(feature_id, key, sizeof(key));
    ret = nvs_set_str(handle, key, name);
    if (ret == ESP_OK) {
        build_user_emp_key(feature_id, key, sizeof(key));
        ret = nvs_set_str(handle, key, employee_id != nullptr ? employee_id : "");
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t erase_feature_user(uint16_t feature_id)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(FACE_USER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    build_user_key(feature_id, key, sizeof(key));
    ret = nvs_erase_key(handle, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        build_user_emp_key(feature_id, key, sizeof(key));
        esp_err_t emp_ret = nvs_erase_key(handle, key);
        if (emp_ret != ESP_OK && emp_ret != ESP_ERR_NVS_NOT_FOUND) {
            ret = emp_ret;
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t load_feature_mapping(uint16_t feature_id,
                                      char *employee_id,
                                      size_t employee_id_size,
                                      bool *has_employee_id,
                                      char *name,
                                      size_t name_size,
                                      bool *has_name)
{
    if (has_employee_id != nullptr) {
        *has_employee_id = false;
    }
    if (has_name != nullptr) {
        *has_name = false;
    }
    if (employee_id != nullptr && employee_id_size > 0) {
        employee_id[0] = '\0';
    }
    if (name != nullptr && name_size > 0) {
        name[0] = '\0';
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(FACE_USER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[16];
    build_user_emp_key(feature_id, key, sizeof(key));
    size_t size = employee_id_size;
    esp_err_t emp_ret = nvs_get_str(handle, key, employee_id, &size);
    if (emp_ret == ESP_OK) {
        if (has_employee_id != nullptr) {
            *has_employee_id = true;
        }
    } else if (emp_ret != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return emp_ret;
    }

    build_user_key(feature_id, key, sizeof(key));
    size = name_size;
    esp_err_t name_ret = nvs_get_str(handle, key, name, &size);
    if (name_ret == ESP_OK) {
        if (has_name != nullptr) {
            *has_name = true;
        }
    } else if (name_ret != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return name_ret;
    }

    nvs_close(handle);
    return (emp_ret == ESP_OK || name_ret == ESP_OK) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t save_face_thumbnail(const char *employee_id,
                                     const uint8_t *data,
                                     uint32_t width,
                                     uint32_t height,
                                     char *path,
                                     size_t path_size)
{
    if (employee_id == nullptr || employee_id[0] == '\0' || data == nullptr ||
        width == 0 || height == 0 || path == nullptr || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mkdir(FACE_PHOTO_DIR, 0775);
    snprintf(path, path_size, "%s/%s.rgb565", FACE_PHOTO_DIR, employee_id);

    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Open face thumbnail failed: %s", path);
        path[0] = '\0';
        return ESP_FAIL;
    }

    const uint16_t *src = reinterpret_cast<const uint16_t *>(data);
    uint16_t line[FACE_PHOTO_WIDTH];
    for (uint32_t y = 0; y < FACE_PHOTO_HEIGHT; y++) {
        uint32_t src_y = (y * height) / FACE_PHOTO_HEIGHT;
        for (uint32_t x = 0; x < FACE_PHOTO_WIDTH; x++) {
            uint32_t src_x = (x * width) / FACE_PHOTO_WIDTH;
            line[x] = src[src_y * width + src_x];
        }
        if (fwrite(line, sizeof(uint16_t), FACE_PHOTO_WIDTH, file) != FACE_PHOTO_WIDTH) {
            fclose(file);
            path[0] = '\0';
            return ESP_FAIL;
        }
    }

    fclose(file);
    ESP_LOGI(TAG, "Saved face thumbnail: %s", path);
    return ESP_OK;
}

static uint16_t read_last_face_feature_id(void)
{
    typedef struct {
        uint16_t num_feats_total;
        uint16_t num_feats_valid;
        uint16_t feat_len;
    } face_db_meta_t;

    FILE *file = fopen(face_model_storage_db_path(), "rb");
    if (file == nullptr) {
        return 0;
    }

    face_db_meta_t meta = {};
    size_t read_len = fread(&meta, sizeof(meta), 1, file);
    fclose(file);
    if (read_len != 1) {
        return 0;
    }
    return meta.num_feats_total;
}

static void fill_event(face_recognition_event_t *event, face_recognition_event_type_t type,
                       const char *message)
{
    memset(event, 0, sizeof(*event));
    event->type = type;
    if (message != nullptr) {
        snprintf(event->message, sizeof(event->message), "%s", message);
    }
}

static void fill_user_event(face_recognition_event_t *event, const user_record_t *record)
{
    if (event == nullptr || record == nullptr) {
        return;
    }
    snprintf(event->employee_id, sizeof(event->employee_id), "%s", record->employee_id);
    snprintf(event->name, sizeof(event->name), "%s", record->name);
}

static void emit_event(const face_recognition_event_t *event)
{
    face_recognition_event_cb_t cb = nullptr;
    void *user_ctx = nullptr;

    lock_state();
    cb = s_state.cb;
    user_ctx = s_state.user_ctx;
    unlock_state();

    if (cb != nullptr) {
        cb(event, user_ctx);
    }
}

static esp_err_t ensure_task(void)
{
    if (s_state.lock == nullptr) {
        s_state.lock = xSemaphoreCreateMutex();
        if (s_state.lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_state.task == nullptr) {
        BaseType_t ret = xTaskCreatePinnedToCore(face_recognition_task,
                                                 "face_recognition",
                                                 FACE_TASK_STACK_SIZE,
                                                 nullptr,
                                                 FACE_TASK_PRIORITY,
                                                 &s_state.task,
                                                 FACE_TASK_CORE);
        if (ret != pdPASS) {
            s_state.task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static esp_err_t ensure_models(void)
{
    if (!face_model_storage_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_detector == nullptr) {
        s_detector = new (std::nothrow) HumanFaceDetect(face_model_storage_model_dir());
        if (s_detector == nullptr) {
            return ESP_ERR_NO_MEM;
        }

        // Keep the official MSR -> MNP cascade, but let MSR retain more
        // candidates. MNP and face recognition still reject weak matches.
        s_detector->set_score_thr(FACE_MSR_SCORE_THRESHOLD, 0);
        s_detector->set_score_thr(FACE_MNP_SCORE_THRESHOLD, 1);
        s_detector->set_nms_thr(FACE_DETECT_NMS_THRESHOLD, 0);
        s_detector->set_nms_thr(FACE_DETECT_NMS_THRESHOLD, 1);
        ESP_LOGI(TAG,
                 "Face detector thresholds: MSR=%.2f MNP=%.2f NMS=%.2f",
                 FACE_MSR_SCORE_THRESHOLD,
                 FACE_MNP_SCORE_THRESHOLD,
                 FACE_DETECT_NMS_THRESHOLD);
    }

    if (s_recognizer == nullptr) {
        char db_path[128];
        snprintf(db_path, sizeof(db_path), "%s", face_model_storage_db_path());
        s_recognizer = new (std::nothrow) HumanFaceRecognizer(db_path,
                                                              HumanFaceFeat::MFN_S8_V1,
                                                              FACE_RECOGNITION_THRESHOLD,
                                                              1);
        if (s_recognizer == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static bool ensure_frame_capacity(size_t len)
{
    if (s_state.frame_cap >= len && s_state.frame_buf != nullptr) {
        return true;
    }

    uint8_t *new_buf = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer: %u bytes", (unsigned int)len);
        return false;
    }

    if (s_state.frame_buf != nullptr) {
        heap_caps_free(s_state.frame_buf);
    }
    s_state.frame_buf = new_buf;
    s_state.frame_cap = len;
    return true;
}

static void preview_frame_listener(const uint8_t *data, uint32_t width, uint32_t height,
                                   size_t len, void *user_ctx)
{
    (void)user_ctx;

    if (data == nullptr || width == 0 || height == 0) {
        return;
    }

    if (len == 0) {
        len = width * height * sizeof(uint16_t);
    }

    if (xSemaphoreTake(s_state.lock, 0) != pdTRUE) {
        return;
    }

    if (!s_state.active || s_state.frame_ready) {
        xSemaphoreGive(s_state.lock);
        return;
    }

    if (!ensure_frame_capacity(len)) {
        xSemaphoreGive(s_state.lock);
        return;
    }

    memcpy(s_state.frame_buf, data, len);
    s_state.frame_len = len;
    s_state.frame_width = width;
    s_state.frame_height = height;
    s_state.frame_ready = true;

    TaskHandle_t task = s_state.task;
    xSemaphoreGive(s_state.lock);

    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

static bool get_frame_for_processing(uint8_t **data, uint32_t *width, uint32_t *height, size_t *len,
                                     face_mode_t *mode)
{
    bool ready = false;

    lock_state();
    if (s_state.active && s_state.frame_ready && s_state.frame_buf != nullptr) {
        *data = s_state.frame_buf;
        *width = s_state.frame_width;
        *height = s_state.frame_height;
        *len = s_state.frame_len;
        *mode = s_state.mode;
        s_state.processing = true;
        ready = true;
    }
    unlock_state();

    return ready;
}

static void release_frame(void)
{
    lock_state();
    s_state.frame_ready = false;
    s_state.processing = false;
    unlock_state();
}

static bool service_is_still_active(face_mode_t mode)
{
    bool active = false;

    lock_state();
    active = s_state.active && s_state.mode == mode;
    unlock_state();

    return active;
}

static void finish_active_session(void)
{
    lock_state();
    s_state.active = false;
    s_state.mode = FACE_MODE_IDLE;
    s_state.frame_ready = false;
    unlock_state();

    auth_camera_preview_set_frame_callback(nullptr, nullptr);
}

static void handle_enroll_frame(const dl::image::img_t &img,
                                const std::list<dl::detect::result_t> &detect_results)
{
    face_recognition_event_t event;

    if (detect_results.empty()) {
        fill_event(&event, FACE_REC_EVT_NO_FACE, "未检测到人脸");
        emit_event(&event);
        return;
    }

    if (detect_results.size() > 1) {
        fill_event(&event, FACE_REC_EVT_MULTI_FACE, "检测到多张人脸");
        emit_event(&event);
        return;
    }

    esp_err_t ret = s_recognizer->enroll(img, detect_results);
    if (ret != ESP_OK) {
        fill_event(&event, FACE_REC_EVT_ERROR, "人脸录入失败");
        emit_event(&event);
        return;
    }

    uint16_t feature_id = read_last_face_feature_id();
    if (feature_id == 0) {
        fill_event(&event, FACE_REC_EVT_ERROR, "人脸特征编号无效");
        emit_event(&event);
        return;
    }

    char name[FACE_REC_NAME_MAX_LEN];
    char employee_id[FACE_REC_EMPLOYEE_ID_MAX_LEN];
    uint16_t feature_ids[USER_FACE_FEATURE_MAX_COUNT] = {};

    lock_state();
    snprintf(name, sizeof(name), "%s", s_state.enroll_name);
    snprintf(employee_id, sizeof(employee_id), "%s", s_state.enroll_employee_id);
    if (s_state.enroll_samples < USER_FACE_FEATURE_MAX_COUNT) {
        s_state.enroll_feature_ids[s_state.enroll_samples] = feature_id;
    }
    s_state.enroll_samples++;
    uint8_t samples = s_state.enroll_samples;
    memcpy(feature_ids, s_state.enroll_feature_ids, sizeof(feature_ids));
    unlock_state();

    ret = save_feature_user(feature_id, employee_id, name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save face user mapping: %s", esp_err_to_name(ret));
        s_recognizer->delete_last_feat();
        fill_event(&event, FACE_REC_EVT_ERROR, "保存用户映射失败");
        emit_event(&event);
        return;
    }

    fill_event(&event, FACE_REC_EVT_ENROLL_PROGRESS, "人脸样本已保存");
    snprintf(event.employee_id, sizeof(event.employee_id), "%s", employee_id);
    snprintf(event.name, sizeof(event.name), "%s", name);
    event.feature_id = feature_id;
    event.sample_count = samples;
    emit_event(&event);

    if (samples >= FACE_ENROLL_SAMPLE_TARGET) {
        char photo_path[USER_FACE_PHOTO_PATH_MAX_LEN] = {};
        esp_err_t photo_ret = save_face_thumbnail(employee_id, static_cast<const uint8_t *>(img.data),
                                                  img.width, img.height,
                                                  photo_path, sizeof(photo_path));
        if (photo_ret != ESP_OK) {
            ESP_LOGW(TAG, "Face thumbnail save failed: %s", esp_err_to_name(photo_ret));
        }

        ret = user_manager_set_face(employee_id, feature_ids, USER_FACE_FEATURE_MAX_COUNT, photo_path);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save face user index: %s", esp_err_to_name(ret));
            fill_event(&event, FACE_REC_EVT_ERROR, "保存人脸绑定失败");
            emit_event(&event);
            return;
        }

        fill_event(&event, FACE_REC_EVT_ENROLL_DONE, "人脸绑定完成");
        snprintf(event.employee_id, sizeof(event.employee_id), "%s", employee_id);
        snprintf(event.name, sizeof(event.name), "%s", name);
        event.feature_id = feature_id;
        event.sample_count = samples;
        finish_active_session();
        emit_event(&event);
    }
}

static void handle_auth_frame(const dl::image::img_t &img,
                              const std::list<dl::detect::result_t> &detect_results)
{
    face_recognition_event_t event;

    if (detect_results.empty()) {
        lock_state();
        s_state.last_match_id = 0;
        s_state.last_match_count = 0;
        unlock_state();

        fill_event(&event, FACE_REC_EVT_NO_FACE, "未检测到人脸");
        emit_event(&event);
        return;
    }

    auto results = s_recognizer->recognize(img, detect_results);
    if (results.empty()) {
        lock_state();
        s_state.last_match_id = 0;
        s_state.last_match_count = 0;
        unlock_state();

        fill_event(&event, FACE_REC_EVT_NO_USER, "未匹配到已注册用户");
        emit_event(&event);
        return;
    }

    uint16_t feature_id = results.front().id;
    float similarity = results.front().similarity;
    user_record_t record = {};
    esp_err_t ret = user_manager_find_by_face_feature(feature_id, &record);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Matched feature %u has no live user record: %s",
                 feature_id, esp_err_to_name(ret));
        fill_event(&event, FACE_REC_EVT_NO_USER, "匹配的人脸没有有效用户");
        emit_event(&event);
        return;
    }

    uint8_t match_count = 0;
    lock_state();
    if (s_state.last_match_id == feature_id) {
        s_state.last_match_count++;
    } else {
        s_state.last_match_id = feature_id;
        s_state.last_match_count = 1;
    }
    match_count = s_state.last_match_count;
    unlock_state();

    if (match_count < FACE_AUTH_CONFIRM_COUNT) {
        fill_event(&event, FACE_REC_EVT_PROGRESS, "正在确认人脸匹配");
        fill_user_event(&event, &record);
        event.feature_id = feature_id;
        event.similarity = similarity;
        event.sample_count = match_count;
        emit_event(&event);
        return;
    }

    fill_event(&event, FACE_REC_EVT_AUTH_MATCH, "人脸验证通过");
    fill_user_event(&event, &record);
    event.feature_id = feature_id;
    event.similarity = similarity;
    event.sample_count = match_count;
    finish_active_session();
    emit_event(&event);
}

static void process_frame(uint8_t *data, uint32_t width, uint32_t height, face_mode_t mode)
{
    dl::image::img_t img = {};
    img.data = data;
    img.width = width;
    img.height = height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

    auto detect_results = s_detector->run(img);
    if (!service_is_still_active(mode)) {
        return;
    }

    if (mode == FACE_MODE_ENROLL) {
        handle_enroll_frame(img, detect_results);
    } else if (mode == FACE_MODE_AUTH) {
        handle_auth_frame(img, detect_results);
    }
}

static void face_recognition_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t *data = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        size_t len = 0;
        face_mode_t mode = FACE_MODE_IDLE;

        if (!get_frame_for_processing(&data, &width, &height, &len, &mode)) {
            continue;
        }

        (void)len;
        process_frame(data, width, height, mode);
        release_frame();
    }
}

esp_err_t face_recognition_service_init(void)
{
    return ensure_task();
}

esp_err_t face_recognition_start_enroll(const char *employee_id, const char *name,
                                        face_recognition_event_cb_t cb, void *user_ctx)
{
    if (employee_id == nullptr || employee_id[0] == '\0' || name == nullptr || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = user_manager_upsert_identity(employee_id, name);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_models();
    if (ret != ESP_OK) {
        return ret;
    }

    lock_state();
    s_state.mode = FACE_MODE_ENROLL;
    s_state.active = true;
    s_state.cb = cb;
    s_state.user_ctx = user_ctx;
    s_state.frame_ready = false;
    s_state.enroll_samples = 0;
    memset(s_state.enroll_feature_ids, 0, sizeof(s_state.enroll_feature_ids));
    s_state.last_match_id = 0;
    s_state.last_match_count = 0;
    snprintf(s_state.enroll_employee_id, sizeof(s_state.enroll_employee_id), "%s", employee_id);
    snprintf(s_state.enroll_name, sizeof(s_state.enroll_name), "%s", name);
    unlock_state();

    auth_camera_preview_set_frame_callback(preview_frame_listener, nullptr);
    return ESP_OK;
}

esp_err_t face_recognition_start_auth(face_recognition_event_cb_t cb, void *user_ctx)
{
    esp_err_t ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_models();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_recognizer->get_num_feats() <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    lock_state();
    s_state.mode = FACE_MODE_AUTH;
    s_state.active = true;
    s_state.cb = cb;
    s_state.user_ctx = user_ctx;
    s_state.frame_ready = false;
    s_state.enroll_samples = 0;
    s_state.last_match_id = 0;
    s_state.last_match_count = 0;
    s_state.enroll_name[0] = '\0';
    unlock_state();

    auth_camera_preview_set_frame_callback(preview_frame_listener, nullptr);
    return ESP_OK;
}

static bool feature_id_in_list(uint16_t feature_id, const uint16_t *feature_ids, uint8_t feature_count)
{
    for (uint8_t i = 0; i < feature_count; i++) {
        if (feature_ids[i] == feature_id) {
            return true;
        }
    }
    return false;
}

static bool other_live_user_has_name(const char *name, const char *employee_id)
{
    if (name == nullptr || name[0] == '\0') {
        return true;
    }

    user_record_t records[USER_MANAGER_MAX_USERS];
    size_t count = 0;
    if (user_manager_list(records, USER_MANAGER_MAX_USERS, &count) != ESP_OK) {
        return true;
    }

    size_t shown = count > USER_MANAGER_MAX_USERS ? USER_MANAGER_MAX_USERS : count;
    for (size_t i = 0; i < shown; i++) {
        if (employee_id != nullptr && strcmp(records[i].employee_id, employee_id) == 0) {
            continue;
        }
        if (strcmp(records[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t delete_one_face_feature(uint16_t feature_id)
{
    if (feature_id == 0) {
        return ESP_OK;
    }

    esp_err_t ret = s_recognizer->delete_feat(feature_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Delete face feature %u failed: %s",
                 feature_id, esp_err_to_name(ret));
        return ret;
    }

    esp_err_t erase_ret = erase_feature_user(feature_id);
    if (erase_ret != ESP_OK) {
        ESP_LOGE(TAG, "Erase face mapping %u failed: %s",
                 feature_id, esp_err_to_name(erase_ret));
        return erase_ret;
    }
    return ESP_OK;
}

esp_err_t face_recognition_delete_features(const uint16_t *feature_ids, uint8_t feature_count)
{
    if (feature_ids == nullptr || feature_count == 0) {
        return ESP_OK;
    }

    esp_err_t ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_models();
    if (ret != ESP_OK) {
        return ret;
    }

    lock_state();
    bool active = s_state.active;
    unlock_state();
    if (active) {
        finish_active_session();
    }

    esp_err_t result = ESP_OK;
    for (uint8_t i = 0; i < feature_count; i++) {
        esp_err_t del_ret = delete_one_face_feature(feature_ids[i]);
        if (del_ret != ESP_OK && result == ESP_OK) {
            result = del_ret;
        }
    }
    return result;
}

esp_err_t face_recognition_delete_user_data(const char *employee_id,
                                            const char *name,
                                            const uint16_t *feature_ids,
                                            uint8_t feature_count)
{
    if (employee_id == nullptr || employee_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (feature_ids == nullptr) {
        feature_count = 0;
    }

    esp_err_t ret = ensure_task();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_models();
    if (ret != ESP_OK) {
        return ret;
    }

    lock_state();
    bool active = s_state.active;
    unlock_state();
    if (active) {
        finish_active_session();
    }

    esp_err_t result = ESP_OK;
    for (uint8_t i = 0; i < feature_count; i++) {
        esp_err_t del_ret = delete_one_face_feature(feature_ids[i]);
        if (del_ret != ESP_OK && result == ESP_OK) {
            result = del_ret;
        }
    }

    bool allow_legacy_name_delete = !other_live_user_has_name(name, employee_id);
    uint16_t max_feature_id = read_last_face_feature_id();
    for (uint16_t id = 1; id <= max_feature_id; id++) {
        if (feature_id_in_list(id, feature_ids, feature_count)) {
            continue;
        }

        char saved_emp[FACE_REC_EMPLOYEE_ID_MAX_LEN] = {};
        char saved_name[FACE_REC_NAME_MAX_LEN] = {};
        bool has_emp = false;
        bool has_name = false;
        ret = load_feature_mapping(id,
                                   saved_emp, sizeof(saved_emp), &has_emp,
                                   saved_name, sizeof(saved_name), &has_name);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (ret != ESP_OK) {
            if (result == ESP_OK) {
                result = ret;
            }
            continue;
        }

        bool match_employee = has_emp && strcmp(saved_emp, employee_id) == 0;
        bool match_legacy_name = allow_legacy_name_delete &&
                                 !has_emp &&
                                 has_name &&
                                 name != nullptr &&
                                 strcmp(saved_name, name) == 0;
        if (!match_employee && !match_legacy_name) {
            continue;
        }

        esp_err_t del_ret = delete_one_face_feature(id);
        if (del_ret != ESP_OK && result == ESP_OK) {
            result = del_ret;
        }
    }
    return result;
}

void face_recognition_cancel(void)
{
    finish_active_session();
}

bool face_recognition_is_active(void)
{
    bool active = false;

    lock_state();
    active = s_state.active;
    unlock_state();

    return active;
}

esp_err_t face_recognition_release_idle_resources(void)
{
    HumanFaceDetect *detector = nullptr;
    HumanFaceRecognizer *recognizer = nullptr;
    uint8_t *frame_buf = nullptr;

    lock_state();
    if (s_state.active || s_state.processing) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }

    detector = s_detector;
    recognizer = s_recognizer;
    frame_buf = s_state.frame_buf;
    s_detector = nullptr;
    s_recognizer = nullptr;
    s_state.frame_buf = nullptr;
    s_state.frame_cap = 0;
    s_state.frame_len = 0;
    s_state.frame_width = 0;
    s_state.frame_height = 0;
    s_state.frame_ready = false;
    unlock_state();

    delete detector;
    delete recognizer;
    if (frame_buf != nullptr) {
        heap_caps_free(frame_buf);
    }

    ESP_LOGI(TAG,
             "Released idle face models and frame buffer: free_internal=%u largest_internal=%u",
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return ESP_OK;
}
