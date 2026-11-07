# esp-dl 3.3.7 人脸数据库 ID 补丁

上游 `DataBase::query_feat()` 返回遍历序号，删除中间用户后，该序号可能与持久化的人脸特征 ID 不一致。本补丁改为返回 `it->id`，保留现有用户映射。

- 上游版本、提交和文件摘要见 `esp-dl-3.3.7-recognition-id.json`。
- 差异见 `esp-dl-3.3.7-recognition-id.patch`；上游 MIT 许可见 `LICENSE.esp-dl`。
- 工具保留 `managed_components` 原件，将完整 esp-dl 复制到被 Git 忽略的 `components/espressif__esp-dl` 后应用补丁。ESP-IDF 会优先采用同名的项目本地组件。
- 工具拒绝错误版本、未知源码和已被其他方式修改的本地副本；重复运行时不会覆盖本地修改。

在 `examples/esp_brookesia_phone` 中，先通过 ESP-IDF 下载依赖，再执行：

```sh
idf.py set-target esp32p4
idf.py reconfigure
python tools/apply_esp_dl_patch.py --check
python tools/apply_esp_dl_patch.py
idf.py reconfigure
```

之后才能运行 `idf.py build`。重新下载依赖或更换 esp-dl 版本后，应重新检查补丁适配性。
