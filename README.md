# 2026年嵌赛乐鑫赛道国家一等奖

基于 **ESP32-P4 Function EV Board、ESP-IDF、LVGL 和 ESP-DL** 的智能工具柜设备端固件。它将人员身份认证、PPE 防护装备检测、柜门控制、工具视觉盘点和借还记录连接成一次完整操作，面向工具领用、归还及 FOD（Foreign Object Debris，外来物）管理场景。

主工程保留了原厂示例目录名 [`examples/esp_brookesia_phone`](examples/esp_brookesia_phone)，实际应用已改为智能工具柜；仓库还保留了独立的 LVGL v8 / v9 开发板示例。

**当前版本说明：** 本仓库提供固件、推理模型和展示资料，云端后台、Web 前端、训练代码及训练数据集需要另行准备。护目镜阈值为 `0`，电子钳被固定判为在位，管理员可绕过 PPE 直接开门；这些现有行为在本次开源中保留，使用前请阅读[当前限制](#当前限制)。

## 功能概览

| 模块 | 当前实现 |
| --- | --- |
| 人员注册 | 注册 PIN 入口、工号和姓名录入、中文输入、人脸录入和 NFC 卡绑定 |
| 身份认证 | 已注册用户选择 NFC 卡或人脸认证，通过其中一种后进入 PPE 检测 |
| PPE 检测 | MIPI-CSI 摄像头预览与采集，在本地运行头盔、护目镜模型，显示检测结果并支持重试 |
| 开门控制 | GPIO48 输出限时开锁脉冲，UART IMU 判断箱盖打开且稳定后进入取放流程 |
| 工具盘点 | USB UVC 摄像头预览、拍照，运行 ESP-DL 工具检测模型，更新 9 个工具状态 |
| 借还结算 | 对比操作前库存与本次盘点，计算借出、归还及异常，在结果页展示 |
| 关门确认 | 15 秒确认窗口，超时产生 `door_unconfirmed` 事件，告警后确认可发送关联的 `door_closed` 事件 |
| 本地记录 | 在 SPIFFS 保存工具状态及最多 100 条操作记录，记录云端同步状态 |
| 网络 | Wi-Fi 扫描、连接、保存网络、待机重连及 SNTP 校时 |
| 云端对接 | HTTP 上传图片、操作事件和库存快照，使用可选的 `X-Device-Key` 请求头 |
| 管理员 | PIN 验证、用户列表与删除，以及绕过身份认证/PPE 的“直接开门”入口 |

### 一次普通用户操作

1. **先完成注册。** 在待机页进入注册，填写工号、姓名，并绑定人脸或 NFC 卡；人脸录入采集 3 个特征样本。
2. **验证身份。** 在待机页选择刷卡或人脸识别。两者是可选入口，无需先刷卡再识别人脸；普通认证等待窗口为 60 秒。
3. **检查 PPE。** 面向 MIPI 摄像头，在预览页点击检测。通过后进入开门流程；检测失败时可调整装备后重试。
4. **打开箱盖。** 启动 IMU 检测并输出最长 5 秒开锁脉冲。IMU 取本轮首个有效 X 轴角度作为基准，要求正向变化至少 45°，并在 2° 范围内稳定约 1.5 秒。因此安装方向和开盖方向需要与代码一致。
5. **取放工具。** 箱盖确认打开后启动 USB 摄像头预览，保留 15 秒操作时间；随后停止预览、拍照并执行工具盘点。
6. **确认关门。** 查看盘点结果，关闭箱盖并在 15 秒内点击“确认关门”。程序输出 2 秒关门控制脉冲，生成本地记录并尝试云端同步。未及时确认时产生未确认关门事件；稍后确认可生成关联恢复事件。
7. **查看结果。** 结果页显示借出、归还、异常和云端同步状态；工具记录页可查看本地记录。

初次使用默认从全部工具在位开始，之后使用持久化状态作为下一次操作的基线。正式操作前应核对实际工具与软件库存。

## 硬件要求与接线

| 硬件 | 用途及当前配置 |
| --- | --- |
| ESP32-P4 Function EV Board | 目标芯片 `esp32p4`；当前 Flash 配置为 16 MB，启用 PSRAM，PSRAM 频率为 200 MHz；不同板卡需按规格调整 |
| 7 英寸 MIPI-DSI 显示屏 | 当前 UI 为 1024 × 600，采用 EK79007 显示驱动及 GT911 触摸适配 |
| SC2336 MIPI-CSI 摄像头 | 人脸、PPE 采集；当前传感器配置为 1280 × 720 RAW8 |
| USB UVC 摄像头 | 工具预览和拍照；优先请求 1280 × 720、30 fps MJPEG，并支持枚举可用 MJPEG 格式，实际模式取决于设备 |
| SD 卡 | 必需于完整流程：保存人脸模型、特征库、注册照片和工具盘点照片，挂载点为 `/sdcard` |
| PN532 UART 模块 | NFC 卡 UID 读取与绑定，UART1，115200 波特率 |
| UART IMU 模块 | 开盖角度判断，UART2，115200 波特率；必须兼容源码的 `0x49 ... 0x4D` 帧协议及命令格式 |
| 门锁驱动电路 | 接收 GPIO48 控制信号；GPIO 是逻辑输出，不能直接驱动电锁负载 |
| 板载 Wi-Fi 协处理器 | ESP32-P4 通过 ESP-Hosted / esp_wifi_remote 使用外部无线能力；需匹配板载无线固件和连接方式 |

外接串口和门锁信号如下，TX/RX 均按 **ESP32-P4 一侧** 标注：

| 功能 | ESP32-P4 引脚 | 外设连接 |
| --- | --- | --- |
| PN532 RX | GPIO4 / UART1 RX | PN532 TX |
| PN532 TX | GPIO5 / UART1 TX | PN532 RX |
| IMU TX | GPIO21 / UART2 TX | IMU RX |
| IMU RX | GPIO22 / UART2 RX | IMU TX |
| 门锁控制 | GPIO48 | 门锁驱动电路逻辑输入；空闲为低，开锁/关门控制为限时高脉冲 |
| 外设参考地 | GND | 外设 GND；供电和电平以模块规格为准 |

USB OTG 的 GPIO20 保留给 D+，不要作为门锁输出。MIPI 摄像头通过板上接口连接，默认 SCCB 为 I2C0、SDA GPIO7、SCL GPIO8。屏幕和板载外围连接请参阅 [Espressif 开发板用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html)。

引脚与协议依据：[NFC](examples/esp_brookesia_phone/main/nfc_manager.c)、[PN532](examples/esp_brookesia_phone/main/pn532_uart.c)、[IMU](examples/esp_brookesia_phone/main/cabinet_imu.c)、[门锁输出](examples/esp_brookesia_phone/main/cabinet_door_signal.c)、[摄像头配置](examples/esp_brookesia_phone/components/camera_support/Kconfig.projbuild)。源码中没有提供可确认的 IMU 商品型号，应先核对协议再选型。

## 环境与快速开始

### 1. 安装环境并获取源码

- 安装 Git、Python 3.10 或更高版本，以及 [ESP-IDF 5.5 系列开发环境](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/get-started/index.html)。
- 主工程最低要求为 **ESP-IDF 5.5.3**；现有 `dependencies.lock` 记录 **5.5.4**，复现锁定环境优先选择 5.5.4。
- Windows 建议使用 ESP-IDF 安装器提供的终端；Linux/macOS 先加载所安装版本的 `export.sh`。执行 `idf.py --version` 确认当前环境。

```sh
git clone https://github.com/liuchang925/esp32-p4-function-ev-board.git
cd esp32-p4-function-ev-board/examples/esp_brookesia_phone
idf.py --version
idf.py set-target esp32p4
idf.py reconfigure
```

首次配置需要联网下载组件。不要只复制 `esp_brookesia_phone` 子目录，构建还依赖相邻的 `../common_components`。仓库没有子模块，无需 `--recursive`。

### 2. 恢复 esp-dl 人脸数据库补丁

主工程固定使用 `espressif/esp-dl == 3.3.7`。它包含一处重要修改：识别返回持久化的人脸 ID，避免删除中间用户后识别结果与用户信息错位。

```sh
python tools/apply_esp_dl_patch.py --check
python tools/apply_esp_dl_patch.py
idf.py reconfigure
```

工具先核对版本、上游文件和补丁摘要，再创建 **被 Git 忽略的本地组件覆盖副本** `components/espressif__esp-dl`，不会修改下载目录中的原件。重复执行会检查已应用状态；版本或文件不匹配时停止，已有本地修改不会被覆盖。详见[补丁说明](examples/esp_brookesia_phone/patches/README.md)。

### 3. 配置开发板与服务

```sh
idf.py menuconfig
```

- `Board Support Package`：核对芯片版本、显示屏及板卡选项。当前默认启用了 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`，新版芯片应按板卡实际情况调整。
- `Video Configuration`：核对摄像头型号及 SCCB 接线。
- `Example Configurations`：保留本工程所需 SD 卡支持。
- `Cloud Uploader`：填写后端地址、设备编号、设备密钥；本地运行可保持地址为空。

配置基线为 [`sdkconfig.defaults`](examples/esp_brookesia_phone/sdkconfig.defaults)，保留了现有工程的板卡及组件设置。生成的 `sdkconfig` 被 Git 忽略，用于保存本机及部署配置。已有 `sdkconfig` 的设置优先；修改 defaults 后不会自动覆盖已有值，可通过 `menuconfig` 调整。

开源版将注册 PIN 和管理员 PIN 都替换为演示值 **`000000`**。使用自定义值时，分别修改 [注册 PIN](examples/esp_brookesia_phone/main/register_pin_gate.c) 和 [管理员 PIN](examples/esp_brookesia_phone/main/screens/screen_admin.c) 中的宏后重新编译；它们目前是源码中的静态比较，不是独立的账户权限系统。

### 4. 编译、烧录与监视

以下命令仍在 `examples/esp_brookesia_phone` 下执行：

```sh
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口，例如 Windows 的 `COM5` 或 Linux 的 `/dev/ttyUSB0`。串口监视退出快捷键为 `Ctrl+]`。当前分区包含 9 MB 应用区和 4 MB SPIFFS 区，须使用符合配置的 Flash 容量。

本次开源整理进行了源码检查和 Python 校验，**未执行固件编译、烧录或上板测试**，实际硬件兼容性和完整运行结果需要使用者验证。

## 首次使用与数据存储

1. 插入可由 BSP 挂载的 SD 卡，连接 MIPI 摄像头、USB 摄像头、PN532、IMU 和门锁驱动，再启动开发板。
2. 启动程序会将固件内的人脸模型准备到 SD 卡；若 SD 卡挂载或模型准备失败，人脸功能不可用。
3. 进入待机页的 Wi-Fi 设置扫描并连接网络。网络凭据保存在设备 NVS，待机时尝试连接已保存网络，联网后启动校时。
4. 进入注册页，输入演示 PIN，填写工号和姓名，再绑定人脸或 NFC。管理员页可查看用户、删除关联信息，以及使用“直接开门”。
5. 按前述普通用户流程执行一次操作。开门开始时保持箱盖关闭，以便 IMU 采集本轮基准；打开后应保持工具在摄像头可见区域，等候盘点完成再关门确认。

| 数据 | 存储位置 |
| --- | --- |
| Wi-Fi、用户资料、NFC 及人脸映射 | 设备 NVS |
| 人脸推理模型 | `/sdcard/espdl_models/p4/` |
| 人脸特征库 | `/sdcard/face.db` |
| 注册人脸照片 | `/sdcard/faces/` |
| 最近一次工具盘点照片 | `/sdcard/tool_detection_last.jpg`，后续盘点可能覆盖 |
| 本地操作记录 | `/spiffs/tool_records.bin` |
| 持久化工具状态 | `/spiffs/tool_states.bin` |

上述运行数据不随仓库分发。未配置后端或断网时，云端同步不会成功，须查看本地记录中的同步状态；当前代码没有完整的持久化失败队列和恢复联网后的自动补传流程。

## 工具清单与视觉规则

当前模型检测 **8 个类别**，对应 **9 件工具**，其中两把螺丝刀共享一个类别。

| 柜位 | 工具编号 | 工具 |
| --- | --- | --- |
| 1 | `SD-001` | 螺丝刀 1 |
| 2 | `SD-002` | 螺丝刀 2 |
| 3 | `SSD-001` | 套筒螺丝刀 |
| 4 | `PL-001` | 老虎钳 |
| 5 | `CR-001` | 压线钳 |
| 6 | `WR-001` | 扳手 |
| 7 | `EP-001` | 电子钳 |
| 8 | `HM-001` | 锤子 |
| 9 | `TM-001` | 卷尺 |

工具模型为 `espdet_pico_416_v14_qat.espdl`，PPE 当前使用 `ppe_qat_v3_416.espdl`。候选框置信度阈值为 `0.25`，同类 NMS 阈值为 `0.45`。工具采集使用 1280 × 720 图像，再由模型预处理执行推理。

当前活动盘点逻辑按类别数量分配给对应柜位，并消耗各检测结果一次；两把螺丝刀按检测数量决定在位数量。虽然保留了槽位模板、物理位置映射和空间匹配辅助代码，**实际活动流程不以框的位置判定工具是否放在正确槽位**。相关实现见 [tool_detection_service.cpp](examples/esp_brookesia_phone/main/tool_detection_service.cpp)。

## 云端接口与配置

本仓库仅包含客户端。后端需要自行实现下列协议，并确保设备能访问该地址。HTTP 成功状态按 `2xx` 处理；图片上传为 `multipart/form-data`，文件字段名为 `file`。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/device/upload-image` | 普通工具盘点流程使用的图片上传接口 |
| POST | `/api/device/events` | 操作、未确认关门及后续关门事件，JSON |
| POST | `/api/device/snapshot` | 工具库存快照，JSON |
| POST | `/api/inspection/upload-and-analyze` | 已保留的巡检上传接口，路径可配置；普通盘点主流程使用上面的通用图片上传接口 |

通过 `menuconfig → Cloud Uploader` 写入部署配置，示例为：

```ini
CONFIG_CLOUD_UPLOADER_URL="http://your-server.example:8088"
CONFIG_CLOUD_UPLOADER_DEVICE_CODE="DEMO-CABINET-001"
CONFIG_CLOUD_UPLOADER_API_KEY="replace-with-your-device-key"
CONFIG_CLOUD_UPLOADER_INSPECTION_UPLOAD_PATH="/api/inspection/upload-and-analyze"
```

开源版的地址、密钥默认均为空，设备编号使用演示值。上面的服务器域名和密钥均为占位符。地址为空时程序跳过云端请求；密钥非空时附加 `X-Device-Key`。当前客户端没有配置 CA 证书或证书包，HTTPS 部署需要补充并验证证书信任配置。

图片上传响应中的 URL 支持以下结构，供后续事件引用：

```json
{"data":{"image_url":"https://your-server.example/images/example.jpg"}}
```

也兼容顶层 `image_url` 或 `url` 字符串。操作事件包含 `event_id`、`device_code`、`operator`、`event_type`、`operation_id`、`related_event_id`、借出/归还/异常清单及关门状态；库存快照包含总数、可用数、槽位数组、图像来源和 `vision_complete`。完整字段以 [cloud_uploader.h](examples/esp_brookesia_phone/main/cloud_uploader.h) 和 [JSON 序列化实现](examples/esp_brookesia_phone/main/cloud_uploader.c) 为准。

## 目录结构

```text
.
├── README.md                         # 项目说明
├── LICENSE                           # Apache-2.0
├── THIRD_PARTY_NOTICES.md             # 第三方许可及资料说明
├── docs/upstream/                    # 原始开发板说明
└── examples/
    ├── common_components/bsp_extra/  # 板级扩展
    ├── esp_brookesia_phone/          # 智能工具柜主工程
    │   ├── main/                    # 初始化、业务、驱动服务
    │   │   ├── screens/             # 认证、注册、管理员、盘点与记录页
    │   │   ├── ui/                  # UI 管理、样式和触摸布局
    │   │   └── fonts/               # 中文字体及覆盖检查
    │   ├── components/              # 摄像头、人脸、PPE、工具模型封装
    │   ├── patches/                 # esp-dl ID 修复补丁及来源摘要
    │   ├── tools/                   # 补丁工具、资源校验及规则测试
    │   ├── docs/                    # 海报及演示资料
    │   ├── spiffs/                  # SPIFFS 镜像目录占位
    │   ├── sdkconfig.defaults       # 脱敏后的默认配置
    │   └── dependencies.lock        # 依赖版本锁定
    ├── lvgl_demo_v8/                # 独立 LVGL v8 示例
    └── lvgl_demo_v9/                # 独立 LVGL v9 示例
```

构建入口位于具体示例目录，仓库根目录不是 ESP-IDF 工程。原来的 `usb_uvc_camera_test` 目录只包含缓存，没有可独立构建的源码，未收入发布版。

## 当前限制

- **PPE 参数仍是当前开发值。** 头盔存在阈值为 `0.1`，护目镜存在阈值为 `0`；采用 `score >= threshold`，因此正常非负护目镜分数即会通过，无法据此确认实际佩戴。阈值定义见 [ppe_detect.hpp](examples/esp_brookesia_phone/components/ppe_detect/ppe_detect.hpp)，本次发布没有调回其他数值。
- **电子钳固定在位。** 活动盘点逻辑将 class 4 作为始终存在类别，无法反映电子钳的真实缺失状态。
- **不严格验证摆放位置。** 同类工具按数量分配，不能据此保证正确柜位，也不能可靠区分同类实物身份。
- **关门依靠人工确认。** IMU 用于判定开盖，后续 `door_confirmed` 来自用户确认操作，不代表独立门磁传感器证明已经闭锁。
- **管理员有旁路入口。** 输入管理员 PIN 后可直接开门，不经过普通用户的身份认证和 PPE 检测。
- **云端为外部依赖。** 没有随附后台、网页、数据库服务、完整自动补传或服务端告警实现；本地同步状态不能等同于后端已收到数据。
- **目前没有量化性能承诺。** 本次未复测识别准确率、延迟和长期稳定性，也没有提供人脸活体检测。静态 PIN、NFC UID 匹配等机制需要按实际使用要求完善。

## 校验与验证范围

在主工程目录可执行以下不依赖开发板的检查：

```sh
python tools/verify_tool_inventory_assets.py .
python tools/test_tool_inventory_policy.py .
python main/fonts/check_font_coverage.py --project-root .
```

本次发布已通过工具模型大小及 SHA-256 校验、槽位资源检查、13 项现有规则测试，以及 6769 个内置字形的 UI 字体覆盖检查。补丁工具另行验证了兼容性检查、重复执行、不兼容版本拒绝及已有修改保护；发布内容和 Git 历史通过密钥扫描。以上检查不替代固件编译和上板测试。

## 资料与许可证

- [开发板原始说明](docs/upstream/README.md)
- [LVGL v8 中文说明](examples/lvgl_demo_v8/README_CN.md) / [LVGL v9 中文说明](examples/lvgl_demo_v9/README_CN.md)
- [项目概念海报](examples/esp_brookesia_phone/docs/esp32-p4-smart-tool-cabinet-poster.png)
- [历史研究展示海报](examples/esp_brookesia_phone/docs/esp32-p4-smart-tool-case-academic-poster.png)
- [软件设计演示文稿](examples/esp_brookesia_phone/docs/ppt/software-design-editable-v2.pptx)

海报和演示文稿保留了历史设计、概念渲染及阶段性实验描述，部分包含本仓库未提供的云端平台和未在本次发布中复现的性能数字。理解当前实现时应以本 README 和实际源码为准。

项目自有贡献采用 [Apache License 2.0](LICENSE)。第三方组件继续适用各自的 MIT、CC0-1.0、Apache-2.0 或 SIL OFL 等许可；详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
