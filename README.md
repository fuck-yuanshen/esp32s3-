# ESP32-S3 + ST7789V（2.4 英寸，240×320）

ESP32-S3 离线工具固件：2.4 英寸 ST7789V（240×320，旋转 90° 后横屏 320×240）
+ 五向按键 + LVGL 9.5 界面。功能包括：

- 从 `data.txt` 选择并发送串口数据（UART2，GPIO19/20，115200 8N1）；
- 选择 BLE LIVE UART2 页面，把手机 BLE RX 特征收到的数据实时原样转发到 UART2；
- 通过 BLE 接收数据写入 SPIFFS `data.txt`，重启后自动读取；
- 通过 BLE 上传 BIN 或 Intel HEX 到 `fw.bin`，用 GPIO19/20 和 NRST
  对单 bank STM32F103 进行 SWD 脱机烧录和回读校验。

编译验证目标：ESP-IDF v6.1 / ESP32-S3 / 2 MB Flash。

## 1. 接线

按屏幕排针丝印连接，表中的编号都是 **GPIO 编号**，不是开发板排针序号。
此方案假设 GPIO8～12 没有被开发板上的其他外设占用。

| 屏幕引脚 | ESP32-S3 接线 | 含义 |
| --- | --- | --- |
| GND | GND | 共地 |
| VCC | 3V3，适用于支持 3.3V 供电的模块 | 先核对模块供电规格；芯片逻辑信号使用 3.3V |
| SCL | GPIO12 | SPI SCLK，时钟 |
| SDA | GPIO11 | SPI MOSI，ESP32 向屏幕发送数据 |
| RESET | GPIO8 | 低电平复位，也称 RES / RST |
| AO / A0 | GPIO9 | DC：低电平为命令，高电平为数据 |
| CS | GPIO10 | 低电平片选 |
| BL | 按模块背光电路接电源或背光驱动，见下文 | 默认由外部供电，软件不占用 GPIO |

这里的 SCL/SDA 是 **SPI**，不走 I²C；`AO` 通常是 `A0` 的字体写法，
它是数字 DC 信号。没有 MISO 引脚也能显示，工程仅发送命令和像素。

首次调试使用短杜邦线，默认 SPI 20 MHz；出现花屏可降至 10 MHz。
ESP32-S3 的 GPIO19/20 常用于 USB，GPIO0/3/45/46 涉及启动配置，
部分 Flash/PSRAM 模组占用 GPIO26～37；本方案避开这些引脚。
特殊开发板仍需核对其原理图。

### 背光 BL 的两种接法

**默认方案：外部背光供电。** `LCD_PIN_BL=-1`。如果模块资料明确标注 BL
可直接接 3.3V 且板上已有背光限流/驱动，可将 BL 接 3V3。
如果 BL 直接通向 LED，需要按 LED 正向电压和额定电流配置限流或恒流驱动。
屏幕尺寸和排针名称本身并不说明背光限流电路是否存在。

**可选方案：PWM 调光。** 在 `menuconfig → ST7789 display` 将背光控制
GPIO 设置为 `7`，再接以下一种电路：

- BL 是明确标注的逻辑使能/PWM 输入：GPIO7 接 BL，按模块输入极性配置。
- 裸 LED 背光：GPIO7 驱动外置 MOSFET/三极管或背光驱动器的控制端，
  LED 电流由电源和限流电路承担。N-MOS 低侧方案需要可接出的 LED 阴极，
  栅极加下拉；若 LED 阴极已在模块内接地，应采用适合该模块的高侧驱动。

GPIO7 只承担控制信号，LED 电流由电源/驱动器承担。
PWM 使用 LEDC 低速 timer 0 / channel 0，5 kHz，10 bit。

### 上、下、左、右、确认按键

使用五个独立的常开按钮，每个按钮接在对应 GPIO 和 GND 之间。
输入启用 ESP32-S3 内部上拉：松开为高电平，按下为低电平。

