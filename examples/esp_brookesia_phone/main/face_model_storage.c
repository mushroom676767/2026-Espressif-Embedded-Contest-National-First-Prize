#include "face_model_storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_BSP_SD_MOUNT_POINT
#define CONFIG_BSP_SD_MOUNT_POINT "/sdcard"
#endif

#define FACE_MODEL_ROOT_DIR      CONFIG_BSP_SD_MOUNT_POINT "/espdl_models"
#define FACE_MODEL_DIR           FACE_MODEL_ROOT_DIR "/p4"
#define FACE_MODEL_MANIFEST_PATH FACE_MODEL_DIR "/model_manifest.txt"
#define FACE_DB_PATH             CONFIG_BSP_SD_MOUNT_POINT "/face.db"
#define COPY_BUFFER_SIZE         4096

extern const uint8_t human_face_detect_msr_s8_v1_espdl_start[] asm("_binary_human_face_detect_msr_s8_v1_espdl_start");
extern const uint8_t human_face_detect_msr_s8_v1_espdl_end[] asm("_binary_human_face_detect_msr_s8_v1_espdl_end");
extern const uint8_t human_face_detect_mnp_s8_v1_espdl_start[] asm("_binary_human_face_detect_mnp_s8_v1_espdl_start");
extern const uint8_t human_face_detect_mnp_s8_v1_espdl_end[] asm("_binary_human_face_detect_mnp_s8_v1_espdl_end");
extern const uint8_t human_face_feat_mfn_s8_v1_espdl_start[] asm("_binary_human_face_feat_mfn_s8_v1_espdl_start");
extern const uint8_t human_face_feat_mfn_s8_v1_espdl_end[] asm("_binary_human_face_feat_mfn_s8_v1_espdl_end");

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} face_model_blob_t;

static const char *TAG = "face_model_storage";
static bool s_ready = false;
static char s_last_error[160] = "Face model storage not initialized";

static const face_model_blob_t s_models[] = {
    {
        .name = "human_face_detect_msr_s8_v1.espdl",
        .start = human_face_detect_msr_s8_v1_espdl_start,
        .end = human_face_detect_msr_s8_v1_espdl_end,
    },
    {
        .name = "human_face_detect_mnp_s8_v1.espdl",
        .start = human_face_detect_mnp_s8_v1_espdl_start,
        .end = human_face_detect_mnp_s8_v1_espdl_end,
    },
    {
        .name = "human_face_feat_mfn_s8_v1.espdl",
        .start = human_face_feat_mfn_s8_v1_espdl_start,
        .end = human_face_feat_mfn_s8_v1_espdl_end,
    },
};

static void set_last_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

static size_t model_size(const face_model_blob_t *model)
{
    return (size_t)(model->end - model->start);
}

static esp_err_t make_path(char *dst, size_t dst_size, const char *dir, const char *name)
{
    int ret = snprintf(dst, dst_size, "%s/%s", dir, name);
    if (ret < 0 || (size_t)ret >= dst_size) {
        set_last_error("Path too long for %s", name);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if ((st.st_mode & S_IFDIR) != 0) {
            return ESP_OK;
        }
        set_last_error("%s exists but is not a directory", path);
        return ESP_ERR_INVALID_STATE;
    }

    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    set_last_error("Failed to create %s: errno %d", path, errno);
    return ESP_FAIL;
}

static bool file_has_expected_size(const char *path, size_t expected_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return st.st_size == (off_t)expected_size;
}

