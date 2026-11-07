# 贡献指南

[返回项目首页](README.md)

本项目接受文档修正、问题报告、硬件适配和代码改进。开始前请阅读 [README](README.md) 与[开发手册](docs/DEVELOPMENT.md)，特别是当前模型参数、人工关门确认和云端依赖的说明。

## 反馈问题或提出建议

先搜索 [Issues](https://github.com/liuchang925/2026-Espressif-Embedded-Contest-National-First-Prize/issues) 是否已有同类问题，再通过 [New issue](https://github.com/liuchang925/2026-Espressif-Embedded-Contest-National-First-Prize/issues/new) 提交。

问题报告建议包含：

- 板卡、芯片版本、屏幕、摄像头和相关外设型号。
- ESP-IDF 版本、使用的提交编号，以及 esp-dl 补丁是否已应用。
- 可复现的操作步骤、预期结果和实际结果。
- 相关串口日志；必要时附配置片段或截图，移除密码、密钥、NFC UID、人脸照片和人员信息。

提出新功能时，描述使用场景和期望行为。较大的硬件、协议或模型变更可先发 Issue 讨论，便于明确兼容范围。

## 提交 Pull Request

1. Fork 仓库，在自己的 Fork 中创建改动分支。
2. 让一次改动围绕一个清晰的问题；沿用相邻代码风格，保留已有 SPDX、版权及第三方许可声明。
3. 根据改动检查文档、资源或业务规则，并如实记录验证结果。
4. 提交清楚的 Git commit，向本仓库 `main` 分支创建 Pull Request。
5. PR 中说明问题、改动后的行为、验证方式，以及未验证的硬件或功能。

涉及工具盘点规则或资源时，在 `examples/esp_brookesia_phone` 中运行：

```sh
python tools/verify_tool_inventory_assets.py .
python tools/test_tool_inventory_policy.py .
```

涉及 UI 文案或字体时，再检查字形覆盖：

```sh
python main/fonts/check_font_coverage.py --project-root .
```

文档改动检查链接、图片、命令路径和排版即可。涉及固件、驱动或硬件行为时，说明是否完成编译及上板验证；未执行的项目请明确写出，不能用 Python 检查代替硬件验证。

## 资源、配置与许可证

- 不提交 `build`、`managed_components`、生成的本地 esp-dl 覆盖组件、编辑器缓存或个人临时文件。
- 部署配置保存在本机生成的 `sdkconfig`；公开示例使用占位值，不上传真实密码、服务器密钥或设备运行数据。
- 修改模型、阈值、类别映射或静态判定规则时，同步更新说明，并提供适用场景和验证依据。新增模型应说明来源及使用许可。
- 对 esp-dl 的现有修复，请保留[补丁与摘要](examples/esp_brookesia_phone/patches/README.md)的可复现性；不要直接提交整个下载依赖目录。
- 项目自有贡献遵循 [Apache-2.0](LICENSE)，第三方材料继续遵循其原许可，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