| 功能 | 配置项 | 默认 GPIO |
| --- | --- | --- |
| 上 | `CONFIG_BUTTON_PIN_UP` | GPIO4 |
| 下 | `CONFIG_BUTTON_PIN_DOWN` | GPIO5 |
| 左 | `CONFIG_BUTTON_PIN_LEFT` | GPIO6 |
| 右 | `CONFIG_BUTTON_PIN_RIGHT` | GPIO13 |
| 确认 | `CONFIG_BUTTON_PIN_OK` | GPIO14 |

GPIO7 保留给可选背光控制，按键避开屏幕 GPIO8～12。
该分配假设开发板未将 GPIO4/5/6/13/14 接到其他板载外设。
接线形式为 `GPIO ── 按钮 ── GND`；四脚轻触开关需使用按下才导通的两组触点。
配置入口为 `menuconfig → Navigation buttons`，默认按下/释放消抖均为 30 ms。
每次按下产生一个事件，持续按住不连发，释放稳定后可再次触发。

默认 LVGL 界面中，方向键切换三个色块按钮的焦点，确认键执行当前色块，
预览色块随之变色并增加计数。切换到原生演示后，按键控制下方方块移动和换色。

按键由 10 ms 周期的 esp_timer 回调采样，默认 30 ms 消抖；LVGL 只读取消抖后的稳定按下状态。
按键模块独立于屏幕驱动，业务代码也可以通过 `button_input_poll()` 获取
`BUTTON_PRESS_EVENT(BUTTON_*)` 按下边沿，或用 `button_input_pressed_mask()`
读取当前按住状态。若按键没有反应，先确认接线和串口日志。


### 串口外设与 STM32 烧录接口

| 用途 | ESP32-S3 | 对方 | 说明 |
| --- | --- | --- | --- |
| 普通串口 TX | GPIO19 | 外设 RX（STM32 USART1 用 PA10） | UART2，115200 8N1 |
| 普通串口 RX | GPIO20 | 外设 TX（STM32 USART1 用 PA9） | UART2，115200 8N1 |
| SWD 数据 | GPIO19 | PA13 / SWDIO | 双向 |
| SWD 时钟 | GPIO20 | PA14 / SWCLK，排针可能标 CLK | ESP32-S3 输出 |
| 目标复位（可选） | GPIO16 | NRST | 自动模式默认不需要连接，开启有线复位时才接 |
| 公共地 | GND | GND | 两端使用 3.3 V 逻辑 |

STM32 由目标板自行供电，BOOT0 保持正常启动的低电平，GPIO15 可以留空。
SWO（PB3/JTDO）是可选跟踪输出，烧录接 PA13/SWDIO。

GPIO19/20 需要按用途接到对应的一组目标引脚。手动换线或使用支持双向的
外部硬件切换器，切换时先断开另一组线。直接并接两组线可能让 UART TX 与 SWD
时钟输出对驱。固件会恢复 ESP32-S3 的 UART2 配置，目标侧的接线由外部切换。

应用关闭了 USB Serial/JTAG。给 ESP32-S3 更新固件和查看日志时使用 UART0
或板载 USB 转串口，GPIO19/20 留给外设。

### Flash 分区（2 MB）

| 分区 | 偏移 | 大小 | 内容 |
| --- | --- | --- | --- |
| nvs | 0x9000 | 24 KB | BLE 绑定信息 |
| phy_init | 0xf000 | 4 KB | RF 校准 |
| factory | 0x10000 | 1.5 MB | 应用镜像 |
| storage | 0x190000 | 448 KB | SPIFFS：data.txt + fw.bin |

448 KB 是 SPIFFS 分区的原始容量。可上传大小取决于文件系统开销、已有文件用量和旧镜像大小。上传时新旧镜像会暂时同时占用空间。

## 2. 模块分工