static esp_err_t build_manifest(char *buffer, size_t buffer_size)
{
    int written = snprintf(buffer,
                           buffer_size,
                           "fod_face_models:v1\n"
                           "dir=espdl_models/p4\n"
                           "%s=%u\n"
                           "%s=%u\n"
                           "%s=%u\n",
                           s_models[0].name,
                           (unsigned int)model_size(&s_models[0]),
                           s_models[1].name,
                           (unsigned int)model_size(&s_models[1]),
                           s_models[2].name,
                           (unsigned int)model_size(&s_models[2]));
    if (written < 0 || (size_t)written >= buffer_size) {
        set_last_error("Model manifest buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool manifest_matches(void)
{
    char expected[256];
    if (build_manifest(expected, sizeof(expected)) != ESP_OK) {
        return false;
    }

    FILE *file = fopen(FACE_MODEL_MANIFEST_PATH, "rb");
    if (file == NULL) {
        return false;
    }

    char actual[256];
    size_t bytes_read = fread(actual, 1, sizeof(actual) - 1, file);
    int eof_reached = feof(file);
    fclose(file);

    actual[bytes_read] = '\0';
    return eof_reached && strcmp(actual, expected) == 0;
}

static bool all_models_match(void)
{
    char path[192];

    for (size_t i = 0; i < sizeof(s_models) / sizeof(s_models[0]); ++i) {
        if (make_path(path, sizeof(path), FACE_MODEL_DIR, s_models[i].name) != ESP_OK) {
            return false;
        }
        if (!file_has_expected_size(path, model_size(&s_models[i]))) {
            return false;
        }
    }

    return true;
}

static esp_err_t write_file_atomically(const char *path, const uint8_t *data, size_t data_size)
{
    char tmp_path[208];
    int ret = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (ret < 0 || (size_t)ret >= sizeof(tmp_path)) {
        set_last_error("Temp path too long for %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(tmp_path, "wb");
    if (file == NULL) {
        set_last_error("Failed to open %s: errno %d", tmp_path, errno);
        return ESP_FAIL;
    }

    size_t offset = 0;
    while (offset < data_size) {
        size_t chunk = data_size - offset;
        if (chunk > COPY_BUFFER_SIZE) {
            chunk = COPY_BUFFER_SIZE;
        }
        if (fwrite(data + offset, 1, chunk, file) != chunk) {
            set_last_error("Failed to write %s: errno %d", tmp_path, errno);
            fclose(file);
            unlink(tmp_path);
            return ESP_FAIL;
        }
        offset += chunk;
    }

    if (fclose(file) != 0) {
        set_last_error("Failed to close %s: errno %d", tmp_path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    unlink(path);
    if (rename(tmp_path, path) != 0) {
        set_last_error("Failed to rename %s: errno %d", tmp_path, errno);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t copy_model(const face_model_blob_t *model)
{
    char path[192];
    esp_err_t ret = make_path(path, sizeof(path), FACE_MODEL_DIR, model->name);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Writing %s (%u bytes)", path, (unsigned int)model_size(model));
    return write_file_atomically(path, model->start, model_size(model));
}

static esp_err_t write_manifest(void)
{
    char manifest[256];
    esp_err_t ret = build_manifest(manifest, sizeof(manifest));
    if (ret != ESP_OK) {
        return ret;
    }

    return write_file_atomically(FACE_MODEL_MANIFEST_PATH, (const uint8_t *)manifest, strlen(manifest));
}

esp_err_t face_model_storage_init(void)
{
    s_ready = false;
    set_last_error("Face model storage not initialized");

    esp_err_t ret = ensure_dir(CONFIG_BSP_SD_MOUNT_POINT);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ensure_dir(FACE_MODEL_ROOT_DIR);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ensure_dir(FACE_MODEL_DIR);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!manifest_matches() || !all_models_match()) {
        ESP_LOGI(TAG, "Face model files missing or outdated, copying bundled models to SD card");
        for (size_t i = 0; i < sizeof(s_models) / sizeof(s_models[0]); ++i) {
            ret = copy_model(&s_models[i]);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        ret = write_manifest();
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Face model files are ready on SD card");
    }

    s_ready = true;
    set_last_error("OK");
    ESP_LOGI(TAG, "Face model dir: %s, database: %s", FACE_MODEL_DIR, FACE_DB_PATH);
    return ESP_OK;
}

bool face_model_storage_is_ready(void)
{
    return s_ready;
}

const char *face_model_storage_model_dir(void)
{
    return FACE_MODEL_DIR;
}

const char *face_model_storage_db_path(void)
{
    return FACE_DB_PATH;
}

const char *face_model_storage_last_error(void)
{
    return s_last_error;
}
