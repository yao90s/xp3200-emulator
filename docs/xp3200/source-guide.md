# 源码结构与阅读指南

本文说明 XP3200 模拟器的代码组织，以及从零开始读源码的推荐顺序。
代码主体是 `hw/s1c33/xp3200.c`（约 9000 行），其余是 QEMU 11 的公共代码。

## 1. 仓库结构

```
xp3200-emulator/
├── hw/s1c33/
│   ├── xp3200.c          XP3200 machine（本项目新增，主战场）
│   ├── bbk9288s.c        9288 machine（上游遗留，保留）
│   ├── Kconfig           注册 CONFIG_XP3200 / CONFIG_BBK9288S
│   ├── meson.build       编译入口
│   ├── s1c33.c           S1C33 总线/中断等公共部分
│   └── ...
├── target/s1c33/         S1C33 CPU 翻译（translate.c）、指令 helper、反汇编
├── docs/xp3200/          硬件连接、键盘映射、SD 卡工具、移植进度
├── scripts/xp3200/       SD 卡镜像工具（GUI + CLI + FAT16 解析）
├── include/hw/s1c33/     CPU/机器头文件
└── （其余为 QEMU 11 原有目录）
```

> 仓库由 9288 模拟器 fork 而来，`bbk9288s.c`、`run-bbk9288*`、`web/` 等
> 9288 专用文件仍保留，与 XP3200 互不影响。

## 2. `xp3200.c` 分段地图

行号随改动会漂移，按“区域标题”搜索更稳。

| 行号（约） | 内容 |
|---|---|
| 1–330 | 头文件；地址/尺寸定义（SDRAM、NAND、LCDC、SPI、音频、按键端口） |
| 341–500 | 键盘表：`xp3200_kbd_col_bit[]`（物理列→掩码位）、`xp3200_kbd_map[]`（PC 键→行列） |
| 500–820 | 状态结构体 `XP3200SState`、machine 状态、属性字段 |
| 857–1200 | 寄存器表 `xp3200s_regs[]`（地址、名字、分组、掩码）与中断对表 |
| 1204–1355 | 寄存器查找、名字工具 |
| 1355–1610 | 当前 PC 读取、IRQ 选择/更新 |
| 1610–1940 | USB/ADC 因子、RTC 初始化、时钟定时器、SD 插卡定时器 |
| 1947–2045 | 端口输入信号（K5/K6 电平、触摸、外部端口） |
| 2043–2270 | 键盘任意键、唤醒线、单键行码映射、扫描码 |
| 2230–2340 | 矩阵刷新（`xp3200_keyboard_refresh_matrix`）与单键行码刷新 |
| 2340–2520 | 按键事件入口（矩阵/单键行码、按键栈、F12 后门） |
| 2520–3900 | 触摸串口、NAND 设备（复位/读写/ECC）、LCD 等 |
| 3899 | 键盘比较器 `xp3200s_key_input_update` |
| 4100–4500 | K5/K6 读（`xp3200s_k_input_data_read_impl`）、端口读 |
| 4471–4600 | IO 字节读写、背光 |
| 4600–4920 | 复位、实例初始化 |
| 4922–5600 | NAND 设备读写、LCD 刷新、其他外设 |
| 5618–5890 | 音频：SDI 数据、SCI 命令、FIFO/DREQ |
| 5891–6300 | SD 卡状态机（`xp3200s_sd_command`/`xp3200s_sd_xfer`）、SPI 块 |
| 6300–6900 | 音频播放器、NAND 存储、杂项 |
| 7387–7500 | NAND IO 读写实现 |
| 8064–8100 | LCD 帧导出（`debug-lcd-dump-*`） |
| 8266 | `xp3200s_init`（machine 初始化：内存、设备、属性） |
| 9030 | `type_init` 注册 machine |

## 3. 推荐阅读顺序

1. **先看寄存器表**：`xp3200s_regs[]`（约 857 行）列出了所有板级寄存器，
   配合 `xp3200s_find_reg()` 和 `xp3200s_io_read/write` 就能建立“地址 → 设备”的映射。