```text
components/st7789_display/   ST7789V SPI 面板驱动（esp_lcd，20 MHz，DMA 行缓冲）
components/button_input/     五向按键扫描与消抖，输出按下边沿/当前掩码
components/lvgl_port/        LVGL 9.5 移植：显示刷新、tick、任务、锁、键盘输入
components/app_ui/           界面外壳与三个屏幕
  app_ui.c                   菜单、屏幕切换、共享控件
  app_ui_send.c              记录发送、UART 模式/计数与接收预览
  app_ui_flash.c             STM32 烧录界面、接线和进度
  app_ui_info.c              系统信息
components/data_store/       SPIFFS 唯一属主：data.txt 表 + fw.bin 上传会话
components/uart_link/        UART2 唯一驱动属主、后台接收任务、DATA/ISP 互斥
components/ble_data/         NimBLE 外设，文本协议与固件分块接收
components/stm32_isp/        STM32 脱机烧录，默认 SWD，可选原 USART 后端
  swd_gpio.c / swd_wire.c     GPIO 适配、SWD 位时序、请求及应答
  arm_dap.c                  SWD DP 与 MEM-AP 访存
  stm32f1_flash.c            识别目标、按页擦除、半字编程、回读校验
  stm32_image.c              BIN 流式读取、Intel HEX 解析
  stm32_programmer_swd.c     镜像和 GPIO 独占、后台任务、错误清理
main/                        启动顺序、板级配置、原生点屏演示（可选）
tools/idf.ps1                固定 ESP-IDF v6.1 环境的构建/烧录包装脚本
tools/test_uart_modes.ps1    主机模拟：串口接收、模式互斥与烧录退出恢复
```

启动顺序：校验按键引脚 → 建屏 → 挂载 SPIFFS 并读 `data.txt` → 初始化共用 UART2、
编程控制引脚 → 启动 BLE → 交给 LVGL 应用。平时 UART2 以 115200 8N1 收发。
开始 SWD 烧录后暂停普通收发，释放 UART2 的 GPIO 映射，将引脚交给 SWD。
烧录结束或出错时会释放 SWD 引脚并恢复 UART2 映射。有线复位模式还会释放目标 NRST。
若 UART2 恢复失败，界面显示 `UART pin restore failed`，普通收发继续保持暂停。
任一外设初始化失败只记录日志，
其余功能继续运行。

驱动独占 SPI2，绘图使用 16 行 DMA 缓冲（320×16×2 = 10240 字节），
不需要 PSRAM 和整屏 framebuffer。

## 3. 配置、编译、烧录

构建和烧录统一走工程脚本（固定 ESP-IDF v6.1 与配套 Python）：

```powershell
.\tools\idf.ps1 build
.\tools\idf.ps1 flash -Port COMx     # COMx 换成 UART0 对应的串口
.\tools\idf.ps1 monitor -Port COMx
.\tools\idf.ps1 size
```

脚本优先读取 `.vscode/settings.json` 的 `idf.currentSetup`，工具目录默认使用
`IDF_TOOLS_PATH`；迁移电脑时可显式传入 `-IdfPath`、`-ToolsPath`、`-PythonEnvPath`。

**烧录口必须是 UART0 排针（TX0/RX0）。** GPIO19/20 已分配给 STM32 ISP，
`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n` 已关闭应用阶段的 USB-Serial/JTAG，
所以原生 USB 口在应用运行时不会出现串口。ROM 下载模式仍可用 USB，但日志只能从 UART0 看。

引脚、波特率、页大小等参数在 `menuconfig` 中修改；`sdkconfig.defaults` 只对新建配置生效，
已有 `sdkconfig` 的优先级更高，改默认值时两处都要同步。

2 MB Flash 的 `factory` 分区为 1.5 MB，当前镜像约 0.87 MB，剩余约 43%。

## 4. 界面与按键

启动后进入主菜单，五向键在菜单项之间移动焦点，确认键进入：

| 菜单 | 内容 |
| --- | --- |
| SEND PAYLOAD / UART2 | 选择记录，显示 DATA/FLASH、波特率、TX/RX 字节数和接收数据 |
| BLE LIVE UART2 | 手机连接 `ESP32S3-TOOL` 后，RX 特征收到的每个 BLE 数据块实时原样发到 UART2 |
| FLASH STM32 | 显示镜像大小和 SWD 引脚，确认键开始烧录，自动识别擦除页大小 |
| SYSTEM INFO | 分辨率、Flash/SPIFFS 占用、记录数、fw 大小、各引脚分配、剩余堆 |

