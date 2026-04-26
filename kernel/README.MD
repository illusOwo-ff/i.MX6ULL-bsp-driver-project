# Linux Kernel 移植：100ASK IMX6ULL Pro

## 概述

将 NXP 官方 Linux 内核 4.9.88（分支 `imx_4.9.88_2.0.0_ga`）移植到百问网 100ASK IMX6ULL Pro 开发板，解决 eMMC、SD卡和网络三个方面的硬件适配问题。

## 硬件平台

| 项目 | 规格 |
|------|------|
| 开发板 | 100ASK IMX6ULL Pro（韦东山版） |
| SoC | NXP i.MX6ULL (MCIMX6Y2DVM05AA) |
| DDR | 512MB DDR3 |
| 存储 | 4GB eMMC (MTFC4GACAAM-4M IT) + SD卡槽 |
| 网络PHY | LAN8720A × 2（SMSC，RMII接口） |
| 调试串口 | UART1 |

## 基于的源码

- **仓库**：[nxp-imx/linux-imx](https://github.com/nxp-imx/linux-imx)
- **分支**：`imx_4.9.88_2.0.0_ga`
- **参考配置**：`imx_v7_defconfig` + `imx6ull-14x14-evk-emmc.dts`

## 移植内容

### 1. eMMC 适配

**问题**：eMMC 初始化超时（error -110），内核尝试 UHS 模式的 1.8V 信号电压切换失败。

**原因**：100ASK 核心板上 eMMC 的 VDDI 信号电压固定 3.3V，无电压切换电路。


### 2. SD卡 适配

**问题**：SD卡初始化超时（error -110），与 eMMC 同类问题。

**原因**：SD卡 NVCC_SD1 电源域固定 3.3V，无电压切换电路；无 GPIO 控制的电源开关。


### 3. 网络 适配

100ASK 开发板的网络硬件与 NXP EVK 的主要差异：

| 差异项 | NXP EVK | 100ASK |
|-------|---------|--------|
| PHY芯片 | KSZ8081 (Micrel) | LAN8720A (SMSC) |
| ENET1 PHY地址 | 0x2 | 0x0 |
| ENET2 PHY地址 | 0x1 | 0x1（一致） |
| PHY复位方式 | 74HC595 GPIO扩展器 | SoC GPIO直连 |
| ENET1复位引脚 | 通过74HC595 | GPIO5_IO09 (SNVS_TAMPER9) |
| ENET2复位引脚 | 通过74HC595 | GPIO5_IO06 (SNVS_TAMPER6) |

### 4. 不需要修改的部分

| 项目 | 原因 |
|------|------|
| smsc.c（PHY驱动） | 4.9.88 版本已无条件执行软复位，无需补丁 |
| fec_main.c（SION位） | 设备树 pinctrl 的电气属性值已包含 SION 位 |
| pinctrl_enet1/enet2 | RMII 数据引脚是 SoC 专用引脚，所有板子一致 |
| CPU 主频 | SoC 内部频率档位由芯片决定，默认 ondemand 策略即可 |

## 验证结果

-  eMMC：3.64GB 正常识别，rootfs 成功挂载
-  SD卡：64GB SDXC 卡正常识别，CD GPIO 正常
-  网络：SMSC LAN8720A PHY 驱动加载，100Mbps/Full 链路建立，ping 通过
-  串口：console 正常输出和交互

## 补丁与文件说明

`patches/` 目录下的文件：

| 文件 | 说明 |
|------|------|
| `imx6ull-my14x14-emmc.dts` | 自定义板级设备树（新建文件，包含所有硬件适配修改） |
| `my_imx_emmc_defconfig` | 自定义板级 defconfig（基于 imx_v7_defconfig，添加 SMSC PHY 驱动） |
| `01-dts-makefile-add-myboard.patch` | 设备树 Makefile 补丁（注册新设备树到编译系统） |

## 使用方法

```bash
# 1. 下载 NXP 原始内核源码
git clone -b imx_4.9.88_2.0.0_ga --single-branch https://github.com/nxp-imx/linux-imx.git
cd linux-imx

# 2. 复制板级文件
cp /path/to/patches/imx6ull-my14x14-emmc.dts arch/arm/boot/dts/
cp /path/to/patches/my_imx_emmc_defconfig arch/arm/configs/

# 3. 应用补丁
git apply /path/to/patches/01-dts-makefile-add-myboard.patch

# 4. 编译
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- my_imx_emmc_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)

# 5. 产出物
# 内核镜像：arch/arm/boot/zImage
# 设备树：arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb

# 6. tftp 加载启动（U-Boot 命令行）
setenv bootargs 'console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw'
tftp 80800000 zImage
tftp 83000000 imx6ull-my14x14-emmc.dtb
bootz 80800000 - 83000000
```

## 详细文档

- [内核移植完整过程记录](docs/kernel_porting_100ask_imx6ull.md)：包含问题定位、原理图分析、设备树修改详解、问题记录
