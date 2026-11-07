#include "slot_constraint.hpp"

#include "cJSON.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

extern const uint8_t slot_template_json_start[] asm("_binary_slot_template_json_start");
extern const uint8_t slot_template_json_end[] asm("_binary_slot_template_json_end");

namespace {

constexpr char TAG[] = "tool_slots";
constexpr std::array<uint8_t, tool_detect::kToolSlotCount> PHYSICAL_TO_CABINET = {
    5, 8, 3, 6, 4, 9, 1, 2, 7,
};

float box_iou(const std::array<float, 4> &left, const std::array<float, 4> &right)
{
    const float x1 = std::max(left[0], right[0]);
    const float y1 = std::max(left[1], right[1]);
    const float x2 = std::min(left[2], right[2]);
    const float y2 = std::min(left[3], right[3]);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    if (intersection <= 0.0f) {
        return 0.0f;
    }
    const float left_area = std::max(0.0f, left[2] - left[0]) *
                            std::max(0.0f, left[3] - left[1]);
    const float right_area = std::max(0.0f, right[2] - right[0]) *
                             std::max(0.0f, right[3] - right[1]);
    const float union_area = left_area + right_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

bool read_int(cJSON *object, const char *name, int *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || value == nullptr) {
        return false;
    }
    *value = item->valueint;
    return true;
}

} // namespace

namespace tool_detect {

const char *slot_status_name(SlotStatus status)
{
    switch (status) {
    case SlotStatus::Present: return "PRESENT";
    case SlotStatus::Mismatch: return "MISMATCH";
    case SlotStatus::Missing: return "MISSING";
    default: return "UNKNOWN";
    }
}

SlotConstraint &SlotConstraint::instance()
{
    static SlotConstraint constraint;
    return constraint;
}

esp_err_t SlotConstraint::ensure_loaded()
{
    return m_loaded ? ESP_OK : load_embedded();
}

esp_err_t SlotConstraint::load_embedded()
{
    const size_t json_size = static_cast<size_t>(slot_template_json_end - slot_template_json_start);
    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(slot_template_json_start),
                                        json_size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Embedded slot template is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    cJSON *image_size = cJSON_GetObjectItemCaseSensitive(root, "image_size");
    cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    if (!cJSON_IsArray(image_size) || cJSON_GetArraySize(image_size) != 2 ||
        !cJSON_IsNumber(cJSON_GetArrayItem(image_size, 0)) ||
        !cJSON_IsNumber(cJSON_GetArrayItem(image_size, 1)) ||
        cJSON_GetArrayItem(image_size, 0)->valueint != kSlotTemplateWidth ||
        cJSON_GetArrayItem(image_size, 1)->valueint != kSlotTemplateHeight ||
        !cJSON_IsArray(slots) || cJSON_GetArraySize(slots) != kToolSlotCount) {
        ESP_LOGE(TAG, "Slot template must describe exactly nine slots at 1280x720");
        goto cleanup;
    }

    {
        std::array<bool, kToolSlotCount> seen_slots{};
        std::array<SlotConfig, kToolSlotCount> parsed{};
        cJSON *slot = nullptr;
        cJSON_ArrayForEach(slot, slots) {
            int slot_no = 0;
            int class_id = -1;
            if (!cJSON_IsObject(slot) || !read_int(slot, "slot_no", &slot_no) ||
                !read_int(slot, "class_id", &class_id) || slot_no < 1 ||
                slot_no > kToolSlotCount || class_id < 0 || class_id >= kNumClasses ||
                seen_slots[slot_no - 1]) {
                ESP_LOGE(TAG, "Slot template has an invalid or duplicate slot");
                goto cleanup;
            }

            cJSON *bbox = cJSON_GetObjectItemCaseSensitive(slot, "bbox");
            if (!cJSON_IsArray(bbox) || cJSON_GetArraySize(bbox) != 4) {
                ESP_LOGE(TAG, "Slot %d bbox must contain four coordinates", slot_no);
                goto cleanup;
            }

            SlotConfig config{};
            config.physical_slot_no = static_cast<uint8_t>(slot_no);
            config.cabinet_slot_no = PHYSICAL_TO_CABINET[slot_no - 1];
            config.class_id = class_id;
            for (int coordinate = 0; coordinate < 4; ++coordinate) {
                cJSON *value = cJSON_GetArrayItem(bbox, coordinate);
                if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                    value->valuedouble < 0.0 || value->valuedouble > 1.0) {
                    ESP_LOGE(TAG, "Slot %d has an invalid normalized bbox", slot_no);
                    goto cleanup;
                }
                config.box[coordinate] = static_cast<float>(value->valuedouble);
            }
            if (config.box[2] <= config.box[0] || config.box[3] <= config.box[1]) {
                ESP_LOGE(TAG, "Slot %d bbox has invalid corners", slot_no);
                goto cleanup;
            }
            parsed[slot_no - 1] = config;
            seen_slots[slot_no - 1] = true;
        }

        if (std::find(seen_slots.begin(), seen_slots.end(), false) != seen_slots.end()) {
            ESP_LOGE(TAG, "Slot template is incomplete");
            goto cleanup;
        }
        m_slots = parsed;
        m_loaded = true;
        ret = ESP_OK;
    }

cleanup:
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Embedded 1280x720 slot template validated: slots=%d", kToolSlotCount);
    }
    return ret;
}

