# 将NXP 官方 Linux内核4.9.88移植到韦东山IMX6ULL Pro开发板上

> 本文记录了将NXP官方Linux内核4.9.88移植到百问网（100ASK）IMX6ULL Pro开发板的完整过程，包括defconfig和设备树选择的分析过程、问题定位与原理图对比、设备树修改、驱动使能，以及最终的验证。本文是BSP移植项目中内核移植部分的详细记录，U-Boot移植已在uboot部分完成。

---

[TOC]

---

## 一、移植背景

### 1.1 硬件信息

- **开发板**：百问网 100ASK IMX6ULL Pro（韦东山）
- **核心板**：MYC-Y6ULX（米尔科技），搭载 i.MX6ULL SoC
- **DDR**：512MB DDR3
- **存储**：4GB eMMC（MTFC4GACAAM-4M IT）+ SD卡槽
- **网络**：双路以太网，均使用 LAN8720A PHY芯片（SMSC公司，RMII接口）
  - ENET1 的 PHY（U6）在核心板上，PHYAD=0，复位引脚 GPIO5_IO09（SNVS_TAMPER9）
  - ENET2 的 PHY（U11）在底板上，PHYAD=1，复位引脚 GPIO5_IO06（SNVS_TAMPER6）

### 1.2 软件版本

- **内核版本**：Linux 4.9.88
- **NXP BSP分支**：`imx_4.9.88_2.0.0_ga`
- **NXP仓库地址**：`https://github.com/nxp-imx/linux-imx`
- **交叉编译工具链**：arm-linux-gnueabihf-（Linaro GCC 6.2-2016.11）
- **U-Boot版本**：2017.03（已在另一篇文章中完成移植）

### 1.3 前置条件

- U-Boot移植已完成，能在SD卡上正常启动，网络功能已验证（tftp可用）
- eMMC中有韦东山出厂的根文件系统（后续验证时挂载这个rootfs）
- 虚拟机已安装tftpd-hpa，tftp功能可用

### 1.4 完整移植流程

```
获取芯片原厂内核源码（与U-Boot版本对应的分支）
        ↓
确定defconfig和设备树（选择和自己SoC/板子最接近的参考配置）
        ↓
用参考板的配置编译内核，加载到自己板子上启动
        ↓
读启动日志，定位哪些功能正常、哪些有问题
        ↓
创建自己的板级文件（复制defconfig和设备树，重命名）
        ↓
对比原理图 + 分析设备树，确定每个问题的根因
        ↓
修改设备树 / defconfig / 驱动代码
        ↓
编译、加载、测试、验证
```



## 二、获取源码与确定配置

### 2.1 下载内核源码

从NXP官方GitHub仓库下载和U-Boot版本配套的内核分支：

```bash
git clone -b imx_4.9.88_2.0.0_ga --single-branch https://github.com/nxp-imx/linux-imx.git
```

复制一份到工作目录：

```bash
cp -r xxximx ~/my-linux-imx-imx_4.9.88.2.0
cd ~/my-linux-imx-imx_4.9.88.2.0
```

### 2.2 确定defconfig

查找和IMX相关的defconfig：

```bash
ls arch/arm/configs/ | grep imx
```

输出包含：`imx_v4_v5_defconfig`、`imx_v6_v7_defconfig`、`imx_v7_defconfig`

**选择 `imx_v7_defconfig`**。理由：IMX6ULL是ARMv7架构的i.MX系列SoC，这个defconfig就是NXP为i.MX6/i.MX7系列ARMv7芯片提供的EVK参考配置。

**验证**：用diff对比韦东山已移植完成的 `100ask_imx6ull_defconfig` 和NXP原厂的几个defconfig，确认差异主要是格式差异（韦东山的是完整展开的.config格式，NXP的是精简的defconfig格式），内容上 `imx_v7_defconfig` 最接近。

**注意**：正点原子使用的是 `imx_v7_mfg_defconfig`，但那是因为他用的内核版本是4.1.15，和我们的4.9.88不同，defconfig命名也不同。这就是为什么不能照搬其他人的操作——必须确认自己的版本和文件。

### 2.3 确定设备树

查看编译出来的IMX6ULL相关设备树：

```bash
ls arch/arm/boot/dts/ | grep imx6ull
```

