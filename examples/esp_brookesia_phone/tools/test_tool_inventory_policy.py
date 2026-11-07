#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from dataclasses import dataclass


ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).parents[1]


@dataclass(frozen=True)
class Detection:
    category: int
    score: float
    box: tuple[float, float, float, float]


def iou(left: tuple[float, float, float, float], right: tuple[float, float, float, float]) -> float:
    x1, y1 = max(left[0], right[0]), max(left[1], right[1])
    x2, y2 = min(left[2], right[2]), min(left[3], right[3])
    intersection = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    left_area = max(0.0, left[2] - left[0]) * max(0.0, left[3] - left[1])
    right_area = max(0.0, right[2] - right[0]) * max(0.0, right[3] - right[1])
    union = left_area + right_area - intersection
    return intersection / union if union > 0.0 else 0.0


def class_aware_nms(candidates: list[Detection], threshold: float) -> list[Detection]:
    kept: list[Detection] = []
    for candidate in sorted(candidates, key=lambda item: item.score, reverse=True):
        if not any(
            candidate.category == accepted.category and iou(candidate.box, accepted.box) > threshold
            for accepted in kept
        ):
            kept.append(candidate)
    return kept


SLOT_CLASSES = [1, 7, 2, 3, 5, 0, 6, 6, 4]
ALWAYS_PRESENT_CLASS = 4


def class_inventory(detections: list[Detection]) -> list[str]:
    counts = [0] * 8
    for detection in detections:
        counts[detection.category] += 1
    results: list[str] = []
    for category in SLOT_CLASSES:
        results.append("PRESENT" if category == ALWAYS_PRESENT_CLASS or counts[category] > 0 else "MISSING")
        counts[category] = max(0, counts[category] - 1)
    return results


