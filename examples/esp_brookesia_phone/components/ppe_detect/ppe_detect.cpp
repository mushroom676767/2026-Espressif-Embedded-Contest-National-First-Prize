#include "ppe_detect.hpp"

#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <new>
#include <string>
#include <vector>

extern const uint8_t ppe_qat_v3_416_espdl_start[]
    asm("_binary_ppe_qat_v3_416_espdl_start");

namespace {

constexpr char TAG[] = "ppe_detect";
constexpr char MODEL_NAME[] = "ppe_qat_v3_416.espdl";
constexpr size_t MIN_INFERENCE_HEAP_HEADROOM = 16 * 1024;
constexpr int MODEL_INPUT_SIZE = 416;
constexpr int PPE_CLASS_COUNT = 4;
constexpr int POSTPROCESS_TOP_K = 100;

const char *category_name(int category)
{
    switch (category) {
    case ppe_detect::PpeDetector::kHelmetClass:
        return "helmet";
    case ppe_detect::PpeDetector::kGogglesClass:
        return "goggles";
    case 2:
        return "head";
    case 3:
        return "face";
    default:
        return "unknown";
    }
}

void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "PPE %s heap: free_8bit=%u largest_8bit=%u free_internal=%u "
             "largest_internal=%u free_psram=%u largest_psram=%u",
             stage,
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

bool has_heap_headroom(const char *stage)
{
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest_block >= MIN_INFERENCE_HEAP_HEADROOM) {
        return true;
    }

    ESP_LOGE(TAG,
             "Not enough heap for PPE %s: largest 8-bit block=%u need>=%u",
             stage,
             static_cast<unsigned int>(largest_block),
             static_cast<unsigned int>(MIN_INFERENCE_HEAP_HEADROOM));
    return false;
}

bool model_loaded(dl::Model *model)
{
    return model != nullptr && model->get_fbs_model() != nullptr &&
           !model->get_inputs().empty() && !model->get_outputs().empty();
}

void log_tensor(const char *role, const std::string &name, dl::TensorBase *tensor)
{
    if (tensor == nullptr) {
        ESP_LOGE(TAG, "PPE %s tensor %s is null", role, name.c_str());
        return;
    }

    ESP_LOGI(TAG,
             "PPE %s tensor name=%s shape=%s dtype=%s exponent=%d bytes=%d",
             role,
             name.c_str(),
             dl::vector_to_string(tensor->shape).c_str(),
             tensor->get_dtype_string(),
             tensor->exponent.get(),
             tensor->get_bytes());
}