按以下逻辑选择：

1. **按封装排除**：排除 `imx6ull-9x9-evk*`（9x9封装），我的板子是14x14封装
2. **按板型选择**：选 `evk` 系列（EVK是NXP公开发售的评估板，第三方板厂参考EVK设计），排除 `ddr3-arm2` 系列
3. **按存储类型选择**：`imx6ull-14x14-evk-emmc.dts` 最接近（我的板子是eMMC方案）

**确认evk和evk-emmc的差异**：

```bash
diff arch/arm/boot/dts/imx6ull-14x14-evk.dts arch/arm/boot/dts/imx6ull-14x14-evk-emmc.dts
```

结果显示evk-emmc.dts的内容只是include了evk.dts，然后覆盖了usdhc2节点改成8线eMMC模式。本质上是同一份配置，emmc版只多了eMMC的8线配置。

**最终选择**：基于 `imx6ull-14x14-evk-emmc.dts` 创建自己的设备树。

---

## 三、首次编译与启动测试

### 3.1 编译

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- imx_v7_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)
```

编译成功后产出：
- 内核镜像：`arch/arm/boot/zImage`
- 设备树：`arch/arm/boot/dts/imx6ull-14x14-evk-emmc.dtb`

### 3.2 tftp加载启动

将编译产出物复制到tftp目录：

```bash
sudo cp arch/arm/boot/zImage /var/lib/tftpboot/
sudo cp arch/arm/boot/dts/imx6ull-14x14-evk-emmc.dtb /var/lib/tftpboot/
```

开发板从SD卡启动（已移植好的U-Boot），在U-Boot命令行：

```bash
setenv bootargs 'console=ttymxc0,115200'
tftp 80800000 zImage
tftp 83000000 imx6ull-14x14-evk-emmc.dtb
bootz 80800000 - 83000000
```

tftp能成功下载文件，也验证了U-Boot的网络移植是成功的。

不指定 `root=`，内核启动后会在最后因找不到根文件系统而kernel panic，这是预期行为。此时关注panic之前的启动日志。

### 3.3 启动日志分析——定位问题

**正常的部分**（SoC内部控制器正常，不需要改）：

```
Linux version 4.9.88 (book@100ask)
Memory: 175852K/524288K available          # DDR 512MB正确
console [ttymxc0] enabled                   # 串口正常
i2c i2c-0: IMX I2C adapter registered      # I2C控制器正常
imx-sdma 20ec000.sdma: loaded firmware 3.3  # SDMA引擎正常
ci_hdrc ci_hdrc.1: USB 2.0 started         # USB Host正常
```

**问题一：eMMC初始化失败**

```
mmc1: Tuning failed, falling back to fixed sampling clock
mmc1: error -110 whilst initialising MMC card
```

error -110是超时错误（ETIMEDOUT）。反复出现，说明eMMC完全不能用。

**问题二：网络PHY没有正常连接**

```
libphy: fec_enet_mii_bus: probed
fec 20b4000.ethernet eth0: registered PHC device 0
fec 2188000.ethernet eth1: registered PHC device 1
```

FEC MAC控制器注册了，但没有任何PHY link up的消息。正常应该能看到类似 `attached PHY driver [SMSC LAN8710/LAN8720]` 的打印。

**问题三：SD卡初始化失败**

```
mmc0: error -110 whilst initialising SD card
```

mmc0对应usdhc1（TF卡槽），同样是超时错误。

**问题四（不需要处理）：EVK特有外设probe失败**

```
fxls8471: probe of 0-001e failed with error -22
mag3110 0-000e: read chip ID 0xfffffffa is not equal to 0xc4!
fsl-quadspi 21e0000.qspi: unrecognized JEDEC id bytes: ff, ff, ff
```

这些是EVK板上才有的设备（加速度计、磁力计、QSPI NOR Flash），100ASK板上没有这些硬件，probe失败正常。后续创建自己设备树时做驱动开发时会逐步清理。

---

## 四、创建自己的板级文件

### 4.1 复制文件

```bash
# 复制defconfig
cp arch/arm/configs/imx_v7_defconfig arch/arm/configs/my_imx_emmc_defconfig

