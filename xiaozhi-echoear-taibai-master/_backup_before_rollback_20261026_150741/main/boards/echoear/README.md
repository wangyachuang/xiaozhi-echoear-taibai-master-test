# EchoEar 喵伴

## 简介

<div align="center">
    <a href="https://oshwhub.com/esp-college/echoear"><b> 立创开源平台 </b></a>
</div>

EchoEar 喵伴是一款智能 AI 开发套件，搭载 ESP32-S3-WROOM-1 模组，1.85 寸 QSPI 圆形触摸屏，双麦阵列，支持离线语音唤醒与声源定位算法。硬件详情等可查看[立创开源项目](https://oshwhub.com/esp-college/echoear)。

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

### 基本配置
- `Xiaozhi Assistant` → `Board Type` → 选择 `EchoEar`

### UI风格选择

EchoEar 支持多种不同的 UI 显示风格，通过 menuconfig 配置选择：

- `Xiaozhi Assistant` → `Select display style` → 选择显示风格

#### 可选风格

##### 表情动画风格 (Emote animation style) - 推荐
- **配置选项**: `USE_EMOTE_MESSAGE_STYLE`
- **特点**: 使用自定义的 `EmoteDisplay` 表情显示系统
- **功能**: 支持丰富的表情动画、眼睛动画、状态图标显示
- **适用**: 智能助手场景，提供更生动的人机交互体验
- **类**: `emote::EmoteDisplay`

**⚠️ 重要**: 选择此风格需要额外配置自定义资源文件：
1. `Xiaozhi Assistant` → `Flash Assets` → 选择 `Flash Custom Assets`
2. `Xiaozhi Assistant` → `Custom Assets File` → 填入资源文件地址：
   ```
   https://dl.espressif.com/AE/wn9_nihaoxiaozhi_tts-font_puhui_common_20_4-echoear.bin
   ```

##### 默认消息风格 (Enable default message style)
- **配置选项**: `USE_DEFAULT_MESSAGE_STYLE` (默认)
- **特点**: 使用标准的消息显示界面
- **功能**: 传统的文本和图标显示界面
- **适用**: 标准的对话场景
- **类**: `SpiLcdDisplay`

##### 微信消息风格 (Enable WeChat Message Style)
- **配置选项**: `USE_WECHAT_MESSAGE_STYLE`
- **特点**: 仿微信聊天界面风格
- **功能**: 类似微信的消息气泡显示
- **适用**: 喜欢微信风格的用户
- **类**: `SpiLcdDisplay`

> **说明**: EchoEar 使用16MB Flash，需要使用专门的分区表配置来合理分配存储空间给应用程序、OTA更新、资源文件等。

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 EchoEar 连接至电脑，**注意打开电源**，并运行：

```bash
idf.py flash
```

## 新增功能：离线语音唤醒 / 动作感知 / 时钟界面

以下三项功能已在 `echoear` 板级代码中实现（对应立创开源页面乐鑫官方发布特性）。

### 1. 本地语音唤醒（离线 AFE 唤醒词）

- 已默认开启：`sdkconfig` 中 `CONFIG_USE_AFE_WAKE_WORD=y`、`CONFIG_USE_AUDIO_PROCESSOR=y`。
- 唤醒词为 **“你好小智”**，由 ESP-SR AFE 本地引擎识别，无需联网。
- 触发逻辑见 `main/application.cc` 中 `OnWakeWordDetected()`（识别到唤醒词后自动进入对话）。

### 2. 动作与姿态感知（IMU 手势）

- 开关位于 `main/boards/echoear/config.h` 的 `#define BOARD_ENABLE_IMU 1`（默认开启）。
- 实现：`main/boards/echoear/EchoEar.cc` 中的 `imu_task()`，在系统启动 5s 后运行。
- 探测逻辑：复用音频编解码器 I2C 总线（`i2c_bus_`，引脚 SDA=2/SCL=1）自动探测 **BMI270 / BMA423 / MPU6050**（地址 0x68/0x69/0x19），无需额外接线。
- 动作响应：检测到**摇晃 / 翻转**等任意动作（any-motion 能量触发）时，触发屏幕显示 `silly` 表情约 1.2s 后回到 `neutral`，作为动作反馈。
- 注意：`config.h` 中的 `IMU_I2C_SDA_PIN/SCL_PIN` 为早期占位值（13/14），当前 `imu_task` 直接复用 `i2c_bus_`，不依赖这两个宏，请勿据此接线。

### 3. 时钟界面（数字时钟 / 模拟时钟，分页 + 滑动切换）

- 实现：`main/display/emote_display.cc`（`EmoteDisplay` 类）+ `main/boards/echoear/EchoEar.cc` 触摸任务。
- 分页模型（避免时钟与默认表情界面互相遮挡）：
  - **默认页 = 表情界面**：设备空闲（STANDBY）时显示猫脸表情与原有小号时间标签，**不显示全屏时钟**。
  - **时钟页**：通过**在屏幕上滑动（swipe）** 切换进入；此时全屏显示时钟并**隐藏猫脸表情**。
  - 再次滑动可切回表情界面（切回时自动恢复上次表情）。
- 时钟页内的两种界面：
  - **数字时钟**：7 段式大字 `HH:MM` + 日期 `YYYY/MM/DD 周X`（位于屏幕中部）。
  - **模拟时钟**：软件渲染的 RGB565A8 圆形表盘（表圈、12 小时刻度、60 分钟刻度、时/分/秒三根指针，中心红点），绘制于 PSRAM 缓冲区。
  - **在时钟页内单击触摸屏**，即在“数字时钟 ↔ 模拟时钟”之间切换（对应立创页面“通过单击屏幕进行切换”）。
- 滑动检测：`EchoEar.cc` 触摸任务比较按下点与抬起点的位移，阈值按触控坐标量程自适应（`max_coord_seen / 12`，下限 40）。若你的屏幕坐标量程不同导致误触/不灵敏，可调整 `touch_event_task` 中的 `kSwipeFraction`。
- 刷新：随系统每秒一次 `clock_tick` 更新；时间来源为系统时钟（由 OTA/上线时通过服务器 `server_time + timezone_offset` 同步，沿用原有 GMT+0 本地时区约定）。
- 交互说明：默认表情界面下，**单击屏幕发起对话**（同原逻辑，唤醒词“你好小智”或 BOOT 按键亦可）；进入时钟页后单击仅切换数字/模拟。需要默认就显示时钟的旧行为时，可把 `EmoteDisplay::SetStatus` 中 `STANDBY` 分支改为始终 `SHOW_CLOCK`。

## 底座通信协议与控制方法（Rotating Base 251110）

- 底座固件目录：`rotating_base_251110`，主要文件：
  - UART 接收与命令解析：`rotating_base_251110/main/rotating_base.c`
  - 电机角度闭环控制：`rotating_base_251110/components/motor_ctrl/motor_ctrl.c`
  - ESP-NOW 远控接口：`rotating_base_251110/main/espnow_ctrl/*`

### 串口参数
- 端口：`UART_NUM_1`
- 波特率：`115200`
- 帧格式：8字节定长帧

### 帧格式（角度控制命令 0x01）
- Byte0：`0xAA`
- Byte1：`0x55`
- Byte2：保留/地址（上位机可置 `0x00`，底座不解析）
- Byte3：数据长度（固定 `0x03`）
- Byte4：命令码（角度控制：`0x01`）
- Byte5：角度高字节（`uint16_t` 高位）
- Byte6：角度低字节（`uint16_t` 低位）
- Byte7：校验和（`Byte4 + Byte5 + Byte6` 按 8 位求和）
- 底座解析位置：`rotating_base_251110/main/rotating_base.c:120-161`
- EchoEar 侧发送实现：`main/boards/echoear/EchoEar.cc:470-487`（构帧并写串口）；调用点日志：`EchoEar.cc:445`

### 角度语义与范围
- 角度单位：度（`float/uint16`）
- 有效物理范围：`0..300`（由电位器标定 `POT_ADC_MIN=30`、`POT_ADC_MAX=2800` 映射，见 `motor_ctrl.c:253-263`）
- 中位角：`BASE_ANGLE_MID=162`（见 `components/motor_ctrl/include/motor_ctrl.h:13`）
- 控制语义：绝对角度到位控制（PID 闭环），并非速度/持续旋转模式；到位阈值约 ±2°，到位或输出过小会自动停驱动（`motor_ctrl.c:301-309`）。

### 上位机制帧发送建议
- 请在上位机将待发送角度裁剪到 `0..300` 范围；若发送 `>300` 度，底座传感器无法达到，PID 将持续尝试，可能表现为“长时间驱动/追角”。
- 发送节流：底座在驱动电机时会暂时跳过 UART 读取（`rotating_base.c:121-125`），过于频繁的帧无意义；建议命令间隔 ≥100ms。
- EchoEar 当前实现会做基础防抖与冷却，参考：
  - 防抖：角度变化 <10° 且 1.5s 内不重复发送直接忽略（`main/boards/echoear/EchoEar.cc:424-429`）
  - 冷却：唤醒入口旋转后 4s 冷却、监听轮次 1.5s 冷却（`EchoEar.cc:684, 779`）

### 典型流程
- 唤醒入口：在唤醒入口窗口内，上位机根据 DOA 角度触发一次旋转，日志示例 `DOA (wake-entry) azimuth=... rotate_to=...`（`EchoEar.cc:674-687, 680`），随后下发一帧角度命令（`EchoEar.cc:470-487`）。
- 监听轮次：满足语音与幅度门限时，可能触发一次角度校正（带节流与冷却，`EchoEar.cc:767-783`）。

### ESP-NOW 远控（可选）
- 底座支持 ESPNOW 绑定与接收控制数据，绑定事件回调与数据回调见：
  - 绑定流程：`rotating_base_251110/main/espnow_ctrl/espnow_ctrl.c:168-243, 245-256`
  - 数据接收：`espnow_ctrl.c:258-299, 301-306`
- 示例映射：摇杆通道变化映射到三个目标角（105/155/210°），见 `rotating_base_251110/main/rotating_base.c:263-277`。

### 故障与安全
- 电机堵转检测：持续低输出或输出不足会停驱动；若持续异常，打印 `Motor stalled, stop` 并保护停机（`motor_ctrl.c:306-323, 314-316`）。
- 供电与驱动：使能脚 `BSP_MOTOR_EN` 控制电机驱动上电，初始化 PWM 通道 `LEDC_CHANNEL_A/B`（`motor_ctrl.c:184-224`）。

### 参考实现位置索引
- 帧解析与调用：`rotating_base_251110/main/rotating_base.c:120-161`
- 角度闭环控制（PID）：`rotating_base_251110/components/motor_ctrl/motor_ctrl.c:265-333`
- 角度映射（ADC→角度）：`motor_ctrl.c:253-263`
- EchoEar 侧帧发送：`main/boards/echoear/EchoEar.cc:470-487`

## 声源定位与旋转逻辑（DOA）

- 管线：`esp_afe_doa` 提供方位角，采样窗口 512；入口在 `main/boards/echoear/EchoEar.cc:511-543`，每次从编解码器读取一帧并累计到 `doa_buf_` 后调用 `afe_doa_process`。
- 角度计算：将 `azimuth` 归一到 `[-180, 180]` 得到 `angle`，然后得到 `corrected`（可选全局反号 `kDoaInvert`，当前为 `false`），见 `main/boards/echoear/EchoEar.cc:659-688`。
- 触发判定：
  - 仅在识别管线运行且出现 VAD 上升沿时触发；位置 `main/boards/echoear/EchoEar.cc:771`，使用 `Application::GetInstance().GetAudioService().IsAudioProcessorRunning()` 与 `vad && !s_prev_vad`。
  - 延迟与冷却：进入 listening 后首发延迟 ≥150ms；唤醒入口旋转后 4s 冷却、监听轮次 1.5s 冷却。见 `EchoEar.cc:684, 780` 及触发条件行。
  - 幅度与连续帧门限：`kDoaMinPeak=25`、`kDoaMinPeakDiff=15`、连续帧 `kVoiceGateFrames=3`，位置 `EchoEar.cc:517, 520, 532`。
- 旋转策略：
  - 唤醒入口：在 1.5s 入场窗口内（`kEntryWindowMs`），下发一次性角度 `cmd_entry=corrected` 到底座（绝对角），限幅 `±kAngleSoftLimit=120` 与 `±kEntryMaxDelta=60`，位置 `EchoEar.cc:699-706`。
  - 对话 listening：当触发判定通过时，下发一次性角度 `cmdAngle=corrected` 到底座（绝对角），位置 `EchoEar.cc:778-785`。
- 绝对角下发：
  - 统一封装在 `RotateToAbsoluteAzimuth(int angle)`，将角度映射为 `mapped360 = 162 ± angle` 并夹限到 `[30, 270]`，位置 `EchoEar.cc:470-497`。
  - 串口帧构造与发送见 `EchoEar.cc:474-491`。
- 运行时方向自校准：
  - 首次旋转时比较“命令角符号”与“`mapped360` 实际变化符号”，若不一致则翻转底座映射并立即重发同一命令，保证与硬件顺/逆时针定义一致。唤醒入口在 `EchoEar.cc:699-711`，listening 在 `EchoEar.cc:778-785`。
- 日志示例：
  - 唤醒入口：`DOA (wake-entry) azimuth=90.0 rotate_to=60` → `Base UART rotate=60 mapped360=222`
  - listening 触发：`DOA decision (listening): ... corrected=90 cmd=45 prev=60` → `Base UART rotate=45 mapped360=267`
  - 若方向与期望相反，首次旋转将自动校准映射并重发，使后续两阶段方向一致。

### 常见问题与建议
- 若环境噪声导致误触发：可提高 `kDoaMinPeak`/`kDoaMinPeakDiff` 或将 `kVoiceGateFrames` 增至 3+；当前已采用较稳妥的中间值（25/15/3）。
- 若首次旋转仍表现反向：检查是否在边界位（`mapped360` 接近 30/270），或等待自校准完成后再次触发；必要时可切换 `gBaseCWPositive` 或 `kDoaInvert` 做全局反号，但不建议与自校准同时使用。
- 发送节流与防抖：保持命令间隔 ≥100ms；角度变化 <10° 且 1.5s 内重复发送将被抑制，位置 `EchoEar.cc:424-429`。
