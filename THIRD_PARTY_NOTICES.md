# 第三方代码与资料说明

本项目在 Espressif ESP32-P4 Function EV Board 示例及 ESP-Brookesia 基础上开发。根目录 Apache-2.0 不覆盖已有的第三方许可证；各文件的 SPDX 标记和组件内许可证优先适用。

| 内容 | 位置 | 许可证 / 来源 |
| --- | --- | --- |
| 板级扩展组件与开发板示例 | `examples/common_components/bsp_extra`、`examples/lvgl_demo_v8`、`examples/lvgl_demo_v9` | 保留 Espressif 版权及 Apache-2.0 标记；BSP 组件附带 LICENSE |
| 人脸检测、识别及行人检测组件和随附模型 | `examples/esp_brookesia_phone/components/human_face_detect`、`human_face_recognition`、`pedestrian_detect` | 各组件附带 Espressif MIT LICENSE |
| 摄像头辅助代码 | `examples/esp_brookesia_phone/components/camera_support` | 按文件保留 CC0-1.0 或 Apache-2.0 SPDX 标记 |
| esp-dl 人脸数据库补丁 | `examples/esp_brookesia_phone/patches` | 基于 Espressif esp-dl 3.3.7，MIT，见 `LICENSE.esp-dl` |
| Noto Sans SC 派生的 LVGL 字体数据 | `examples/esp_brookesia_phone/main/fonts` | SIL Open Font License 1.1，见该目录 `OFL.txt`；生成参数保留在字体文件头 |
| 自动下载的 ESP-IDF 组件 | 各示例的 `idf_component.yml` 与 `dependencies.lock` | 不随仓库复制；使用时须遵守下载组件各自的许可证 |

esp-dl 补丁对应上游提交：[`7a3d4c02e8b978b5d4b7ddb23dc68f42e56e83c7`](https://github.com/espressif/esp-dl/tree/7a3d4c02e8b978b5d4b7ddb23dc68f42e56e83c7/esp-dl)。

开发板原始说明保存在 [docs/upstream/README.md](docs/upstream/README.md)。`docs` 中海报和演示文件为历史设计及展示资料，可能包含完整系统设计、阶段性实验数字或概念渲染图；这些内容不代表本仓库包含云端实现，也不代表本次发布复现了其中实验结果。当前固件行为以源码及根目录 README 为准。