# 复制设备树（基于evk-emmc版本）
cp arch/arm/boot/dts/imx6ull-14x14-evk-emmc.dts arch/arm/boot/dts/imx6ull-my14x14-emmc.dts
```

### 4.2 注册设备树到编译系统

编辑 `arch/arm/boot/dts/Makefile`，在 `CONFIG_SOC_IMX6ULL` 段里加上自己的设备树编译：

```makefile
imx6ull-my14x14-emmc.dtb \
```

![image-20260426212729065](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260426212729065.png)

### 4.3 defconfig调整

**为什么要禁掉V6？** 因为我的IMX6ULL是纯ARMv7芯片。如果内核同时编译V6和V7支持，某些内核模块加载时可能因为指令集兼容性问题失败。对于后面要做的控制器驱动开发（编译成.ko模块加载），会产生问题

检查CONFIG_ARCH_MULTI_V6配置（IMX6ULL是纯ARMv7芯片，不应编译V6支持）：

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- my_imx_emmc_defconfig
grep CONFIG_ARCH_MULTI_V6 .config
```

结果显示 `# CONFIG_ARCH_MULTI_V6 is not set`，4.9.88版本的defconfig默认已处理好，不需要额外修改。

![image-20260426150926571](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260426150926571.png)

---

## 五、问题分析与原理图对比

### 5.1 问题一：eMMC（Tuning failed）

**分析过程**：

mmc1对应usdhc2控制器。查看EVK设备树里usdhc2的配置：

```bash
sed -n '/&usdhc2/,/^};/p' arch/arm/boot/dts/imx6ull-14x14-evk-emmc.dts
```

关键发现：节点里**没有 `no-1-8-v` 属性**。这意味着内核会尝试UHS模式的1.8V信号电压切换。

**对比原理图**：核心板原理图第5页，eMMC芯片（MTFC4GACAAM-4M IT）的VCC引脚连到VDD_3V3，VDDI引脚也是3.3V供电（通过去耦电容C62、C63滤波），没有电压切换电路。

去耦电容知识点：芯片电源引脚旁边一端接电源一端接地的小电容是去耦电容，用于滤除高频噪声，不是把电源短接到地。判断VDDI电压看的是这根电源网络连接的源头。

![image-20260420024359502](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260420024359502.png)

**结论**：eMMC信号电压固定3.3V，不支持1.8V切换。必须加 `no-1-8-v;` 属性。

![](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260426155801568.png)

### 5.2 问题二：网络PHY不工作，查看原理图部分见移植uboot文章

**分析过程（按四个子问题逐一排查）**：

**子问题1：PHY地址对不对？**

在fec2节点的mdio子节点里检查：

```
ethphy0: ethernet-phy@2 { reg = <2>; };
ethphy1: ethernet-phy@1 { reg = <1>; };
```

对比原理图：核心板第10页ENET1的LAN8720A，PHYAD0引脚（pin 10）通过R222 10K下拉到GND → PHY地址=0。而EVK设备树里写的是2。**地址不匹配，需要改。**

ENET2的PHY地址是1，与EVK一致，不需要改。

**子问题2：PHY复位引脚配了没有？**

fec1和fec2节点里都没有 `phy-reset-gpios` 属性。EVK板上PHY复位是通过SPI接口的GPIO扩展芯片74HC595间接控制的，所以不需要写这个属性。但100ASK板的PHY复位是直接接到SoC的GPIO。

对比原理图确认：
- ENET1 PHY复位：核心板第10页，RST引脚通过R214连到SNVS_TAMPER9 = GPIO5_IO09
- ENET2 PHY复位：底板第6页，RST引脚通过ENET2_nRST网络标号连到SNVS_TAMPER6 = GPIO5_IO06

**需要在fec节点加 `phy-reset-gpios` 和 `phy-reset-duration` 属性。** 内核FEC驱动会自动读取这些属性并执行GPIO复位操作，不需要像U-Boot那样手写C代码。

**子问题3：SMSC PHY驱动有没有编译进内核？**

```bash
grep CONFIG_SMSC_PHY .config
```

结果是 `not set`。`imx_v7_defconfig` 默认没有使能SMSC PHY驱动（EVK用的是KSZ8081，Micrel的PHY）。**需要在defconfig里加 `CONFIG_SMSC_PHY=y`。**