esp_err_t SlotConstraint::check_slots(
    const std::vector<detection_t> &detections,
    int image_width,
    int image_height,
    std::array<SlotResult, kToolSlotCount> *results)
{
    if (results == nullptr || image_width != kSlotTemplateWidth ||
        image_height != kSlotTemplateHeight) {
        ESP_LOGE(TAG, "Slot matching requires an exact 1280x720 frame");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t load_ret = ensure_loaded();
    if (load_ret != ESP_OK) {
        return load_ret;
    }

    for (size_t i = 0; i < m_slots.size(); ++i) {
        (*results)[i] = {
            m_slots[i].physical_slot_no,
            m_slots[i].cabinet_slot_no,
            m_slots[i].class_id,
            -1,
            0.0f,
            0.0f,
            SlotStatus::Missing,
        };
    }

    struct Pair {
        size_t slot_index;
        size_t detection_index;
        float iou;
        float score;
    };
    std::array<bool, kToolSlotCount> slot_used{};
    std::vector<bool> detection_used(detections.size(), false);

    for (int phase = 0; phase < 2; ++phase) {
        std::vector<Pair> pairs;
        for (size_t slot_index = 0; slot_index < m_slots.size(); ++slot_index) {
            if (slot_used[slot_index]) {
                continue;
            }
            const auto &slot = m_slots[slot_index];
            for (size_t detection_index = 0; detection_index < detections.size(); ++detection_index) {
                if (detection_used[detection_index]) {
                    continue;
                }
                const auto &detection = detections[detection_index];
                const bool class_matches = detection.category == slot.class_id;
                if ((phase == 0) != class_matches || detection.box[2] <= detection.box[0] ||
                    detection.box[3] <= detection.box[1]) {
                    continue;
                }
                const std::array<float, 4> detection_box = {
                    static_cast<float>(detection.box[0]) / image_width,
                    static_cast<float>(detection.box[1]) / image_height,
                    static_cast<float>(detection.box[2]) / image_width,
                    static_cast<float>(detection.box[3]) / image_height,
                };
                const float overlap = box_iou(slot.box, detection_box);
                if (overlap > kSlotIouThreshold) {
                    pairs.push_back({slot_index, detection_index, overlap, detection.score});
                }
            }
        }

        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair &left, const Pair &right) {
            if (left.iou != right.iou) {
                return left.iou > right.iou;
            }
            return left.score > right.score;
        });
        for (const auto &pair : pairs) {
            if (slot_used[pair.slot_index] || detection_used[pair.detection_index]) {
                continue;
            }
            const auto &detection = detections[pair.detection_index];
            auto &result = (*results)[pair.slot_index];
            result.detected_class_id = detection.category;
            result.confidence = detection.score;
            result.iou = pair.iou;
            result.status = phase == 0 ? SlotStatus::Present : SlotStatus::Mismatch;
            slot_used[pair.slot_index] = true;
            detection_used[pair.detection_index] = true;
        }
    }

    for (const auto &result : *results) {
        ESP_LOGI(TAG,
                 "Slot physical=%u cabinet=%u status=%s expected=%d detected=%d score=%.3f iou=%.3f",
                 result.physical_slot_no,
                 result.cabinet_slot_no,
                 slot_status_name(result.status),
                 result.expected_class_id,
                 result.detected_class_id,
                 result.confidence,
                 result.iou);
    }
    return ESP_OK;
}

} // namespace tool_detect