class ToolInventoryPolicyTests(unittest.TestCase):
    def test_door_unlock_pulse_is_bounded(self) -> None:
        door = (ROOT / "main/cabinet_door_signal.c").read_text(encoding="utf-8")
        header = (ROOT / "main/cabinet_door_signal.h").read_text(encoding="utf-8")
        self.assertIn("CABINET_DOOR_UNLOCK_PULSE_US (5LL * 1000LL * 1000LL)", door)
        self.assertIn("CABINET_DOOR_CLOSE_PULSE_US (2LL * 1000LL * 1000LL)", door)
        self.assertIn("esp_timer_start_once", door)
        self.assertIn("now_us >= s_pulse_deadline_us", door)
        self.assertIn("Door pulse already active; refusing to extend it", door)
        self.assertIn("cabinet_door_signal_unlock_pulse", header)
        self.assertIn("cabinet_door_signal_close_pulse", header)
        self.assertIn("cabinet_door_signal_lock", header)
        self.assertNotIn("cabinet_door_signal_set_open", header)
        core = (ROOT / "main/cabinet_core.c").read_text(encoding="utf-8")
        self.assertIn("cabinet_door_signal_close_pulse()", core)

    def test_nms_suppresses_only_same_class(self) -> None:
        candidates = [
            Detection(0, 0.95, (0.0, 0.0, 1.0, 1.0)),
            Detection(0, 0.80, (0.05, 0.05, 0.95, 0.95)),
            Detection(1, 0.75, (0.05, 0.05, 0.95, 0.95)),
        ]
        kept = class_aware_nms(candidates, 0.45)
        self.assertEqual([(item.category, item.score) for item in kept], [(0, 0.95), (1, 0.75)])

    def test_two_screwdrivers_fill_two_physical_slots(self) -> None:
        detections = [
            Detection(6, 0.90, (0.0, 0.0, 0.1, 0.1)),
            Detection(6, 0.85, (0.8, 0.8, 0.9, 0.9)),
        ]
        results = class_inventory(detections)
        self.assertEqual(results[6:8], ["PRESENT", "PRESENT"])

    def test_one_detection_cannot_fill_two_slots(self) -> None:
        results = class_inventory([Detection(6, 0.9, (0.2, 0.0, 0.8, 1.0))])
        self.assertEqual(results[6:8].count("MISSING"), 1)

    def test_electronic_pliers_are_always_present(self) -> None:
        results = class_inventory([])
        self.assertEqual(results[8], "PRESENT")

    def test_inventory_ignores_detection_position(self) -> None:
        detections = [
            Detection(category, 0.9, (0.0, 0.0, 0.01, 0.01))
            for category in range(8)
        ] + [Detection(6, 0.8, (0.99, 0.99, 1.0, 1.0))]
        self.assertEqual(class_inventory(detections), ["PRESENT"] * 9)

    def test_detector_source_policy(self) -> None:
        detector = (ROOT / "components/tool_detect/tool_detect.cpp").read_text(encoding="utf-8")
        header = (ROOT / "components/tool_detect/tool_detect.hpp").read_text(encoding="utf-8")
        self.assertIn("candidate.category == accepted.category", detector)
        self.assertIn("m_score_thr,\n            1.0f", detector)
        self.assertIn("kCandidateScoreThreshold = 0.25f", header)
        self.assertIn("kNmsThreshold = 0.45f", header)
        self.assertIn("enable_letterbox(std::array<uint8_t, 3>{114, 114, 114})", detector)

    def test_usb_inventory_flow_policy(self) -> None:
        screen = (ROOT / "main/screens/screen_usb_preview.c").read_text(encoding="utf-8")
        service = (ROOT / "main/tool_detection_service.cpp").read_text(encoding="utf-8")
        uvc = (ROOT / "main/usb_uvc_camera_service.c").read_text(encoding="utf-8")
        core = (ROOT / "main/cabinet_core.c").read_text(encoding="utf-8")

        self.assertIn("cabinet_core_get_capture_wait_remaining_ms", screen)
        self.assertIn("cabinet_core_begin_capture_wait", screen)
        self.assertEqual(screen.count("cabinet_core_start_operation("), 1)
        self.assertIn("cabinet_imu_start_open_detection", screen)
        self.assertIn("cabinet_imu_stop_open_detection", screen)
        self.assertIn("face_recognition_release_idle_resources", screen)
        self.assertIn("CABINET_IMU_DOOR_FULLY_OPEN", screen)
        self.assertNotIn("inventory_closed_early", screen)
        self.assertNotIn("INVENTORY_DOOR_", screen)
        self.assertIn("cabinet_core_abort_operation", screen)
        self.assertIn("bool defer_prepare", screen)
        self.assertIn("Create USB inventory task failed", screen)
        self.assertIn("tool_detection_run_sync_with_snapshot", screen)
        self.assertIn("cabinet_core_apply_vision_inventory(&detection)", screen)
        self.assertIn("cabinet_core_begin_close_grace", screen)
        self.assertNotIn("start_usb_preview_upload", screen)
        self.assertIn("retry_btn_event_cb", screen)
        self.assertIn("screen_operation_set_read_only(true)", screen)
        self.assertIn("frame_width != 1280", core)
        self.assertIn("memcpy(s_current, next, sizeof(s_current))", core)
        self.assertIn("CABINET_VISION_MIN_CONFIDENCE", core)
        self.assertIn("CABINET_CLOSE_GRACE_MS 15000", core)
        self.assertIn("CABINET_CAPTURE_WAIT_MS 15000", core)
        self.assertIn('"door_unconfirmed"', core)
        self.assertIn('"door_closed"', core)
        self.assertNotIn("slot->iou <= 0.3f", core)

        self.assertIn("usb_uvc_camera_capture_rgb888_with_jpeg", service)
        self.assertIn("build_class_inventory_results", service)
        self.assertIn("TOOL_ALWAYS_PRESENT_CLASS_ID = 4", service)
        self.assertIn("always_present || detected", service)
        self.assertNotIn("check_slots(", service)
        self.assertIn("jpeg_alloc_decoder_mem", uvc)
        self.assertIn("JPEG_DEC_ALLOC_OUTPUT_BUFFER", uvc)
        self.assertIn("JPEG_PIXEL_FORMAT_RGB888", uvc)
        self.assertIn("USB_UVC_CAPTURE_WARMUP_FRAMES", uvc)
        self.assertIn("USB_UVC_PREVIEW_TASK_CORE   1", uvc)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(USB_UVC_PREVIEW_IDLE_MS))", uvc)
        self.assertIn("jpeg_decoder_get_info", uvc)
        self.assertIn("jpeg_dec_process", uvc)
        self.assertNotIn("jpeg_decoder_process(s_uvc.jpeg_decoder", uvc)
        self.assertIn("Replacing stale idle UVC device", uvc)
        self.assertIn("same_device", uvc)
        self.assertIn("idle_for_reenumeration", uvc)
        self.assertIn("if (ret == ESP_OK && close_ret != ESP_OK)", uvc)
        decode_at = uvc.index("decode_mjpeg_to_rgb888(item.frame")
        save_at = uvc.index("fwrite(item.frame->data", decode_at)
        return_at = uvc.index("uvc_host_frame_return", save_at)
        self.assertLess(decode_at, save_at)
        self.assertLess(save_at, return_at)

    def test_door_event_cloud_protocol(self) -> None:
        header = (ROOT / "main/cloud_uploader.h").read_text(encoding="utf-8")
        uploader = (ROOT / "main/cloud_uploader.c").read_text(encoding="utf-8")
        core = (ROOT / "main/cabinet_core.c").read_text(encoding="utf-8")
        store = (ROOT / "main/tool_record_store.c").read_text(encoding="utf-8")
        for field in ("operation_id", "related_event_id", "occurred_at",
                      "door_confirmed", "reminder_count", "quantity",
                      "screwdriver_present", "vision_complete"):
            self.assertIn(field, header)
            self.assertIn(field, uploader)
        self.assertIn("#define CLOUD_UPLOADER_EVENT_TYPE_LEN 32", header)
        self.assertIn("char event_type[CLOUD_UPLOADER_EVENT_TYPE_LEN]", header)
        self.assertGreaterEqual(32, len("door_unconfirmed") + 1)
        self.assertIn('sizeof("door_unconfirmed")', uploader)
        self.assertIn('add_optional_string(root, "closed_at", event->closed_at)', uploader)
        self.assertNotIn('cJSON_AddStringToObject(root, "closed_at", event->closed_at)', uploader)
        self.assertIn("heap_caps_calloc(1, sizeof(*event)", core)
        self.assertIn("0, NULL, CABINET_STATE_SOURCE_VISION", core)
        self.assertNotIn("tool_operation_diff_t empty_diff", core)
        self.assertIn("TOOL_RECORD_STORE_V1_MAGIC", store)
        self.assertIn("migrate_v1_store", store)
        self.assertIn("TOOL_RECORD_STORE_BACKUP_PATH", store)

    def test_imu_door_protocol_and_threshold_policy(self) -> None:
        imu = (ROOT / "main/cabinet_imu.c").read_text(encoding="utf-8")
        operation = (ROOT / "main/screens/screen_operation.c").read_text(encoding="utf-8")
        self.assertIn("CABINET_IMU_UART UART_NUM_2", imu)
        self.assertIn("CABINET_IMU_TX_GPIO GPIO_NUM_21", imu)
        self.assertIn("CABINET_IMU_RX_GPIO GPIO_NUM_22", imu)
        self.assertIn("CABINET_IMU_BAUD_RATE 115200", imu)
        self.assertIn("CABINET_IMU_QUERY_INTERVAL_MS 200", imu)
        self.assertIn("CABINET_IMU_OPEN_DELTA_DEG 45.0f", imu)
        self.assertIn("CABINET_IMU_STABLE_TIME_US 1500000LL", imu)
        self.assertIn("CABINET_IMU_SAMPLE_MAX_AGE_US", imu)
        self.assertIn("sample_fresh ? s_imu.tracker.state : CABINET_IMU_DOOR_UNAVAILABLE", imu)
        self.assertIn("CABINET_IMU_FUNCTION_WAKE 0x03", imu)
        self.assertIn("CABINET_IMU_FUNCTION_GET_STATE 0x10", imu)
        self.assertIn("CABINET_IMU_FUNCTION_SET_PARAMS 0x12", imu)
        self.assertIn("prepare_sensor_for_euler", imu)
        self.assertIn("Euler subscription configured", imu)
        self.assertIn("expected_wake_tail", imu)
        self.assertIn("query_length == 56U && wake_length == 56U", imu)
        self.assertIn("run_protocol_self_test", imu)
        self.assertIn("run_tracker_self_test", imu)
        self.assertIn("first legal sample is the closed reference", imu)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", imu)
        self.assertNotIn("CABINET_IMU_DOOR_CLOSING", imu)
        self.assertNotIn("CABINET_IMU_CLOSED_TOLERANCE_DEG", imu)
        self.assertIn("cabinet_core_confirm_close_async", operation)
        self.assertIn("Confirm door close - user triggered", operation)
        self.assertNotIn("cabinet_imu_", operation)
        screen = (ROOT / "main/screens/screen_usb_preview.c").read_text(encoding="utf-8")
        self.assertIn("cabinet_door_signal_unlock_pulse", screen)
        self.assertIn("cabinet_core_begin_capture_wait", screen)
        core = (ROOT / "main/cabinet_core.c").read_text(encoding="utf-8")
        self.assertNotIn("cabinet_imu_", core)

    def test_admin_open_defers_unlock_to_imu_flow(self) -> None:
        admin = (ROOT / "main/screens/screen_admin.c").read_text(encoding="utf-8")
        self.assertNotIn("cabinet_door_signal_set_open(true)", admin)
        self.assertIn('screen_usb_preview_start("管理员", AUTH_TYPE_ADMIN)', admin)
        self.assertIn("ui_manager_goto_screen(SCREEN_USB_PREVIEW)", admin)
        self.assertIn("lv_async_call(open_door_async_cb, NULL)", admin)
        self.assertIn("LV_EVENT_SCREEN_UNLOADED", admin)
        event_body = admin[admin.index("static void open_door_btn_event_cb"):
                           admin.index("static void open_door_async_cb", admin.index("static void open_door_btn_event_cb"))]
        self.assertNotIn("clear_list();", event_body)

    def test_standby_has_no_restart_control(self) -> None:
        standby = (ROOT / "main/screens/screen_standby.c").read_text(encoding="utf-8")
        self.assertNotIn("esp_restart", standby)
        self.assertNotIn("reset_btn", standby)
        self.assertNotIn("LV_SYMBOL_REFRESH", standby)

    def test_nfc_uses_real_pn532_events(self) -> None:
        auth = (ROOT / "main/screens/screen_auth.c").read_text(encoding="utf-8")
        register = (ROOT / "main/screens/screen_register.c").read_text(encoding="utf-8")
        combined = auth + register
        self.assertNotIn("generate_simulated_nfc_uid", combined)
        self.assertNotIn("AUTO_PASS_MS", combined)
        self.assertNotIn("esp_random", combined)
        self.assertIn("nfc_manager_set_event_callback(auth_nfc_event_cb, NULL)", auth)
        self.assertIn("user_manager_find_by_nfc_card(event.card_hex, &record)", auth)
        self.assertIn("nfc_manager_set_event_callback(register_nfc_event_cb, NULL)", register)
        self.assertIn("user_manager_set_nfc(s_data.employee_id, event.card_hex)", register)
        self.assertIn("nfc_manager_save_card_binding(event.card_hex", register)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
