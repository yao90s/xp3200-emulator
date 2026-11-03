# SD 卡镜像工具

给 XP3200 模拟器（以及真机）生成 / 编辑 FAT16 SD 卡镜像的图形和命令行工具。

## 图形界面（Windows）

```
powershell -ExecutionPolicy Bypass -File PRJ2\tools\SDImageTool.ps1
```

### 新建镜像

1. 选「**新建镜像**」；
2. 填镜像路径、容量（64/128/256/512 MB）、卷标（默认 `SD CARD`）；
3. 用「添加文件...」「添加文件夹...」选择要放进去的内容（文件夹会作为子目录整体复制）；
4. 点「**生成镜像**」。

生成后会自动切到编辑模式并列出镜像内容。

### 编辑已有镜像

1. 选「**编辑已有镜像**」；
2. 点「浏览...」打开一个 `.img`（程序会从 MBR 自动识别分区起始）；
3. 右侧「镜像内的文件」会列出全部文件/目录（支持中文、日文长文件名）；
4. 在左侧「待添加的文件 / 文件夹」里添加内容，点「**写入镜像**」追加进去；
5. 选中右侧某个文件/目录，点「**删除选中**」可删除（目录会递归删除）。

镜像规则与真机 128M 卡一致：MBR 分区表（类型 `0x06`，起始 **LBA 97**）+ FAT16，
文件名用 VFAT LFN 保存 Unicode。

## 命令行（WSL）

```bash
sd_image.sh create <输出.img> <容量MB> <卷标> <文件或文件夹>...
sd_image.sh add    <镜像.img> <文件或文件夹>...
sd_image.sh delete <镜像.img> <路径>...
sd_image.sh list   <镜像.img>        # 输出 JSON
```

例：

```bash
bash PRJ2/tools/sd_image.sh create ~/card.img 128 "SD CARD" ~/音乐 ~/电子书
bash PRJ2/tools/sd_image.sh add ~/card.img ~/另外的歌.mp3
bash PRJ2/tools/sd_image.sh list ~/card.img
bash PRJ2/tools/sd_image.sh delete ~/card.img "MP3/old.mp3"
```

- 文件放到根目录，文件夹作为子目录递归复制；
- 删除目录时路径末尾加 `/`（如 `"MP3/"`），会递归删除；
- 支持 Windows 路径（`C:\...` 自动转换）；
- `list` 由 `fat16_list.py` 解析镜像，输出 JSON（含目录标记、大小、Unicode 路径）。

## 在模拟器里使用

```bash
./qemu-system-s1c33 \
  -M "xp3200,nand-image=.../nand_oob_formatted.img,sd-image=.../card.img" \
  -bios .../NOR_unique_1MB.bin -cpu c33l05,exit-on-halt=off -display gtk
```

进系统：**系统设置 → 磁盘管理 → A盘 → 文件管理**。

> CSD（容量/扇区数）由模拟器按镜像大小自动生成，镜像不需要预置 CSD。

## 依赖

```bash
sudo apt install dosfstools mtools
```

## 相关脚本

| 文件 | 作用 |
|---|---|
| `sd_image.sh` | 命令行入口（create/add/delete/list） |
| `mk_mbr.py` | 写 MBR 分区项 |
| `fat16_lba.py` | 读 MBR，取分区起始 LBA |
| `fat16_list.py` | 解析 FAT16，输出 JSON 文件列表 |
| `SDImageTool.ps1` | Windows 图形界面 |
