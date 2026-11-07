#include "tool_detect.hpp"

#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>

extern const uint8_t tool_detect_espdl_start[] asm("_binary_tool_detect_espdl_start");

namespace {

constexpr char TAG[] = "tool_detect";
constexpr char MODEL_NAME[] = "espdet_pico_416_v14_qat.espdl";
constexpr size_t MIN_INFERENCE_HEAP_HEADROOM = 16 * 1024;
constexpr int MODEL_INPUT_SIZE = 416;
constexpr int POSTPROCESS_TOP_K = 100;

float inclusive_iou(const tool_detect::detection_t &left,
                    const tool_detect::detection_t &right)
{
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

std::vector<tool_detect::detection_t> class_aware_nms(
    std::vector<tool_detect::detection_t> candidates,
    float threshold)
{
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const tool_detect::detection_t &left,
                        const tool_detect::detection_t &right) {
                         return left.score > right.score;
                     });

    std::vector<tool_detect::detection_t> kept;
    kept.reserve(candidates.size());
    for (const auto &candidate : candidates) {
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

const char *category_name(int category)
{
    switch (category) {
    case 0: return "tape_measure";
    case 1: return "crimping_pliers";
    case 2: return "socket_screwdriver";
    case 3: return "wrench";
    case 4: return "diagonal_pliers";
    case 5: return "slip_joint_pliers";
    case 6: return "screwdriver";
    case 7: return "hammer";
    default: return "unknown";
    }
}

void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "Tool %s heap: free_8bit=%u largest_8bit=%u free_internal=%u "
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
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= MIN_INFERENCE_HEAP_HEADROOM) {
        return true;
    }
    ESP_LOGE(TAG,
             "Not enough heap for tool %s: largest=%u need>=%u",
             stage,
             static_cast<unsigned int>(largest),
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
        ESP_LOGE(TAG, "Tool %s tensor %s is null", role, name.c_str());
        return;
    }
    ESP_LOGI(TAG,
             "Tool %s tensor name=%s shape=%s dtype=%s exponent=%d bytes=%d",
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
        return ESP_ERR_INVALID_RESPONSE;
    }

    dl::TensorBase *input = model->get_input();
    log_tensor("input", "images", input);
    if (input == nullptr || input->data == nullptr || input->dtype != dl::DATA_TYPE_INT8 ||
        input->shape.size() != 4 || input->shape[0] != 1 ||
        input->shape[1] != MODEL_INPUT_SIZE || input->shape[2] != MODEL_INPUT_SIZE ||
        input->shape[3] != 3) {
        ESP_LOGE(TAG, "Tool model input must be [1,416,416,3] NHWC int8");
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
            ESP_LOGE(TAG, "Tool model is missing %s or %s", score_name.c_str(), box_name.c_str());
            return ESP_ERR_INVALID_RESPONSE;
        }

        dl::TensorBase *score = score_it->second;
        dl::TensorBase *box = box_it->second;
        log_tensor("output", score_name, score);
        log_tensor("output", box_name, box);
        const int spatial = stage_sizes[stage];
        const bool valid_score = score != nullptr && score->data != nullptr &&
                                 score->dtype == dl::DATA_TYPE_INT8 && score->shape.size() == 4 &&
                                 score->shape[0] == 1 && score->shape[1] == spatial &&
                                 score->shape[2] == spatial && score->shape[3] == tool_detect::kNumClasses;
        const bool valid_box = box != nullptr && box->data != nullptr &&
                               box->dtype == dl::DATA_TYPE_INT8 && box->shape.size() == 4 &&
                               box->shape[0] == 1 && box->shape[1] == spatial &&
                               box->shape[2] == spatial && box->shape[3] == 4;
        if (!valid_score || !valid_box) {
            ESP_LOGE(TAG, "Tool stage %d output tensor contract mismatch", stage);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "Tool model tensor contract validated");
    return ESP_OK;
}

} // namespace