按键为低有效，内部上拉，30 ms 消抖；`lvgl_port` 把上/左映射为上一项、下/右映射为下一项，
确认映射为 LVGL 的 ENTER。每个屏幕使用独立 `lv_group`，切换时重新绑定，焦点不会串屏。

### 普通串口接收与烧录互斥

- 接收任务常驻，不依赖发送页面是否打开；普通发送不会清空待接收数据或临时切换波特率。
- 发送页面每 100 ms 读取一次线程安全快照，显示累计普通 TX/RX 字节数、最新 10 字节 HEX 与 ASCII。
  非打印字节显示为 `.`；RAM 保留最新 256 字节，满后覆盖最旧内容，接收数据不写入 Flash。
- 普通发送默认追加 CRLF；关闭 `CONFIG_UART_LINK_APPEND_CRLF` 后按原始字节发送。
  TX 计数包含实际发出的 CRLF；ISP 协议字节不计入普通 TX/RX 计数。
- 烧录期间普通记录发送按钮禁用；底层发送 API 同时校验 DATA 模式，避免界面切换时发生串口混发。
- UART 接收内容在 BLE LIVE UART2 页面下会通过独立 BLE 数据特征回传手机；命令模式下仍只供本机屏幕显示。
- 不安装两套 UART2 驱动，也不反复删除驱动；共用硬件通过独占模式切换实现两项功能。

### BLE 串口（数据与固件上传）

设备名 `ESP32S3-TOOL`，使用 Nordic UART 形状的服务，手机端任意 BLE 串口调试 App 即可：

| UUID | 属性 | 用途 |
| --- | --- | --- |
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Service | 主服务 |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write | 接收命令行 |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | 返回执行结果 |
| `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | Write/Notify | 固件上传 |
| `6E400005-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | BLE LIVE UART2 的 UART 原始接收数据 |

选择 `BLE LIVE UART2` 后，手机向 `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
写入的字节会直接调用 UART2 原始发送接口，使用 115200 8N1、GPIO19 TX 和 GPIO20 RX。
数据不会写入 `data.txt`，不会追加 CRLF，二进制值、`0x00` 和换行符都会保留。
返回菜单后实时转发关闭，RX 特征恢复为文本命令模式。STM32 烧录占用共享引脚期间，
实时发送接口返回忙状态并丢弃当次 BLE 数据块。

外部设备从 GPIO20 返回的数据由 UART2 接收任务读取，并在实时页面开启且手机已订阅
`6E400005` 时通过 Notify 原样回传。`6E400003` 只承载 READY、错误和命令回复文本。

文本命令（每行一条，`\n` 或 `\r\n` 结尾）：

```text
name|hex:AA 55 01        # 追加一条十六进制记录
name|text:hello          # 追加一条文本记录
name|AA5501              # 裸内容，能按十六进制解析就按 hex
LIST                     # 回读当前记录列表
INFO                     # SPIFFS 用量与 fw.bin 大小
CLEAR                    # 清空记录（保留 fw.bin）
DEL                      # 删除 fw.bin
```

固件上传（写入 `6E400004`，按顺序发）：

```text
FW:BEGIN:12345           # 期望字节数
FW:HEX:0800...           # 每包十六进制数据，可重复多次
FW:END                   # 收尾并校验字节数
```

也可以直接向 `6E400004` 写原始二进制块（不带 `FW:` 前缀），驱动会原样追加。
`data.txt` 格式为每行 `名称|hex:十六进制`；最多 32 条，名称 24 字符，负载 160 字节。
文件通过临时文件 + rename 原子重写，掉电不会留下半截表。

## 5. STM32 脱机烧录

1. 准备 STM32 `.bin` 或 Intel HEX `.hex` 固件。BIN 通常占用空间更小；HEX
   保留了每段数据的目标地址，适合有地址间隔的镜像。烧录器按文件内容自动识别：
   `:` 开头按 Intel HEX 解析，否则按 BIN 处理。HEX 支持数据、EOF、扩展地址记录，
   并校验每条记录的长度与 checksum。

```text
arm-none-eabi-objcopy -O binary app.elf fw.bin
```

2. 通过 BLE 上传固件原始文件字节：`FW:BEGIN:<文件字节数>` → 向固件特征值写入原始分块 → `FW:END`。
   `.hex` 上传 Intel HEX 文件的原始文本字节。收到 `OK fw end` 后，新镜像才会成为 `fw.bin`。断开连接、长度不符或写入失败会保留上一个已提交的镜像。
3. 将 GPIO19 接 PA13/SWDIO、GPIO20 接 PA14/SWCLK，两板共地。
   先断开 GPIO19/20 上的普通串口外设。
4. 进入 `FLASH STM32` 并按一次 `START FLASHING`。自动模式会连接 SWD、暂停共享 UART、
   擦除、写入、回读校验，并通过 Cortex-M3 `SYSRESETREQ` 自动启动新固件。
5. 等待出现 `Verified`。若要使用普通串口，换回串口接线。

无 NRST 接线时保持 `STM32_SWD_MANUAL_RESET=n`、`STM32_SWD_WIRED_RESET=n`，这是当前默认配置。
如果接出 GPIO16 到 STM32 NRST，可在 menuconfig 打开 `STM32_SWD_WIRED_RESET`，仍然只需点击一次开始烧录。

BIN 默认烧到 `0x08000000`，HEX 使用文件内的地址。SWD 按目标容量检查每段范围，
支持识别到的 STM32F1 单 bank 设备 ID `0x412`、`0x410`、`0x414`，页大小为
1 KB 或 2 KB。程序只擦有镜像数据的页，但每个命中页都会整页擦除，页内未提供的数据
会保持擦除值 `0xFF`。受读保护的芯片及 XL 双 bank 设备会被拒绝。
HEX 解析需要 RAM，较大的文件优先使用 BIN。

SWD 协议参考 [Arm DAPLink](https://github.com/ARMmbed/DAPLink/blob/main/source/daplink/cmsis-dap/SW_DP.c)，
Flash 流程参考 [ST PM0075](https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf)
和 [STM32F1 HAL](https://github.com/STMicroelectronics/stm32f1xx-hal-driver/blob/master/Src/stm32f1xx_hal_flash.c)。

## 6. 在其他业务模块中绘图

```c
#include "st7789_display.h"

