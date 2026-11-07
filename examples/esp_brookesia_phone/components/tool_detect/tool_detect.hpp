#pragma once

#include "dl_detect_base.hpp"
#include "esp_err.h"
#include <vector>

namespace tool_detect {

typedef struct {
    int category;
    float score;
    int box[4];
} detection_t;

typedef struct {
    std::vector<detection_t> detections;
} detection_result_t;

static constexpr int kNumClasses = 8;

class ToolDetector {
public:
    static constexpr float kCandidateScoreThreshold = 0.25f;
    static constexpr float kNmsThreshold = 0.45f;

    explicit ToolDetector(float score_thr = kCandidateScoreThreshold,
                          float nms_thr = kNmsThreshold);
    ~ToolDetector();

    ToolDetector(const ToolDetector &) = delete;
    ToolDetector &operator=(const ToolDetector &) = delete;

    esp_err_t run(const dl::image::img_t &img, detection_result_t *result);

private:
    float m_score_thr;
    float m_nms_thr;
};

} // namespace tool_detect