同时需要检查smsc.c里的软复位实现——在U-Boot移植时曾在phy.c里给LAN8720A加过软复位补丁。内核4.9.88版本的smsc.c中检查发现 `smsc_phy_reset` 函数已经在if条件**外部**调用 `genphy_soft_reset(phydev)`，意味着无条件执行软复位，**不需要修改smsc.c**。

**子问题4：引脚配置有没有问题？**

```bash
grep -A 12 'pinctrl_enet1:' arch/arm/boot/dts/imx6ull-14x14-evk.dts
grep -A 15 'pinctrl_enet2:' arch/arm/boot/dts/imx6ull-14x14-evk.dts
```

RMII数据引脚配置和100ASK板一致（都是标准RMII专用引脚），TX_CLK的电气属性值 `0x4001b031` 已包含SION位（0x40000000）。**pinctrl不需要改。**

**引脚冲突排查**：

```bash
grep -n 'TAMPER9' arch/arm/boot/dts/imx6ull-14x14-evk.dts
grep -n 'TAMPER6' arch/arm/boot/dts/imx6ull-14x14-evk.dts
```

结果：TAMPER9在第583行被 `pinctrl_lcdif_reset` 占用（LCD复位功能）。TAMPER6没有被占用。

进一步追踪TAMPER9的引用链：

```bash
grep 'pinctrl_lcdif_reset' arch/arm/boot/dts/imx6ull-14x14-evk.dts
```

发现被 `&lcdif` 节点的 `pinctrl-0` 引用。

对比原理图确认：100ASK板的LCD_RESET信号使用的是SoC专用的LCD_RESET引脚，不是TAMPER9。所以解除TAMPER9的LCD占用不影响LCD功能。**需要覆盖 `pinctrl_lcdif_reset` 清空其引脚配置。**

最终修改汇总：

#### 5.2.1 设备树修改（imx6ull-my14x14-emmc.dts）

**改动1：覆盖fec1节点——加PHY复位GPIO和复位pinctrl引用**

**改动2：覆盖fec2节点——加PHY复位GPIO，改mdio里的PHY地址，加SMSC属性**

**改动3：解除TAMPER9的LCD占用——覆盖lcdif_reset的pinctrl，去掉TAMPER9**（你的板子LCD_RESET用的是专用引脚，不是TAMPER9）

**改动4：在iomuxc_snvs里添加两个PHY复位引脚的pinctrl group**

#### 5.2.2 defconfig修改

**改动5：加 `CONFIG_SMSC_PHY=y`**

#### 5.2.3 不需要改的

- smsc.c —— 4.9.88已经修复了软复位问题
- fec_main.c —— SION位在设备树pinctrl里已经设了，先不改，测试不通再加
- pinctrl_enet1/enet2 —— RMII引脚和EVK一致，电气属性先用默认值

### 5.3 问题三：SD卡（error -110）

**分析过程**：

mmc0对应usdhc1。查看EVK设备树里usdhc1的配置：

```bash
sed -n '/&usdhc1/,/^};/p' arch/arm/boot/dts/imx6ull-14x14-evk.dts
```

**对比原理图确认CD引脚**：

底板原理图第4页的TF卡座，CD引脚连到信号 `SD1_CD`。追踪到底板连接器（第2页）pin 53，对应核心板侧标注 `UART1_RTS`。

这里涉及引脚复用知识：pad名 `UART1_RTS_B` 在ALT5模式下功能是 `GPIO1_IO19`。这和EVK设备树里的 `cd-gpios = <&gpio1 19 GPIO_ACTIVE_LOW>` 完全一致。**CD引脚不需要改。**

**对比原理图确认电源控制**：底板第4页，SD卡VDD直接通过上拉电阻接VDD_3V3，没有GPIO控制的电源开关。**需要去掉 `vmmc-supply` 属性。**

底板转接板

![image-20260420024210985](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260420024210985.png)

底板SD卡槽

![image-20260420024037294](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260420024037294.png)

**电压切换**：SD卡的NVCC_SD1电源域直接接VDD_3V3，没有电压切换电路。和eMMC同理，**需要加 `no-1-8-v;` 属性。**

![image-20260426162945393](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260426162945393.png)

---

## 六、具体修改

### 6.1 设备树修改