// display 是初始化成功的屏幕句柄。
ESP_ERROR_CHECK(st7789_display_clear(display, 0x0000));
ESP_ERROR_CHECK(st7789_display_fill_rect(display, 20, 30, 100, 60,
                                       st7789_rgb565(255, 0, 0)));

// 输入是本机 uint16_t RGB565 数值，不需要事先交换字节。
const uint16_t pixels[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
ESP_ERROR_CHECK(st7789_display_draw_rgb565(display, 10, 10, 2, 2, pixels, 2));

// 配置了背光控制 GPIO 时支持 0..100% 亮度。
// ESP_ERROR_CHECK(st7789_display_set_brightness(display, 50));
```

`fill_rect()` 自动裁剪屏幕外的部分，宽高必须大于 0。
`draw_rgb565()` 要求目标矩形完全在屏幕内，并提供足够的源像素；
`stride_pixels` 是源图每行的 uint16_t 元素数。
图像接口负责字节序转换，红色传入 `0xF800`。

## 7. GitHub 代码来源与选择

本次对 GitHub API、raw 和网页的联网请求均超时，因此下表不代表本次已完成在线
检索或下载。官方驱动已通过本机 ESP-IDF v6.1 源码核对，当前工程直接链接该内置
组件；第三方库是供后续查看的既有项目入口，当前工程未集成它们。

| 项目 | 入口 | 用途 |
| --- | --- | --- |
| Espressif ESP-IDF | [ST7789 官方实现](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_lcd/src/esp_lcd_panel_st7789.c) | 当前工程采用，包含 reset、RGB565、窗口地址、旋转、反色等实现 |
| Espressif ESP-IDF | [SPI LCD + LVGL 示例](https://github.com/espressif/esp-idf/tree/v6.1/examples/peripherals/lcd/spi_lcd_touch) | 以后需要图形界面和触摸时参考，需按实际控制器调整 |
| Bodmer/TFT_eSPI | [GitHub 仓库](https://github.com/Bodmer/TFT_eSPI) | Arduino 生态的 SPI TFT 库，需配置控制器和引脚 |
| lovyan03/LovyanGFX | [GitHub 仓库](https://github.com/lovyan03/LovyanGFX) | ESP32 图形库，支持面板和总线配置 |
| adafruit/Adafruit-ST7735-Library | [GitHub 仓库](https://github.com/adafruit/Adafruit-ST7735-Library) | 同时包含 ST7789 系列支持，适合 Arduino 参考 |

本机核对文件：`D:/esp/v6.1/esp-idf/components/esp_lcd/src/esp_lcd_panel_st7789.c`。
官方源码保留在 SDK 内，工程未复制第三方驱动源码。

## 8. 验证边界

ESP-IDF v6.1 构建使用 `.\tools\idf.ps1 build`。以下命令运行主机模拟检查：

```powershell
.\tools\test_uart_modes.ps1
```

依赖本机 Visual C++ 工具链；可用 `-VcVarsPath` 指定 `vcvars64.bat`。
检查覆盖 CRLF 开/关、UART 引脚交接和恢复、Intel HEX 解析、原 USART 后端，
以及 SWD 请求位序、WAIT 重试和超时、FAULT、奇偶校验、DAP 读写、F1 页擦除和校验。
主机检查使用模拟端口和目标芯片。

SWD 握手诊断通过独立的 UART0/CH340（COM24，115200 8N1）输出。
在按住 RESET 和确认松开 RESET 后，各保存一次首笔交易的本地 GPIO 采样：
`DIO low/high` 分别是 SWCLK 低/高半周期结束时的数据电平；
`OE H/T` 的 H 表示主机输出已打开，T 表示主机已释放 SWDIO。
位串从左到右按时间排列，前 8 位应为 `10100101`（LSB-first 的 `0xA5`），
接着是 1 个 turnaround 周期、3 个 ACK 周期。
`CLK mismatches` 统计时钟实际输入电平和要求电平不符的半周期数。
这些采样发生在 ESP32 本地引脚，目标端波形仍需在 STM32 接点测量。
逐位采样先存入 RAM，交易结束后才打印，避免 UART 输出插入 SWD 位周期。
GPIO 适配层在整个交易期间保留输入通路，只切换输出使能位交接总线。
失败时进度条旁显示具体步骤以及原始 ACK/request。

保存 COM24 日志（使用已安装 pyserial 的 Python 环境）：

```powershell
python tools/serial_capture.py --port COM24 --seconds 180 --output build/swd_capture.log
```

该工具以 115200 8N1 打开端口，并将 DTR/RTS 设置为释放状态。
运行前关闭其他占用 COM24 的监视器；Ctrl+C 提前结束并释放串口。

**这是编译与静态尺寸验证，不是实物验证。** 以下项目必须接上板子逐项确认：

1. 屏幕：供电电压、背光限流、SPI 模式、颜色极性（INVON/BGR）和扫描方向；
   花屏先把 SPI 从 20 MHz 降到 10 MHz。
2. 按键：五个 GPIO 是否被开发板占用；上电日志里 `reads low at startup` 说明引脚被拉低。
3. BLE：手机能否发现 `ESP32S3-TOOL`，`LIST`/`INFO` 是否回包，重启后 `data.txt` 是否保留。
4. STM32：接好 PA13、PA14 和 GND，按屏幕提示操作 RESET，再测试已知可运行的点灯镜像。
   UART0 日志会报告 `DP IDCODE`、目标容量和失败阶段。
5. GPIO19/20：确认没有被开发板上的 USB 电路或外设占用。
6. 普通串口：断开目标设备后，临时将 GPIO19 与 GPIO20 短接；进入 SEND PAYLOAD，
   发送已有记录，检查 RX 字节数增长且 HEX/ASCII 出现相同内容。结束后移除短接再接目标设备。
7. 模式切换：验证烧录期间普通数据收发暂停；烧录成功、目标未响应等退出后，
   重新进入发送页面应显示 DATA 115200，普通收发恢复。烧录测试前移除回环短接。

本轮完成编译与主机模拟检查；板上串口收发、屏幕效果和 STM32 实际烧录尚待验证。