2. **再看 machine 初始化**：`xp3200s_init()`（8266 行）创建内存区域、
   注册 MMIO、挂接设备、声明属性（`nand-image`、`sd-image`、`audio-player` 等）。
3. **按外设读**：
   - LCD：搜 `lcdc` / `graphic_hw_update` / `xp3200s_lcd_refresh`；
   - NAND：搜 `nand_`（复位、命令/地址/数据状态机、ECC、存储回写）；
   - 键盘：从 `xp3200_keyboard_event` 看起，再到 `refresh_matrix`/`refresh_lines`；
   - SD：`xp3200s_sd_command` → `xp3200s_sd_xfer`（状态机与 SPI 收发）；
   - 音频：`xp3200s_audio_data_write` / `xp3200s_audio_sci_write`，
     以及 `xp3200s_audio_dreq_ready` 的 FIFO 节拍。
4. **最后看中断**：`xp3200s_update_irq()`、`xp3200s_itc_pairs[]`、
   `xp3200s_select_irq()` —— 键盘/定时器/端口中断都从这里进 CPU。

## 4. 几个关键概念

- **地址映射**：主 OS 在 NAND `0x7C80000`，运行时在 SDRAM `0x02100000`。
  两者换算：`VA = NAND 偏移 − 0x7C80000 + 0x02100000`。反汇编/断点都用 VA。
- **寄存器表驱动**：大部分 IO 是“查表 + 读写回调”，新增寄存器通常只需
  在 `xp3200s_regs[]` 加一项并处理读写分支。
- **按调用者 PC 路由**：SD 卡和 VS1003 共用 `0x003A0000` SPI 块，
  模型用 `xp3200s_spi_selected()` 根据调用者 PC 区分（SD 驱动在
  `0x02192xxx–0x02194xxx`，音频在 `0x0216Fxxx`）。
- **调试属性**：
  - `debug-lcd-dump-path` + QMP `debug-lcd-dump-seq`：导出当前帧 PGM；
  - `debug-call-ms` / `debug-call-addr`：定时跳转到指定固件函数；
  - `debug-sd-insert-ms`：模拟开机后插入 SD 卡；
  - `-gdb tcp::2349`：GDB-RSP，可断点/单步/读写内存；
  - 按 `F12`：进入固件内置的工厂自检菜单。

## 5. 修改指南

- **加一个外设寄存器**：`xp3200s_regs[]` 加条目 → `xp3200s_io_read_byte` /
  `xp3200s_io_write_byte` 加分支 → 需要时在 `xp3200s_init` 里注册 MMIO 区域。
- **加/改按键**：`xp3200_kbd_map[]`（PC 键 → 行/列位）；第 5 行热键还要看
  `xp3200s_k_input_data_read_impl` 的 row5 分支。
- **SD 协议**：`xp3200s_sd_xfer` 是逐字节状态机，命令解析在
  `xp3200s_sd_command`，CSD 在 `xp3200s_sd_build_csd`。
- **音频**：数据口/命令口由 `xp3200s_audio_stream_selected` 判断，
  DREQ 节拍由 `xp3200s_audio_dreq_ready` + FIFO 容量决定。
- **新增 machine 属性**：参考 `DEFINE_PROP_*` 与 `xp3200s_init` 中的读取，
  属性名即 `-M xp3200,<name>=...`。

## 6. 调试建议

- 固件行为不清楚时，优先用 GDB-RSP 停在目标函数，读寄存器/内存；
- 先用 `debug-lcd-dump-path` 导出帧做自动化比对，再下断点看细节；
- 反汇编文件由 `gnu33` 工具链生成（`target c33-epson-elf`），
  地址按 NAND 偏移标注，换算成 VA 后与 QEMU 断点/日志对应。

更具体的逆向过程见 [移植进度](progress.md)，硬件连接见
[硬件连接](hardware.md)，SD 卡镜像格式见 [SD 卡镜像工具](sdcard-tool.md)。
