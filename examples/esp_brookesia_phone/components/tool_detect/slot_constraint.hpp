#pragma once

#include "esp_err.h"
#include "tool_detect.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tool_detect {

constexpr int kToolSlotCount = 9;
constexpr int kSlotTemplateWidth = 1280;
constexpr int kSlotTemplateHeight = 720;
constexpr float kSlotIouThreshold = 0.3f;

enum class SlotStatus : uint8_t {
    Present = 0,
    Mismatch,
    Missing,
};

struct SlotResult {
    uint8_t physical_slot_no;
    uint8_t cabinet_slot_no;
    int expected_class_id;
    int detected_class_id;
    float confidence;
    float iou;
    SlotStatus status;
};

class SlotConstraint {
public:
    static SlotConstraint &instance();

    esp_err_t ensure_loaded();
    esp_err_t check_slots(const std::vector<detection_t> &detections,
                          int image_width,
                          int image_height,
                          std::array<SlotResult, kToolSlotCount> *results);

private:
    struct SlotConfig {
        uint8_t physical_slot_no;
        uint8_t cabinet_slot_no;
        std::array<float, 4> box;
        int class_id;
    };

    SlotConstraint() = default;
    esp_err_t load_embedded();

    std::array<SlotConfig, kToolSlotCount> m_slots{};
    bool m_loaded = false;
};

const char *slot_status_name(SlotStatus status);

} // namespace tool_detect