#### 最终完整的设备树文件 (`imx6ull-my14x14-emmc.dts`)：

```dts
/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "imx6ull-14x14-evk.dts"

/* 解除TAMPER9的LCD占用——100ASK板用TAMPER9做ENET1 PHY复位 */
&pinctrl_lcdif_reset {
	fsl,pins = <
		/* cleared: TAMPER9 used for ENET1 PHY reset on this board */
	>;
};

/* 添加PHY复位引脚的pinctrl定义 */
&iomuxc_snvs {
	imx6ull-myboard {
		pinctrl_enet1_reset: enet1resetgrp {
			fsl,pins = <
				MX6ULL_PAD_SNVS_TAMPER9__GPIO5_IO09	0x000110A0
			>;
		};

		pinctrl_enet2_reset: enet2resetgrp {
			fsl,pins = <
				MX6ULL_PAD_SNVS_TAMPER6__GPIO5_IO06	0x000110A0
			>;
		};
	};
};

/* ENET1：加复位引脚 */
&fec1 {
	pinctrl-names = "default";
	pinctrl-0 = <&pinctrl_enet1
		     &pinctrl_enet1_reset>;
	phy-mode = "rmii";
	phy-handle = <&ethphy0>;
	phy-reset-gpios = <&gpio5 9 GPIO_ACTIVE_LOW>;
	phy-reset-duration = <200>;
	status = "okay";
};

/* ENET2：加复位引脚 */
&fec2 {
	pinctrl-names = "default";
	pinctrl-0 = <&pinctrl_enet2
		     &pinctrl_enet2_reset>;
	phy-mode = "rmii";
	phy-handle = <&ethphy1>;
	phy-reset-gpios = <&gpio5 6 GPIO_ACTIVE_LOW>;
	phy-reset-duration = <200>;
	status = "okay";
};

/* 覆盖PHY节点：改ENET1地址为0，加SMSC属性 */
&ethphy0 {
	smsc,disable-energy-detect;
	reg = <0>;
};

&ethphy1 {
	smsc,disable-energy-detect;
};

/* SD卡：去掉vmmc-supply（无电源控制GPIO），加no-1-8-v */
&usdhc1 {
	pinctrl-names = "default", "state_100mhz", "state_200mhz";
	pinctrl-0 = <&pinctrl_usdhc1>;
	pinctrl-1 = <&pinctrl_usdhc1_100mhz>;
	pinctrl-2 = <&pinctrl_usdhc1_200mhz>;
	cd-gpios = <&gpio1 19 GPIO_ACTIVE_LOW>;
	keep-power-in-suspend;
	enable-sdio-wakeup;
	no-1-8-v;
	status = "okay";
};

/* eMMC：加no-1-8-v（信号电压固定3.3V，不支持UHS切换） */
&usdhc2 {
	pinctrl-names = "default", "state_100mhz", "state_200mhz";
	pinctrl-0 = <&pinctrl_usdhc2_8bit>;
	pinctrl-1 = <&pinctrl_usdhc2_8bit_100mhz>;
	pinctrl-2 = <&pinctrl_usdhc2_8bit_200mhz>;
	bus-width = <8>;
	no-1-8-v;
	non-removable;
	status = "okay";
};
```

**PHY节点的覆盖方式**：

最初尝试在 `&fec2` 里重写mdio子节点并定义 `ethphy0: ethernet-phy@0`，编译时报错 `Duplicate label 'ethphy0'`。原因是EVK设备树里已有 `ethphy0: ethernet-phy@2`，新建的 `ethernet-phy@0` 和 `ethernet-phy@2` 是不同的节点名，但label重名。

正确做法：**用 `&ethphy0` 通过label直接覆盖已有节点**，修改其 `reg` 属性。虽然节点名仍然是 `ethernet-phy@2`，但内核使用的是 `reg` 属性的值来确定PHY地址。

### 6.2 defconfig修改

在 `my_imx_emmc_defconfig` 中添加SMSC PHY驱动使能。找到PHY相关配置的位置：

```bash
grep -n 'PHY\|MICREL' arch/arm/configs/my_imx_emmc_defconfig
```

在第147行 `CONFIG_MICREL_PHY=y` 下面添加：

```
CONFIG_SMSC_PHY=y
```

两个PHY驱动都保留不冲突。