esp_err_t validate_model_contract(dl::Model *model)
{
    if (!model_loaded(model)) {
        ESP_LOGE(TAG, "PPE model did not load a valid graph");
        return ESP_ERR_INVALID_RESPONSE;
    }

    dl::TensorBase *input = model->get_input();
    log_tensor("input", "images", input);
    if (input == nullptr || input->data == nullptr || input->dtype != dl::DATA_TYPE_INT8 ||
        input->shape.size() != 4 || input->shape[0] != 1 ||
        input->shape[1] != MODEL_INPUT_SIZE || input->shape[2] != MODEL_INPUT_SIZE ||
        input->shape[3] != 3) {
        ESP_LOGE(TAG, "PPE model input must be [1,416,416,3] NHWC int8");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const auto &outputs = model->get_outputs();
    constexpr int stage_sizes[] = {52, 26, 13};
    for (int stage = 0; stage < 3; ++stage) {
        const std::string score_name = "score" + std::to_string(stage);
        const std::string box_name = "box" + std::to_string(stage);
        const auto score_it = outputs.find(score_name);
        const auto box_it = outputs.find(box_name);
        if (score_it == outputs.end() || box_it == outputs.end()) {
            ESP_LOGE(TAG, "PPE model is missing %s or %s", score_name.c_str(), box_name.c_str());
            return ESP_ERR_INVALID_RESPONSE;
        }

        dl::TensorBase *score = score_it->second;
        dl::TensorBase *box = box_it->second;
        log_tensor("output", score_name, score);
        log_tensor("output", box_name, box);
        const int spatial = stage_sizes[stage];
        const bool valid_score = score != nullptr && score->data != nullptr &&
                                 score->dtype == dl::DATA_TYPE_INT8 &&
                                 score->shape.size() == 4 && score->shape[0] == 1 &&
                                 score->shape[1] == spatial && score->shape[2] == spatial &&
                                 score->shape[3] == PPE_CLASS_COUNT;
        const bool valid_box = box != nullptr && box->data != nullptr &&
                               box->dtype == dl::DATA_TYPE_INT8 &&
                               box->shape.size() == 4 && box->shape[0] == 1 &&
                               box->shape[1] == spatial && box->shape[2] == spatial &&
                               box->shape[3] == 4;
        if (!valid_score || !valid_box) {
            ESP_LOGE(TAG,
                     "PPE stage %d must be NHWC int8: score=[1,%d,%d,4], box=[1,%d,%d,4]",
                     stage,
                     spatial,
                     spatial,
                     spatial,
                     spatial);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    ESP_LOGI(TAG, "PPE model tensor contract validated");
    return ESP_OK;
}

float inclusive_iou(const dl::detect::result_t &left, const dl::detect::result_t &right)
{
    if (left.box.size() != 4 || right.box.size() != 4) {
        return 0.0f;
    }

    const int intersection_left = std::max(left.box[0], right.box[0]);
    const int intersection_top = std::max(left.box[1], right.box[1]);
    const int intersection_right = std::min(left.box[2], right.box[2]);
    const int intersection_bottom = std::min(left.box[3], right.box[3]);
    if (intersection_right < intersection_left || intersection_bottom < intersection_top) {
        return 0.0f;
    }

    const float intersection = static_cast<float>(intersection_right - intersection_left + 1) *
                               static_cast<float>(intersection_bottom - intersection_top + 1);
    const float left_area = static_cast<float>(left.box[2] - left.box[0] + 1) *
                            static_cast<float>(left.box[3] - left.box[1] + 1);
    const float right_area = static_cast<float>(right.box[2] - right.box[0] + 1) *
                             static_cast<float>(right.box[3] - right.box[1] + 1);
    const float union_area = left_area + right_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

std::vector<dl::detect::result_t> class_aware_nms(
    const std::list<dl::detect::result_t> &candidates,
    float threshold)
{
    std::vector<dl::detect::result_t> sorted(candidates.begin(), candidates.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const dl::detect::result_t &left,
                        const dl::detect::result_t &right) {
                         return left.score > right.score;
                     });

    std::vector<dl::detect::result_t> kept;
    kept.reserve(sorted.size());
    for (const auto &candidate : sorted) {
        bool suppressed = false;
        for (const auto &accepted : kept) {
            if (candidate.category == accepted.category &&
                inclusive_iou(candidate, accepted) > threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            kept.push_back(candidate);
        }
    }
    return kept;
}

} // namespace

namespace ppe_detect {

PpeDetector::PpeDetector(float score_thr, float nms_thr) :
    m_score_thr(score_thr), m_nms_thr(nms_thr)
{
}

PpeDetector::~PpeDetector() = default;

esp_err_t PpeDetector::run(const dl::image::img_t &img, detection_scores_t *scores)
{
    if (scores == nullptr || img.data == nullptr || img.width == 0 || img.height == 0 ||
        (img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB888 &&
         img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_BGR888) ||
        m_score_thr < 0.0f || m_score_thr > 1.0f ||
        m_nms_thr < 0.0f || m_nms_thr > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    *scores = {};

    if (!has_heap_headroom("model setup")) {
        return ESP_ERR_NO_MEM;
    }
    log_heap_state("before model setup");

    const char *model_blob = reinterpret_cast<const char *>(ppe_qat_v3_416_espdl_start);
    std::unique_ptr<dl::Model> model(new (std::nothrow) dl::Model(
        model_blob, fbs::MODEL_LOCATION_IN_FLASH_RODATA));
    if (!model_loaded(model.get())) {
        ESP_LOGE(TAG, "Failed to load embedded PPE model: %s", MODEL_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    model->minimize();
    ESP_LOGI(TAG, "PPE model loaded: %s", MODEL_NAME);
    log_heap_state("after model setup");

    esp_err_t contract_status = validate_model_contract(model.get());
    if (contract_status != ESP_OK) {
        return contract_status;
    }

    std::unique_ptr<dl::image::ImagePreprocessor> preprocessor(
        new (std::nothrow) dl::image::ImagePreprocessor(
            model.get(),
            std::array<float, 3>{0.0f, 0.0f, 0.0f},
            std::array<float, 3>{255.0f, 255.0f, 255.0f}));
    if (preprocessor == nullptr) {
        ESP_LOGE(TAG, "Allocate PPE image preprocessor failed");
        return ESP_ERR_NO_MEM;
    }
    preprocessor->enable_letterbox(std::array<uint8_t, 3>{114, 114, 114});

    std::unique_ptr<dl::detect::ESPDetPostProcessor> postprocessor(
        new (std::nothrow) dl::detect::ESPDetPostProcessor(
            model.get(),
            preprocessor.get(),
            m_score_thr,
            1.0f,
            POSTPROCESS_TOP_K,
            std::vector<dl::detect::anchor_point_stage_t>{
                {8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}}));
    if (postprocessor == nullptr) {
        ESP_LOGE(TAG, "Allocate PPE ESPDet postprocessor failed");
        return ESP_ERR_NO_MEM;
    }

    const size_t input_bytes = static_cast<size_t>(img.width) * img.height * 3;
    const char *input_format =
        img.pix_type == dl::image::DL_IMAGE_PIX_TYPE_BGR888 ? "BGR888" : "RGB888";
    ESP_LOGI(TAG,
             "PPE %s input: %ux%u bytes=%u letterbox=[114,114,114]",
             input_format,
             static_cast<unsigned int>(img.width),
             static_cast<unsigned int>(img.height),
             static_cast<unsigned int>(input_bytes));

    preprocessor->preprocess(img);
    model->run();

    if (!has_heap_headroom("postprocess")) {
        return ESP_ERR_NO_MEM;
    }

    postprocessor->clear_result();
    postprocessor->postprocess();
    const std::list<dl::detect::result_t> &decoded =
        postprocessor->get_result(img.width, img.height);
    std::vector<dl::detect::result_t> results = class_aware_nms(decoded, m_nms_thr);

    float class_top_scores[PPE_CLASS_COUNT] = {};
    size_t accepted_count = 0;
    for (const auto &result : results) {
        if (result.category < 0 || result.category >= PPE_CLASS_COUNT ||
            !std::isfinite(result.score) || result.box.size() != 4 ||
            result.box[2] < result.box[0] || result.box[3] < result.box[1]) {
            ESP_LOGW(TAG, "Drop invalid PPE postprocess result");
            continue;
        }

        ++accepted_count;
        ESP_LOGI(TAG,
                 "PPE result category=%d(%s) score=%.6f box=[%d,%d,%d,%d]",
                 result.category,
                 category_name(result.category),
                 result.score,
                 result.box[0],
                 result.box[1],
                 result.box[2],
                 result.box[3]);

        class_top_scores[result.category] =
            std::max(class_top_scores[result.category], result.score);
        if (result.category == kHelmetClass) {
            scores->helmet_score = std::max(scores->helmet_score, result.score);
        } else if (result.category == kGogglesClass) {
            scores->goggles_score = std::max(scores->goggles_score, result.score);
        }
    }

    ESP_LOGI(TAG,
             "PPE detections decoded=%u class_aware_kept=%u",
             static_cast<unsigned int>(decoded.size()),
             static_cast<unsigned int>(accepted_count));
    ESP_LOGI(TAG,
             "PPE class top scores helmet=%.6f goggles=%.6f head=%.6f face=%.6f",
             class_top_scores[kHelmetClass],
             class_top_scores[kGogglesClass],
             class_top_scores[2],
             class_top_scores[3]);
    ESP_LOGI(TAG,
             "PPE selected scores helmet=%.6f goggles=%.6f thresholds helmet=%.2f goggles=%.2f",
             scores->helmet_score,
             scores->goggles_score,
             kHelmetPresenceThreshold,
             kGogglesPresenceThreshold);

    return ESP_OK;
}

} // namespace ppe_detect
