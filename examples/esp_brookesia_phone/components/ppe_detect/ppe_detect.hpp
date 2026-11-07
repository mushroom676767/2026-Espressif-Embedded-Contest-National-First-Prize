#pragma once

#include "dl_detect_base.hpp"
#include "esp_err.h"

namespace ppe_detect {

typedef struct {
    float helmet_score;
    float goggles_score;
} detection_scores_t;

class PpeDetector {
public:
    static constexpr int kHelmetClass = 0;
    static constexpr int kGogglesClass = 1;
    static constexpr float kHelmetPresenceThreshold = 0.1f;
    static constexpr float kGogglesPresenceThreshold = 0;
    static constexpr float kCandidateScoreThreshold = 0.25f;
    static constexpr float kNmsThreshold = 0.45f;

    explicit PpeDetector(float score_thr = kCandidateScoreThreshold,
                         float nms_thr = kNmsThreshold);
    ~PpeDetector();

    PpeDetector(const PpeDetector &) = delete;
    PpeDetector &operator=(const PpeDetector &) = delete;

    esp_err_t run(const dl::image::img_t &img, detection_scores_t *scores);

private:
    float m_score_thr;
    float m_nms_thr;
};

} // namespace ppe_detect
