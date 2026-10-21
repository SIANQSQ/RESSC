# Rocket Engine Support Structure Controller

基于 **ESP32-S3** 的四轴步进电机滑台控制项目，提供 Wi-Fi HTTP 控制接口和 Python 桌面上位机，可用于桁架实验装置、四轴滑台及多电机定位控制原型。

通过桌面滑块或 HTTP 请求，即可为四个轴设置以毫米为单位的绝对位置，并查看运动状态。仓库包含控制固件、硬件配置、桌面控制程序和主机端测试代码。

## 功能特点

- **四轴定位**：支持单轴控制和一条指令下发四轴目标，各轴独立加减速。
- **毫米坐标**：按丝杆导程、驱动器每圈脉冲数换算位置，校验目标行程与脉冲频率。
- **局域网控制**：提供 HTTP GET / JSON 接口，支持状态查询、定位、停止和运行参数设置。
- **桌面上位机**：基于 Tkinter，提供四轴滑块、数值输入、同步拖动、可选自动发送和状态显示；支持离线演示。
- **上电动作自检**：默认四轴共同前进 10 mm，再返回原点。
- **任务分离**：运动与网络任务通过队列交换命令和状态快照，配有 RGB 状态指示。

> 本项目使用手动原点与开环步进控制。每次上电或复位前，需将四个滑台放到各自的物理原点；固件将启动位置记为 0。位置反馈根据输出脉冲累计计算，无法检测丢步、卡滞或人为移动，当前不使用限位开关与编码器。

## 仓库结构

```text
TrussController/
├── src/main.cpp                       # 运动控制、上电自检与任务调度
├── include/
│   ├── rail_config.h                  # 行程、导程、每圈脉冲数与自检参数
│   ├── device_config.h                # 网络与 RGB 配置入口
│   ├── device_config.local.h.example  # 本地配置模板
│   └── network_control.h              # HTTP 接口与状态指示
├── master/
│   ├── slider_controller.py           # Python 桌面上位机
│   ├── http_client.py                 # HTTP 命令行客户端
│   ├── start_controller.cmd           # Windows 启动脚本
│   ├── demo_controller.cmd            # 离线演示启动脚本
│   └── SLIDER_CONTROLLER.md           # 上位机详细操作说明
├── test/                              # 上位机与固件主机端测试
└── platformio.ini                     # PlatformIO 构建配置
```

## 快速开始

### 1. 准备环境

- ESP32-S3 开发板、四路 PUL/DIR 步进驱动器及配套电机、滑台和电源。
- PlatformIO，用于编译与烧录 Arduino 固件。
- Python 3.9 或更新版本，包含 Tkinter；上位机无需第三方 Python 依赖。
- 电脑和设备连接同一局域网。

当前 PlatformIO 板型为 `4d_systems_esp32s3_gen4_r8n16`，默认环境为 `esp32s3_uart`。请按实际开发板确认板型、GPIO 和 USB 连接方式；默认串口 `COM10` 也需按本机情况调整。

### 2. 配置设备

在仓库根目录执行：

```powershell
Copy-Item include/device_config.local.h.example include/device_config.local.h
```

编辑生成的 `include/device_config.local.h`，填写 Wi-Fi 名称、密码、设备主机名和 RGB 引脚。该文件已加入 `.gitignore`，用于保存本地配置。

模板主机名为 `esp32-stepper`，对应地址 `http://esp32-stepper.local`；未覆盖时，固件默认主机名为 `truss-ctrl`。也可以直接使用设备的局域网 IP，上位机连接地址应与实际配置一致。

接着检查 [include/rail_config.h](include/rail_config.h)，确保每轴行程、导程、方向和每圈脉冲数与机械装置、驱动器拨码一致。

### 3. 编译与烧录

```powershell
pio run -e esp32s3_uart
pio run -e esp32s3_uart -t upload
pio device monitor -e esp32s3_uart
```

使用原生 USB CDC 连接时，可按实际板卡情况选择另一个环境：

```powershell
pio run -e 4d_systems_esp32s3_gen4_r8n16
```

上电前先将四轴放到物理原点，并为自动前进 10 mm 的自检动作留出空间。等待自检通过、四轴空闲且原点有效后，再发送定位指令。

### 4. 启动桌面上位机

Windows 可双击 `master/start_controller.cmd`，也可在仓库根目录执行：

```powershell
python master/slider_controller.py --base http://192.168.1.100
```

将示例 IP 替换为设备地址，点击“连接设备”。调整目标后点击“发送四轴目标”；界面位置单位为 **cm**，发送到固件时自动换算为 **mm**。

无需硬件即可预览界面：

```powershell
python master/slider_controller.py --demo
```

详细操作见 [上位机使用说明](master/SLIDER_CONTROLLER.md)。其中若出现旧的 `examples/` 命令路径，请使用当前仓库的 `master/` 路径。

## 硬件与运动配置

### 默认引脚

| 轴 | PUL GPIO | DIR GPIO | 行程 | 每电机圈位移 | 每圈脉冲数 |
|---|---:|---:|---:|---:|---:|
| M1 | 4 | 5 | 100 mm | 2 mm | 1600 |
| M2 | 6 | 7 | 100 mm | 2 mm | 1600 |
| M3 | 9 | 10 | 100 mm | 2 mm | 1600 |
| M4 | 11 | 12 | 100 mm | 2 mm | 1600 |