![image-20260426211449984](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260426211449984.png)

### 6.3 不需要修改的部分

| 项目 | 原因 |
|------|------|
| smsc.c（PHY驱动） | 4.9.88版本已无条件执行软复位（genphy_soft_reset在if外部），不需要像U-Boot移植时那样加补丁 |
| fec_main.c（SION位） | 设备树pinctrl里TX_CLK的电气属性值0x4001b031已包含SION位（0x40000000），不需要在代码里重复设置 |
| pinctrl_enet1/enet2（RMII引脚） | RMII数据引脚是SoC专用引脚，所有用RMII接口的板子配置一样 |
| pinctrl电气属性值 | 先用EVK默认值，如果网络工作异常再考虑调整 |
| CPU主频 | SoC内部的频率档位由芯片决定，和板子设计无关，默认ondemand策略够用 |

---

## 七、编译与测试

### 7.1 编译前检查

```bash
# 编译defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- my_imx_emmc_defconfig

# 检查SMSC PHY驱动是否使能
grep CONFIG_SMSC_PHY .config
# 输出：CONFIG_SMSC_PHY=y ✓

# 单独编译设备树（快速验证语法）
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- dtbs

# 确认设备树编译成功
ls arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb
```

### 7.2 编译内核

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)
```

编译后检查驱动是否编译出来：

```bash
ls drivers/net/phy/smsc.o    # 确认SMSC驱动编译成功
```

### 7.3 加载启动

复制到tftp目录：

```bash
sudo cp arch/arm/boot/zImage /var/lib/tftpboot/
sudo cp arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb /var/lib/tftpboot/
```

在U-Boot命令行（这次指定挂载eMMC上的根文件系统）：

```bash
setenv bootargs 'console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw'
tftp 80800000 zImage
tftp 83000000 imx6ull-my14x14-emmc.dtb
bootz 80800000 - 83000000
```

---

## 八、验证结果

### 8.1 eMMC 

移植前：
```
mmc1: Tuning failed, falling back to fixed sampling clock
mmc1: error -110 whilst initialising MMC card
```

移植后：
```
mmc1: new DDR MMC card at address 0001
mmcblk1: mmc1:0001 S40004 3.64 GiB
mmcblk1: p1 p2 p3
EXT4-fs (mmcblk1p2): mounted filesystem with ordered data mode.
VFS: Mounted root (ext4 filesystem) on device 179:10.
```

eMMC成功识别3.64GB，3个分区正常，rootfs成功挂载。`no-1-8-v` 属性生效。

### 8.2 SD卡 

移植前：
```
mmc0: error -110 whilst initialising SD card
```

移植后：
```
sdhci-esdhc-imx 2190000.usdhc: Got CD GPIO
mmc0: new high speed SDXC card at address aaaa
mmcblk0: mmc0:aaaa SD64G 59.5 GiB
mmcblk0: p1
```

`Got CD GPIO` 确认CD引脚配置正确。SD卡被识别为64GB SDXC卡。

### 8.3 网络 

移植前：
```
fec 20b4000.ethernet eth0: registered PHC device 0
（没有PHY attach和link up的信息）
```

移植后：
```
SMSC LAN8710/LAN8720 20b4000.ethernet-1:01: attached PHY driver [SMSC LAN8710/LAN8720]
fec 20b4000.ethernet eth2: Link is Up - 100Mbps/Full - flow control rx/tx
```

SMSC PHY驱动成功加载，LAN8720A被识别，100Mbps全双工链路建立。

进入Linux后手动验证：

```bash
ifconfig eth2 192.168.5.9 netmask 255.255.255.0 up
ping 192.168.5.11    # ping虚拟机，成功
```

网络完全正常。

### 8.4 一个非功能性问题：网卡命名

启动日志中出现：

```
fec 20b4000.ethernet eth124: renamed from eth0
fec 20b4000.ethernet eth2: renamed from eth124
Starting network: Cannot find device "eth0"
Failed to bring up eth0.
```

这是因为韦东山出厂的根文件系统里的udev规则和新内核的网卡命名不匹配。网卡硬件本身正常工作，只是名字从 `eth0` 变成了 `eth2`。这不是内核移植的问题，是根文件系统配置的问题，后续构建自己的rootfs时会解决。
