# Buildroot 根文件系统构建：100ASK IMX6ULL Pro

## 概述

使用 Buildroot 2020.02.12 为百问网 100ASK IMX6ULL Pro 开发板构建根文件系统，集成项目所需的调试和验证工具，并完成 eMMC 全链路部署。

## 硬件平台

| 项目 | 规格 |
|------|------|
| 开发板 | 100ASK IMX6ULL Pro（韦东山版） |
| SoC | NXP i.MX6ULL (Cortex-A7) |
| 存储 | 4GB eMMC（rootfs 在 mmcblk1p2 分区） |
| 调试串口 | UART1（ttymxc0，115200） |

## 基于的源码

- Buildroot 官网：https://buildroot.org/downloads/buildroot-2020.02.12.tar.gz
- 配置起点：Buildroot 自带的官方配置 `imx6ulevk_defconfig`

## 构建内容

### 1. Buildroot 配置

基于 `imx6ulevk_defconfig` 修改，主要调整：

| 配置类别 | 修改内容 |
|---------|---------|
| 工具链 | 从 Internal 改为 External（Linaro GCC 6.2，和内核编译一致） |
| 系统配置 | BusyBox init + mdev，设置主机名/密码/PS1提示符 |
| 内核/U-Boot | 关闭（自己编译，不用 Buildroot 编译） |
| 文件系统 | ext4 + tar，添加 `-O ^metadata_csum` 兼容旧内核 |
| 软件包 | i2c-tools、spi-tools、strace、trace-cmd、gdbserver、dropbear、evtest、nfs-utils、nano |

### 2. 内核配置补充（配合 rootfs 调试工具）

为支持 rootfs 中的调试工具，在内核 defconfig 中补充开启：

- CONFIG_FTRACE / CONFIG_FUNCTION_TRACER / CONFIG_FUNCTION_GRAPH_TRACER / CONFIG_IRQSOFF_TRACER
- CONFIG_DYNAMIC_DEBUG

更新后的 defconfig 见 `kernel/patches/my_imx_emmc_defconfig`。

### 3. U-Boot 启动参数更新（配合 eMMC 部署）

修改 U-Boot 头文件使系统从 eMMC 自动启动：

| 修改项 | 原始值 | 修改后 | 原因 |
|--------|--------|--------|------|
| CONFIG_SYS_MMC_IMG_LOAD_PART | 1 | 2 | 内核在 eMMC 分区 2 |
| fdt_file | undefined | imx6ull-my14x14-emmc.dtb | 指定内核设备树 |
| loadimage | fatload ... ${image} | ext2load ... /boot/${image} | 分区是 ext4，文件在 /boot/ 下 |
| loadfdt | fatload ... ${fdt_file} | ext2load ... /boot/${fdt_file} | 同上 |

补丁见 `uboot/patches/06-update-bootargs-for-emmc.patch`。

## 文件说明

```
rootfs/
├── configs/
│   └── my_imx6ull_defconfig        # Buildroot defconfig（直接复制到 configs/）
├── overlay/                         # rootfs 覆盖目录（复制到 board/my_imx6ull/rootfs-overlay/）
│   └── etc/profile.d/ps1.sh        # PS1 命令提示符定制
└── docs/
```

## 使用方法

```bash
# 1. 下载 Buildroot
wget https://buildroot.org/downloads/buildroot-2020.02.12.tar.gz
tar -xzf buildroot-2020.02.12.tar.gz && cd buildroot-2020.02.12

# 2. 复制配置文件
cp /path/to/configs/my_imx6ull_defconfig configs/
mkdir -p board/my_imx6ull/rootfs-overlay
cp -r /path/to/overlay/* board/my_imx6ull/rootfs-overlay/

# 3. 编译
make my_imx6ull_defconfig
sudo make

# 4. 部署到 eMMC（在 NFS 启动的板子上操作）
dd if=output/images/rootfs.ext2 of=/dev/mmcblk1p2 bs=1M && sync
mount /dev/mmcblk1p2 /mnt/emmc
mkdir -p /mnt/emmc/boot
cp <zImage> /mnt/emmc/boot/
cp <imx6ull-my14x14-emmc.dtb> /mnt/emmc/boot/
sync && umount /mnt/emmc
```

## 验证结果

eMMC 独立启动成功，全链路自主构建：U-Boot → 内核 → rootfs → shell。所有调试工具（i2c-tools、strace、trace-cmd、gdbserver、dropbear 等）验证可用。