引脚为 GPIO 编号。驱动器使用 **PUL + DIR** 模式，当前固件没有 ENA 输出控制。接线时需按实际驱动器手册确认信号电压、电流、有效边沿和供电范围；ESP32 为 3.3 V GPIO，需要时通过匹配的电平转换或驱动电路连接。

### 参数说明

持久配置位于 [include/rail_config.h](include/rail_config.h)，修改后需重新编译烧录。

| 参数 | 当前值 / 含义 |
|---|---|
| `MOTOR1_PULSES_PER_REV` ～ `MOTOR4_PULSES_PER_REV` | 各轴 1600 脉冲/圈，必须与驱动器一致 |
| `lengthMm` | 各轴最大行程，默认 100 mm |
| `mmPerRev` | 每电机圈对应位移，默认 2 mm，需包含减速比影响 |
| `positiveDirLevel` | 位置增加时的方向电平，默认 `HIGH` |
| `SELF_TEST_ON_BOOT` | 默认 `true`，启用上电动作自检 |
| `SELF_TEST_DISTANCE_MM` | 自检前进 10 mm，再返回 0 mm |
| `SELF_TEST_RPM` | 自检转速 50 RPM |
| `SELF_TEST_TIMEOUT_US` | 每段动作超时 120 秒 |

普通定位默认转速为 **15 RPM**，默认加速度为 **800 脉冲/s²**。每轴脉冲频率上限为 **2000 Hz**，最高允许转速为 `120000 / 每圈脉冲数`；当前 1600 脉冲/圈对应 75 RPM。

位移换算公式为 `脉冲数 = 位移 mm ÷ mmPerRev × pulsesPerRev`。当前配置下，10 mm 对应 8000 脉冲，指令分辨率为 0.00125 mm；这并不代表实际机械精度。修改驱动器细分时，应填写面板标注的 **pulse/rev**，而不是细分倍率。

## HTTP 接口

接口基地址为 `http://设备IP` 或配置的 `.local` 地址，端口为 80。位置参数均为 **从原点算起的绝对位置，单位 mm**，转速单位为 **RPM**。

| 功能 | GET 路径示例 |
|---|---|
| 查询设备与四轴状态 | `/api/status` |
| 单轴定位 | `/api/move?motor=2&mm=25.5&rpm=15` |
| 四轴定位 | `/api/move_all?m1=10&m2=20&m3=30&m4=40&rpm=15` |
| 全部停止 | `/api/stop` |
| 单轴停止 | `/api/stop?motor=2` |
| 手动确认单轴零位 | `/api/zero?motor=2` |
| 设置单轴默认转速 | `/api/set?motor=2&param=rpm&value=15` |
| 设置单轴每圈脉冲数 | `/api/set?motor=2&param=ppr&value=1600` |
| 设置单轴加速度 | `/api/set?motor=2&param=accel&value=800` |
| 查询指令执行结果 | `/api/result?id=1` |

四轴定位必须同时提供 `m1`～`m4`；任一轴的目标、速度或状态不符合要求，整组指令将被拒绝。四轴共用启动时刻，各自加减速，不保证同时到达。重复发送同一绝对位置不会累加位移。

控制请求先返回 HTTP `202`，表示指令已入队：

```json
{"accepted":true,"id":1,"result_url":"/api/result?id=1"}
```

随后查询 `result_url`：`202` 表示仍在排队，`200` 表示指令执行或运动启动成功，**不代表已经到位**。通过 `/api/status` 检查 `moving`、`position_mm`、`target_mm`、`homed` 和 `fault` 等字段判断后续状态。自检启用时，应等待 `self_test="passed"`；关闭自检时为 `disabled`。

`/api/set` 的设置仅保存在 RAM 中，重启后恢复代码配置。更改 PPR 会使该轴原点失效，需手动放回原点后调用 `/api/zero`。`zero` 仅记录当前位置为 0，不会移动电机；当前不支持 `/api/home` 自动寻找原点。

命令行客户端示例（显式指定设备地址）：

```powershell
python master/http_client.py --base http://192.168.1.100 status
python master/http_client.py --base http://192.168.1.100 move_all m1=10 m2=20 m3=30 m4=40 rpm=15
python master/http_client.py --base http://192.168.1.100 stop
```

## 使用边界

- 上电自检用于验证固件的发脉冲流程，无法确认电机实际移动或接线正确。首次运行需实机核对方向、距离和驱动器配置。
- 软件行程限制依赖正确原点和未丢步的前提。位置失去可信度后，应重新手动确认原点。
- STOP 立即停止脉冲，无减速过程；网络停止受通信和队列延迟影响，不能替代硬件急停。自检期间停止任一轴会取消整个自检并停止四轴。
- 关闭上位机或断开连接不会自动停止已启动的运动；网络超时后应先查询状态，再决定后续操作。
- 仓库提供主机端模拟测试，见 [test/host/README.md](test/host/README.md)。模拟测试无法替代实际接线、脉冲波形、带载运动及 Wi-Fi 环境验证。