namespace tool_detect {

ToolDetector::ToolDetector(float score_thr, float nms_thr) :
    m_score_thr(score_thr), m_nms_thr(nms_thr)
{
}

ToolDetector::~ToolDetector() = default;

esp_err_t ToolDetector::run(const dl::image::img_t &img, detection_result_t *result)
{
    if (result == nullptr || img.data == nullptr || img.width == 0 || img.height == 0 ||
        img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB888 ||
        m_score_thr < 0.0f || m_score_thr > 1.0f ||
        m_nms_thr < 0.0f || m_nms_thr > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    result->detections.clear();

    if (!has_heap_headroom("model setup")) {
        return ESP_ERR_NO_MEM;
    }
    log_heap_state("before model setup");

    const char *model_blob = reinterpret_cast<const char *>(tool_detect_espdl_start);
    std::unique_ptr<dl::Model> model(new (std::nothrow) dl::Model(
        model_blob, MODEL_NAME, fbs::MODEL_LOCATION_IN_FLASH_RODATA));
    if (!model_loaded(model.get())) {
        ESP_LOGE(TAG, "Failed to load embedded tool model: %s", MODEL_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    model->minimize();
    ESP_LOGI(TAG, "Tool model loaded: %s", MODEL_NAME);
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
        return ESP_ERR_NO_MEM;
    }

    preprocessor->preprocess(img);
    model->run();
    if (!has_heap_headroom("postprocess")) {
        return ESP_ERR_NO_MEM;
    }

    postprocessor->clear_result();
    postprocessor->postprocess();
    const std::list<dl::detect::result_t> &results =
        postprocessor->get_result(img.width, img.height);
    ESP_LOGI(TAG,
             "Tool RGB888 input: %ux%u bytes=%u letterbox=[114,114,114]",
             static_cast<unsigned int>(img.width),
             static_cast<unsigned int>(img.height),
             static_cast<unsigned int>(img.width * img.height * 3U));

    std::vector<detection_t> candidates;
    candidates.reserve(results.size());
    for (const auto &detected : results) {
        if (detected.category < 0 || detected.category >= kNumClasses ||
            !std::isfinite(detected.score) || detected.box.size() != 4 ||
            detected.box[0] < 0 || detected.box[1] < 0 ||
            detected.box[2] < detected.box[0] || detected.box[3] < detected.box[1] ||
            detected.box[2] >= static_cast<int>(img.width) ||
            detected.box[3] >= static_cast<int>(img.height)) {
            ESP_LOGW(TAG, "Drop invalid tool detection");
            continue;
        }

        detection_t output = {};
        output.category = detected.category;
        output.score = detected.score;
        for (int coordinate = 0; coordinate < 4; ++coordinate) {
            output.box[coordinate] = detected.box[coordinate];
        }
        candidates.push_back(output);
    }

    result->detections = class_aware_nms(std::move(candidates), m_nms_thr);
    int class_counts[kNumClasses] = {};
    float class_top_scores[kNumClasses] = {};
    for (const auto &detected : result->detections) {
        class_counts[detected.category]++;
        class_top_scores[detected.category] =
            std::max(class_top_scores[detected.category], detected.score);
        ESP_LOGI(TAG,
                 "Tool result category=%d(%s) score=%.6f box=[%d,%d,%d,%d]",
                 detected.category,
                 category_name(detected.category),
                 detected.score,
                 detected.box[0], detected.box[1], detected.box[2], detected.box[3]);
    }

    ESP_LOGI(TAG,
             "Tool detections decoded=%u class_aware_kept=%u",
             static_cast<unsigned int>(results.size()),
             static_cast<unsigned int>(result->detections.size()));
    for (int category = 0; category < kNumClasses; ++category) {
        ESP_LOGI(TAG,
                 "Tool class %d(%s): count=%d top_score=%.6f",
                 category,
                 category_name(category),
                 class_counts[category],
                 class_top_scores[category]);
    }
    return ESP_OK;
}

} // namespace tool_detect
