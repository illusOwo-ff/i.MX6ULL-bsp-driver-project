# U-Boot-2017.03 移植：100ASK IMX6ULL Pro

## 概述

将 NXP 官方 U-Boot 2017.03（分支 `imx_v2017.03_4.9.88_2.0.0_ga`）移植到百问网 100ASK IMX6ULL Pro 开发板，并实现完整的网络功能支持。

## 硬件平台

| 项目 | 规格 |
|------|------|
| 开发板 | 100ASK IMX6ULL Pro（开发板厂商：韦东山） |
| SoC | NXP i.MX6ULL (MCIMX6Y2DVM05AA) |
| DDR | 512MB DDR3 |
| 存储 | 4GB eMMC + SD卡 |
| 网络PHY | LAN8720A × 2（RMII接口） |
| 调试串口 | UART1（USB转串口 CP2104） |

## 基于的源码

- **仓库**：[nxp-imx/uboot-imx](https://github.com/nxp-imx/uboot-imx)
- **分支**：`imx_v2017.03_4.9.88_2.0.0_ga`

## 移植内容

### 1. 板级文件创建

基于 NXP 官方 `mx6ullevk` 配置，创建了 `mx6ull_myboard` 板级支持：
- defconfig、头文件、板级C文件、设备树

### 2. 网络驱动适配

100ASK 开发板的网络硬件与 NXP EVK 的主要差异：

| 差异项 | NXP EVK | 100ASK |
|-------|---------|--------|
| PHY芯片 | KSZ8081 (Micrel) | LAN8720A (SMSC) |
| ENET1 PHY地址 | 0x2 | 0x0 |
| PHY复位方式 | 74HC595 GPIO扩展器 | SoC GPIO直连 |
| ENET1复位引脚 | 通过74HC595 | GPIO5_IO09 (SNVS_TAMPER9) |
| ENET2复位引脚 | 通过74HC595 | GPIO5_IO06 (SNVS_TAMPER6) |

对应的代码修改：
- 头文件：PHY驱动宏改为 `CONFIG_PHY_SMSC`，修正PHY地址，添加默认MAC/IP环境变量
- 板级C文件：添加PHY硬件复位GPIO操作，删除KSZ8081特定寄存器配置
- PHY驱动：添加LAN8720A首次链路协商的软复位补丁
- 设备树：修正ENET1的PHY地址节点
- Kconfig：使能 PHYLIB、PHY_SMSC、FEC_MXC（2017版U-Boot的Kconfig过渡期问题）

### 3. 验证结果

- DDR 512MB 识别正确
- SD卡和eMMC正常读写
- 网络ping通（PHY自动协商成功，SMSC LAN8720A正确识别）
- 从eMMC成功加载并启动Linux内核

## 补丁文件说明

`patches/` 目录下的补丁文件基于原始源码生成：

| 文件 | 说明 |
|------|------|
| `01-add-myboard-header.patch` | 头文件修改（PHY配置、环境变量） |
| `02-add-myboard-board-file.patch` | 板级C文件修改（PHY复位、初始化） |
| `03-lan8720a-phy-fix.patch` | LAN8720A软复位补丁 |
| `04-add-myboard-dts.patch` | 设备树修改（PHY地址） |
| `05-dts-makefile-add-myboard.patch` | 设备树编译配置 |
| `mx6ull_myboard_defconfig` | 板级defconfig（直接复制到configs/） |

## 使用方法

```bash
# 1. 下载NXP原始源码
git clone -b imx_v2017.03_4.9.88_2.0.0_ga --single-branch https://github.com/nxp-imx/uboot-imx.git
cd uboot-imx

# 2. 应用补丁
git apply /path/to/patches/01-add-myboard-header.patch
git apply /path/to/patches/02-add-myboard-board-file.patch
git apply /path/to/patches/03-lan8720a-phy-fix.patch
git apply /path/to/patches/04-add-myboard-dts.patch
git apply /path/to/patches/05-dts-makefile-add-myboard.patch
cp /path/to/patches/mx6ull_myboard_defconfig configs/

# 3. 编译
make mx6ull_myboard_defconfig
make -j$(nproc)

# 4. 烧写到SD卡
sudo dd if=u-boot-dtb.imx of=/dev/sdX bs=1024 seek=1 conv=fsync
```

## 详细文档

- [移植完整过程记录](docs/)：包括踩坑记录、问题排查思路、原理图分析
