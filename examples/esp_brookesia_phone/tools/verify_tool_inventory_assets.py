#!/usr/bin/env python3
import hashlib
import json
import pathlib
import sys


EXPECTED_MODEL_SIZE = 576_416
EXPECTED_MODEL_SHA256 = "692C2D84DC388A7E689E26A202C9B2F20B5E1D5479D878FCE7F614E58956EA2E"
EXPECTED_MAPPING = [5, 8, 3, 6, 4, 9, 1, 2, 7]
EXPECTED_CLASSES = [1, 7, 2, 3, 5, 0, 6, 6, 4]


def fail(message: str) -> None:
    raise SystemExit(f"tool inventory verification failed: {message}")


def main() -> None:
    root = pathlib.Path(sys.argv[1]).resolve()
    model = root / "components/tool_detect/models/p4/espdet_pico_416_v14_qat.espdl"
    template = root / "components/tool_detect/slot_template.json"

    data = model.read_bytes()
    if len(data) != EXPECTED_MODEL_SIZE:
        fail(f"model size is {len(data)}, expected {EXPECTED_MODEL_SIZE}")
    digest = hashlib.sha256(data).hexdigest().upper()
    if digest != EXPECTED_MODEL_SHA256:
        fail(f"model SHA-256 is {digest}, expected {EXPECTED_MODEL_SHA256}")

    config = json.loads(template.read_text(encoding="utf-8"))
    if config.get("image_size") != [1280, 720]:
        fail("slot template image_size must be [1280, 720]")
    slots = config.get("slots")
    if not isinstance(slots, list) or len(slots) != 9:
        fail("slot template must contain exactly nine slots")
    if [slot.get("slot_no") for slot in slots] != list(range(1, 10)):
        fail("physical slot numbers must be unique and ordered 1..9")
    if [slot.get("class_id") for slot in slots] != EXPECTED_CLASSES:
        fail("slot class order differs from the calibrated template")
    for slot in slots:
        box = slot.get("bbox")
        if not isinstance(box, list) or len(box) != 4 or not all(
            isinstance(value, (int, float)) and 0.0 <= value <= 1.0 for value in box
        ):
            fail(f"slot {slot.get('slot_no')} has an invalid bbox")
        if box[2] <= box[0] or box[3] <= box[1]:
            fail(f"slot {slot.get('slot_no')} has invalid bbox corners")

    matcher = (root / "components/tool_detect/slot_constraint.cpp").read_text(encoding="utf-8")
    mapping_token = "5, 8, 3, 6, 4, 9, 1, 2, 7"
    if mapping_token not in matcher:
        fail(f"physical-to-cabinet mapping must remain {EXPECTED_MAPPING}")

    print(f"tool inventory assets OK: model={len(data)} bytes sha256={digest} slots={len(slots)}")


if __name__ == "__main__":
    main()
