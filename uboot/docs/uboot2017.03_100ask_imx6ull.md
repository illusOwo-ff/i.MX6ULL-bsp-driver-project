# 在韦东山100ASK IMX6ULL Pro开发板上移植U-Boot 2017.03并支持网络功能

> 本文记录了将 NXP 官方 U-Boot 2017.03 移植到百问网（100ASK）IMX6ULL Pro 开发板的完整过程，包括源码获取、板级文件创建、网络驱动适配、问题排查与解决，以及最终的内核启动验证。

---

[TOC]



## 一、移植背景

### 1.1 移植的本质

U-Boot 移植的本质是：**芯片原厂已经把 U-Boot 适配好了自己的参考开发板（EVK），我要做的是在这个基础上，根据自己开发板与参考板的硬件差异，修改 U-Boot 中对应的代码，使 U-Boot 能在我的板子上正常工作。**

我不需要从零写 U-Boot，也不需要从社区主线 U-Boot 开始移植。芯片原厂的 BSP 包已经做好了最难的部分（SoC 级别的初始化、DDR 配置、时钟树等），我只需要处理板级差异。

核心思维模式是三步：

1. **读原厂参考板的代码**，搞清楚"原来是怎么做的"
2. **对比原理图**，找出"自己的板子和参考板有什么硬件差异"
3. **根据差异改代码**，只改有差异的部分

韦东山100ASK IMX6ULL Pro开发板出厂自带的U-Boot是已经移植完成的版本。但作为驱动工程师的学习项目，我需要从NXP官方提供的原始BSP出发，自己完成移植过程——这和实际工作中"拿到一块新设计的板子，从芯片原厂BSP开始适配"的场景完全一致。

### 1.2 硬件信息

- **开发板**：百问网 100ASK IMX6ULL Pro（韦东山版）
- **核心板**：MYC-Y6ULX（米尔科技），搭载 i.MX6ULL SoC
- **DDR**：512MB DDR3（MT41K128M16JT-125）
- **存储**：4GB eMMC + SD卡槽
- **网络**：双路以太网，均使用 LAN8720A PHY芯片（RMII接口）
  - ENET1 的 PHY（U6）在核心板上
  - ENET2 的 PHY（U11）在底板上
- **调试串口**：UART1

### 1.3 软件版本

- **U-Boot版本**：2017.03
- **NXP BSP分支**：`imx_v2017.03_4.9.88_2.0.0_ga`
- **交叉编译工具链**：arm-linux-gnueabihf-（Buildroot 2020.02 提供的 gcc 7.5.0）

### 1.4 完整移植流程

```
获取芯片原厂 U-Boot 源码
        ↓
用原厂参考板的 defconfig 编译，烧到自己板子上启动
        ↓
在 U-Boot 命令行检查各项基础功能（DDR、SD/eMMC、串口）
        ↓
在 U-Boot 中添加自己的板子（复制并重命名相关文件）
        ↓
对比原理图，找出硬件差异
        ↓
针对差异修改代码（网络、LCD、其他外设）
        ↓
编译、烧写、测试、调试
        ↓
验证通过（U-Boot 能正常启动 Linux 内核），移植完成
```

---

## 二、获取NXP官方U-Boot源码

### 2.1 确定版本

先正常从EMMC启动一次开发板，通过查看开发板原有U-Boot的启动打印信息确定版本：

```
U-Boot 2017.03 (Jun 03 2020 - 13:12:42 +0800)
```

同时内核启动信息显示内核版本为 4.9.88。

### 2.2 找到对应的NXP仓库分支

芯片原厂通常在 GitHub 或官网提供基于主线 U-Boot 修改的 BSP 版本。NXP的U-Boot仓库地址：`https://github.com/nxp-imx/uboot-imx`

仓库中 `nxp-imx` 是NXP公司的GitHub官方账号，所有分支都是NXP在原始U-Boot基础上修改过的版本。有些分支名带 `nxp/` 前缀、有些不带，这只是命名风格不统一，内容都是NXP修改后的uboot。

NXP 的分支命名规则是 `imx_v{uboot版本}_{内核版本}_{BSP发布号}`，在仓库的分支搜索框中搜索 `4.9.88`，找到NXP官方发布的适配4.9.88内核的2017.03版Uboot `imx_v2017.03_4.9.88_2.0.0_ga`（`ga` = General Availability，正式发布版）。

### 2.3 下载源码

```bash
git clone -b imx_v2017.03_4.9.88_2.0.0_ga --single-branch https://github.com/nxp-imx/uboot-imx.git
# 或者在 GitHub 上切换到对应分支后，点 Code → Download ZIP，下载ZIP压缩包
```

同时也下载了对应的内核源码（后续内核移植使用）：

```bash
git clone -b imx_4.9.88_2.0.0_ga --single-branch https://github.com/nxp-imx/linux-imx.git
```

---

## 三、首次编译与启动测试

将下载好的源码拷贝到虚拟机

### 3.1 确定defconfig

在源码的 `configs/` 目录下查找与 SoC 型号匹配的 defconfig：

```bash
ls configs/ | grep mx6ull
```

输出中包含 `mx6ull_14x14_evk_defconfig`，这是NXP官方 i.MX6ULL EVK 评估板的配置。韦东山的开发板基于相同的SoC，理论上硬件配置最接近这个EVK，所以选用它。（也可以查看韦东山提供的手册给的编译uboot时的命令，发现他编译时用的是`mx6ull_14x14_evk_defconfig`这个config，证明他就是基于这个名字的原厂配置改的。）

### 3.2 编译

```bash
make distclean
make mx6ull_14x14_evk_defconfig
make -j$(nproc)
```

编译成功后生成 `u-boot-dtb.imx`。

### 3.3 烧写到SD卡

**始终用 SD 卡启动来做移植开发**，eMMC 里的原始系统不会被破坏，拔掉 SD 卡就能恢复。

将TF卡插入USB读卡器插入电脑连接到虚拟机，在虚拟机中把编译好的uboot镜像烧录到TF卡中：

```bash
# 确认SD卡设备名
lsblk
# 输出显示 sdb 是SD卡，sdb1 是其分区

# # 如果 SD 卡被自动挂载了文件系统，先卸载SD卡文件系统
sudo umount /media/book/3361-3634

# 烧写U-Boot到SD卡（i.MX6ULL的BootROM规定从1KB偏移处读取，单位 1024 字节）
sudo dd if=u-boot-dtb.imx of=/dev/sdb bs=1024 seek=1 conv=fsync
```

注意：`of=` 后面是整个设备（如 `/dev/sdb`），不是分区（不是 `/dev/sdb1`）。SD 卡上原有的文件系统数据（如 Windows 生成的 `System Volume Information`）不影响，因为 `dd` 是直接写物理偏移，不经过文件系统。

### 3.4 首次启动检查

将烧录好的TF卡插入开发板TF卡槽，切换拨码开关到 SD 卡启动，上电后通过串口观察打印信息，检查打印信息：

```
U-Boot 2017.03 (Apr 12 2026 - 01:41:12 -0400)   //版本和编译时间，确认是你刚编译的
CPU:   Freescale i.MX6ULL rev1.1 696 MHz (running at 396 MHz)
DRAM:  512 MiB  								 //DDR 容量是否正确
MMC:   FSL_SDHC: 0, FSL_SDHC: 1					 //SD 卡和 eMMC 是否识别
Display: TFT43AB (480x272)
In:    serial									//串口正常
Out:   serial
Err:   serial
Net:   No ethernet found. 						//网络状态
```

然后进入命令行后手动检查存储设备：

```bash
mmc dev 0    # 切换到 SD 卡
mmc info     # 查看信息
mmc dev 1    # 切换到 eMMC
mmc info
```

**如果 DDR、串口、存储都正常**，说明 SoC 级别初始化没有问题，接下来只需处理板级外设差异。



#### 检查结果：

| 功能 | 状态 | 说明 |
|------|------|------|
| DDR | 正常 | 512 MiB 识别正确 |
| SD卡 (mmc0) | 正常 | mmc info 显示SD卡信息 |
| eMMC (mmc1) | 正常 | mmc info 显示eMMC信息 |
| 串口 | 正常 | 能正常交互 |
| LCD | 不正常：参数不匹配 | 默认480x272，需要根据实际屏幕调整（因为项目不涉及LCD，所以本次不做） |
| 网络 | 不正常：未工作 | `No ethernet found`，需要移植 |

结论：基础功能正常，需要做板级外设移植中的移植网络。

---

## 四、添加自己的板子

### 4.1 复制并重命名为自己名字的文件

#### 需要复制的文件

| 文件      | 源路径                    | 操作                     |
| --------- | ------------------------- | ------------------------ |
| defconfig | `configs/<evk>_defconfig` | 复制，改名为自己的板子名 |
| 头文件    | `include/configs/<evk>.h` | 复制，改名               |
| 板级目录  | `board/<vendor>/<evk>/`   | 整个目录复制，改名       |
| 设备树    | `arch/arm/dts/<evk>.dts`  | 复制，改名               |

```bash
# defconfig
cp configs/mx6ull_14x14_evk_defconfig configs/mx6ull_myboard_defconfig

# 头文件
cp include/configs/mx6ull_14x14_evk.h include/configs/mx6ull_myboard.h

# 板级目录
cp -r board/freescale/mx6ull_14x14_evk board/freescale/mx6ull_myboard

# 设备树
cp arch/arm/dts/imx6ull-14x14-evk.dts arch/arm/dts/imx6ull-myboard.dts
```

### 4.2 复制后需要修改文件的内容

**defconfig**：把里面涉及旧板子名的配置项（如 `CONFIG_TARGET_xxx`、`CONFIG_DEFAULT_DEVICE_TREE`）改成新名字。

![image-20260412170537416](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412170537416.png)

**头文件**：修改 `#ifndef` 宏定义守卫名称。

**板级目录下的文件**：

| 文件          | 改什么                        |
| ------------- | ----------------------------- |
| `Makefile`    | obj-y 编译的 .o 文件名        |
| `Kconfig`     | config 标识符和引用的头文件名 |
| `MAINTAINERS` | 板子信息和文件路径            |
| `.c` 源文件   | 重命名，如 `mx6ull_myboard.c` |

**Kconfig 注册**：编辑 `arch/arm/cpu/armv7/mx6/Kconfig`（路径因 SoC 而异），参考原有的config条目，添加你自己的 config 条目及source来源。

![image-20260412171703068](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412171703068.png)

![image-20260412165729151](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412165729151.png)

**设备树相关**：

- 在 `arch/arm/dts/Makefile` 中添加自己设备树的 dtb 的编译条目
- 头文件中更新 `fdt_file` 环境变量里的 dtb 文件名为自己的设备树
- defconfig 中更新 `CONFIG_DEFAULT_DEVICE_TREE`为自己的设备树

**板级 .c 文件中的显示信息**：`checkboard()` 函数中的板子名打印可以改成自己的。

编译验证：

```bash
make distclean
make mx6ull_myboard_defconfig
make -j$(nproc)
```

编译通过就说明板子添加成功。此时功能和 EVK 完全一样。

---

## 五、网络驱动移植

### 5.1 硬件架构理解

网络分两层硬件：

- **SoC 内部的 MAC 控制器**（如 i.MX6ULL 的 FEC）：芯片原厂已经写好驱动，不需要改
- **外部 PHY 芯片**（如 LAN8720A、KSZ8081）：通过标准 RMII 接口与 MAC 连接

RMII 数据接口是标准的，所有 PHY 芯片都一样。**硬件差异的来源**是 PHY 芯片的额外控制引脚：

| 引脚        | 功能               | 是否需要关注                                                 |
| ----------- | ------------------ | ------------------------------------------------------------ |
| nRST        | 复位（低电平有效） | **是**——通常和芯片原厂的不一样，我们具体的板子可能接到不同的 GPIO |
| PHYAD0/1/2  | 地址选择           | **是**——我们具体的板子连的上下拉决定地址值具体是什么，可能与芯片原厂的地址值不一样 |
| nINT        | 中断输出           | U-Boot 中不需要（轮询模式）                                  |
| MODE0/1/2   | 工作模式选择       | 硬件电阻决定，软件不管                                       |
| XTAL1/CLKIN | 参考时钟           | 通常和芯片原厂的参考板一致，确认即可，一致就不需要修改       |

### 5.2 分析硬件差异的方法

按照"从代码出发"的方法论进行：

1. 读头文件（.h）中网络相关的宏定义
2. 读板级C文件（.c）中网络相关的函数
3. 读设备树（.dts）中网络相关的节点和引脚配置
4. 识别其中所有硬件相关的部分
5. 对比自己的原理图，确认哪些需要修改

### 5.3 头文件分析与修改

通过 grep 找到网络相关的宏定义：

```bash
grep -n "PHY\|FEC\|ENET\|CONFIG_PHY\|CONFIG_FEC" include/configs/mx6ull_myboard.h
```

发现需要修改的部分：

#### **修改一：PHY驱动使能**

NXP EVK 使用 KSZ8081（Micrel公司），我的板子使用 LAN8720A（SMSC公司），需要将其改成我的网络芯片的公司提供的驱动的使能。

```c
// 修改前
#define CONFIG_PHY_MICREL

// 修改后
#define CONFIG_PHY_SMSC
```

![image-20260412184800588](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412184800588.png)

#### **修改二：ENET1的PHY地址**

**什么是PHY地址**：SoC通过MDIO管理总线和PHY芯片通信（读写PHY内部寄存器来配置速率、查询链路状态等）。MDIO总线上可以挂多个PHY，每个PHY有一个5位地址（0~31），就像I2C从机地址一样。U-Boot需要知道PHY的地址才能通过MDIO找到它。

**怎么查**：PHY地址由芯片的特定引脚在复位时刻的电平决定。不同PHY芯片的地址引脚不同，以LAN8720A为例：

- 只有一个地址引脚：PHYAD0（pin 10，和RXER复用）
- 复位时 PHYAD0 = 0（下拉到GND）→ PHY地址 = 0
- 复位时 PHYAD0 = 1（上拉到VDD）→ PHY地址 = 1

在原理图中找到PHY芯片的地址引脚，看它通过电阻接到了VDD还是GND，就能确定地址。如果有多个地址引脚（如KSZ8081有PHYAD[2:0]），需要看每个引脚的状态，组合成地址值。

**怎么改**：在头文件中找到 `CONFIG_FEC_MXC_PHYADDR` 或类似的宏定义，改成你板子上PHY的实际地址。如果有两路网络（如 ENET1 和 ENET2），可能有两个地址需要分别配置。

**对于我的开发板从原理图确认如下**：核心板上ENET1 : LAN8720A 的 PHYAD0 引脚（pin 10）通过电阻下拉到 DGND，所以 ENET1的PHY 地址为 0，芯片原厂提供的ENET1地址为2，需要修改。 底板上ENET2的PHYAD0引脚上拉连接到VDD，PHY地址为1，与芯片原厂提供的ENET2地址相同，不需要修改。

```c
// 修改前
#define CONFIG_FEC_MXC_PHYADDR  0x2

// 修改后
#define CONFIG_FEC_MXC_PHYADDR  0x0
```

![image-20260412184823488](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412184823488.png)

**修改三：添加默认MAC地址和IP到环境变量**

U-Boot 2017.03 使用 DM_ETH 框架，要求必须设置 MAC 地址。在 `CONFIG_EXTRA_ENV_SETTINGS` 宏中添加地址环境变量（根据自己虚拟机和电脑的网络地址对应设置）：

```c
"ethaddr=00:04:9f:04:d2:35\0" \
"eth1addr=00:04:9f:04:d2:36\0" \
"ipaddr=192.168.5.9\0" \
"serverip=192.168.5.11\0" \
```

![image-20260412233807545](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412233807545.png)

### 5.3 板级C文件分析与修改

PHY复位分为硬件复位和软件复位，软件复位在刚刚使能的PHY芯片厂家提供的驱动中由芯片厂家已经做在自己的驱动代码里了，我们需要实现的是在开发板上电时使得CPU能找到这个PHY芯片的硬件复位，所以硬件复位是需要做在板级.c文件里的。

通读整个 `mx6ull_myboard.c`，识别网络相关代码（被 `#ifdef CONFIG_FEC_MXC` 包裹的部分），发现有两个函数：

**setup_fec 函数**：配置 FEC MAC 控制器的参考时钟。通过对比硬件原理图后确认我的板子的时钟方案与NXP公司的一致（SoC 提供 50MHz REF_CLK 给 PHY），不需要修改时钟配置部分。

但需要**添加 PHY 硬件复位代码**，因为原厂代码中没有 PHY 复位的 GPIO 操作。NXP EVK 的 PHY 复位是通过一个 74HC595 移位寄存器间接控制的（在设备树中以 gpio expander 形式定义），我的板子没有 74HC595，PHY 复位引脚直接接到 SoC 的 GPIO 上，所以需要在 `setup_fec` 中手动添加 GPIO 复位代码，之所以在这里添加，是因为我们需要实现的是在开发板上电时使得CPU能找到这个PHY芯片的硬件复位，这个函数就是上电时执行的初始化，所以放在这里最合适。

查看我们的硬件原理图，找到我们的 PHY 复位引脚：

底板原理图（ENET2）：

![image-20260413043337904](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260413043337904.png)

![image-20260413043429863](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260413043429863.png)

核心板原理图 (ENET1)：

![image-20260413043535142](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260413043535142.png)

- ENET1（核心板 U6）：RST 引脚连到 SoC 的 `SNVS_TAMPER9` = `GPIO5_IO09`
- ENET2（底板 U11）：RST 引脚通过网络标号 `ENET2_nRST` 连到 SoC 的 `SNVS_TAMPER6` = `GPIO5_IO06`

添加的复位代码：

```c
#define ENET1_RESET_GPIO    IMX_GPIO_NR(5, 9)	//定义引脚
#define ENET2_RESET_GPIO    IMX_GPIO_NR(5, 6)

// 在 setup_fec 函数的 enable_enet_clk(1) 之后参照LAN8720A芯片手册的复位要求，添加硬件复位操作：
if (fec_id == 0) {				//这是ENET2的复位
    gpio_request(ENET1_RESET_GPIO, "enet1_reset");
    gpio_direction_output(ENET1_RESET_GPIO, 0);
    mdelay(20);
    gpio_direction_output(ENET1_RESET_GPIO, 1);
    mdelay(100);
} else {								//这是ENET1的复位
    gpio_request(ENET2_RESET_GPIO, "enet2_reset");
    gpio_direction_output(ENET2_RESET_GPIO, 0);
    mdelay(20);
    gpio_direction_output(ENET2_RESET_GPIO, 1);
    mdelay(100);
}
```

**board_phy_config 函数**：原代码中有一行 `phy_write(phydev, MDIO_DEVAD_NONE, 0x1f, 0x8190)`，这是针对 KSZ8081 的特定寄存器配置。LAN8720A 的寄存器定义不同，直接删掉这行。

![image-20260412213029167](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412213029167.png)

### 5.4 PHY芯片的驱动层修改

在 `drivers/net/phy/phy.c` 的 `genphy_update_link` 函数中添加 LAN8720A 的软复位补丁（参考正点原子的做法）。LAN8720A 在某些情况下首次链路协商时需要额外的软复位才能正常建立链路,这是这个芯片的额外步骤。

```c
#ifdef CONFIG_PHY_SMSC
    static int lan8720_flag = 0;
    int bmcr_reg = 0;
    if (lan8720_flag == 0) {
        bmcr_reg = phy_read(phydev, MDIO_DEVAD_NONE, MII_BMCR);
        phy_write(phydev, MDIO_DEVAD_NONE, MII_BMCR, BMCR_RESET);
        while(phy_read(phydev, MDIO_DEVAD_NONE, MII_BMCR) & 0X8000) {
            udelay(100);
        }
        phy_write(phydev, MDIO_DEVAD_NONE, MII_BMCR, bmcr_reg);
        lan8720_flag = 1;
    }
#endif
```

![image-20260412215043607](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412215043607.png)

### 5.5 设备树修改

设备树中的PHA地址要和头文件里保持一致，也要修改ENET1的PHY地址。

修改 `arch/arm/dts/imx6ull-myboard.dts` 中 ENET1 的 PHY 地址：

```dts
// 修改前
ethphy0: ethernet-phy@2 {
    reg = <2>;
};

// 修改后
ethphy0: ethernet-phy@0 {
    reg = <0>;
};
```

ENET2 的 PHY 地址（reg = 1）与实际一致，不需要改。网络引脚配置（pinctrl_enet1、pinctrl_enet2）是 RMII 标准专用引脚，不需要改。

![image-20260412200402505](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260412200402505.png)

### 5.6 Kconfig 配置（关键步骤）

这是移植中遇到的最大坑之一。做完以上所有代码修改后，编译烧录启动开发板后发现网络仍然不工作（`No ethernet found`）。

排查发现：U-Boot 2017.03 处于从"头文件定义配置"向"Kconfig管理配置"的过渡期。`CONFIG_PHYLIB`、`CONFIG_PHY_SMSC`、`CONFIG_FEC_MXC` 虽然在头文件中用 `#define` 定义了，但实际已被 Kconfig 接管。使用grep命令检查 `.config` 里这些宏定义的配置发现这些选项都是 `not set`。说明被 Kconfig 覆盖了，需要通过 `make menuconfig` 来使能。

```c
grep "CONFIG_XXX" .config
```

通过 `make menuconfig` 使能：

```
Device Drivers →
    Ethernet PHY (physical media interface) support → [*]
        SMSC PHY support → [*]
    Network device support →
        FEC Ethernet controller → [*]
```

![image-20260413040759206](C:\Users\Administrator\AppData\Roaming\Typora\typora-user-images\image-20260413040759206.png)

选上后重新make编译，FEC会询问 MDIO 基地址，输入 `0x020B4000`（ENET2_BASE_ADDR）。现在的uboot镜像就是完全修改好以后的镜像了。因为我们使能这些配置是用menuconfig使能的，配置只保存在.config里，为了防止之后make distclean后配置消失，需要保存配置到defconfig源文件中，这样每次make defconfig后生成的配置,config 都是我们完全改好之后的配置

**保存配置到defconfig**（防止 distclean 后丢失）：

```bash
make savedefconfig
cp defconfig configs/mx6ull_myboard_defconfig   //改名替换掉我们自己的defconfig
# 然后手动确认 defconfig 中 CONFIG_DEFAULT_DEVICE_TREE 等是否正确，配置是否保存成功
```

---

## 六、测试与验证

### 6.1 网络功能验证

编译烧录后，在U-Boot命令行测试：

```bash
=> ping 192.168.5.11
ethernet@020b4000 Waiting for PHY auto negotiation to complete.... done
Using ethernet@020b4000 device
host 192.168.5.11 is alive
```

PHY 自动协商完成，ping 成功，网络移植完成。

```bash
=> mdio list
FEC1:
1 - SMSC LAN8710/LAN8720 <--> ethernet@020b4000
```

MDIO 总线上正确识别了 LAN8720A PHY。网络移植成功！

关于启动时显示 `No ethernet found`：这是 DM_ETH 框架的正常行为（懒加载机制），不影响实际网络功能。通过 `dm tree` 可以看到两个 eth 设备都已注册，只是尚未 probe。

关于只显示一个 PHY 设备：头文件中 `CONFIG_FEC_ENET_DEV=1` 指定了当前只使用 ENET2。`setup_fec` 只初始化了一个网口，这是 NXP 原版代码的设计。两个网口的底层配置（PHY 驱动、地址、复位 GPIO）都已在代码中实现，切换 `CONFIG_FEC_ENET_DEV` 为 0 即可使用 ENET1。

### 6.2 启动内核验证

这是 U-Boot 移植的最终验证——U-Boot 的核心任务就是启动 Linux 内核。这里因为我们是在SD卡里启动UBOOT，此时SD卡中还没有内核和设备树 根文件系统，所以通过SD卡启动，三秒内进入uboot命令行，在uboot命令行中使用命令加载emmc中的内核 设备树 挂载emmc中的根文件系统，并使用bootz命令启动内核，验证我们SD卡中的uboot是否能启动内核

```bash
=> mmc dev 1
=> ls mmc 1:2 /boot
    39327 100ask_imx6ull-14x14.dtb
  7924872 zImage

=> ext2load mmc 1:2 0x80800000 /boot/zImage  # 从 eMMC 加载内核和设备树到内存
=> ext2load mmc 1:2 0x83000000 /boot/100ask_imx6ull-14x14.dtb
=> setenv bootargs 'console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw' # 设置内核启动参数
=> bootz 0x80800000 - 0x83000000  # 启动内核
```

内核成功启动，进入Linux shell。U-Boot移植验证通过。

---

## 七、修改文件汇总

| 文件 | 修改内容 |
|------|---------|
| `include/configs/mx6ull_myboard.h` | PHY地址（0x2→0x0）、PHY驱动宏（MICREL→SMSC）、默认MAC/IP环境变量、fdt_file名 |
| `board/freescale/mx6ull_myboard/mx6ull_myboard.c` | 添加PHY复位GPIO代码、删除KSZ8081寄存器写入、修改checkboard打印 |
| `drivers/net/phy/phy.c` | 添加LAN8720A软复位延时补丁 |
| `arch/arm/dts/imx6ull-myboard.dts` | ENET1 PHY地址改为0 |
| `arch/arm/dts/Makefile` | 添加 `imx6ull-myboard.dtb` 编译条目 |
| `configs/mx6ull_myboard_defconfig` | 添加PHYLIB、PHY_SMSC、FEC_MXC的Kconfig使能、更新DEVICE_TREE名 |

---

## 八、uboot网络移植过程中的踩坑记录与经验总结

### 8.1 Kconfig 与头文件冲突

**现象**：头文件里明确写了 `#define CONFIG_PHYLIB` 和 `#define CONFIG_PHY_SMSC`，但编译后检查 `.config` 发现都是 `not set`。

**原因**：U-Boot 2017.03 处于配置系统过渡期，部分配置项已被 Kconfig 接管。Kconfig 的优先级高于头文件的 `#define`。

**解决**：通过 `make menuconfig` 手动使能。修改后用 `make savedefconfig` 保存到 defconfig 文件，防止 `make distclean` 后丢失。

**经验**：遇到"明明定义了但不生效"的情况，第一步是 `grep xxx .config` 检查实际编译配置。

### 8.2 DM_ETH 框架的 MAC 地址问题

**现象**：PHYLIB 和 FEC 都使能后，ping 时报 `Error: ethernet@020b4000 address not set`。

**原因**：2017版 U-Boot 启用了 DM_ETH（设备模型以太网框架），要求环境变量中必须有 `ethaddr`，否则网络设备拒绝工作。旧版（2016）不用 DM_ETH，没有这个要求。

**解决**：在头文件的 `CONFIG_EXTRA_ENV_SETTINGS` 中添加默认 `ethaddr` 和 `eth1addr`。

### 8.3 PHY 硬件复位的必要性

**现象**：所有配置都正确，但 `mdio list` 为空，MDIO 总线上找不到 PHY。

**原因**：PHY 芯片没有被硬件复位。LAN8720A 上电后如果 RST 引脚没有经历正确的复位时序，可能不会进入正常工作状态。NXP EVK 通过 74HC595 间接控制复位，我的板子是直接 GPIO 控制，但代码中没有复位操作。

**解决**：在 `setup_fec` 函数中添加 GPIO 复位代码（拉低20ms→拉高→等待100ms）。

### 8.4 启动时 "No ethernet found" 不代表网络不工作

**现象**：启动信息显示 `Net: No ethernet found`，但手动执行 `ping` 时网络正常工作。

**原因**：DM_ETH 是懒加载的，启动时 `eth_initialize()` 只是注册设备，不执行 probe。`No ethernet found` 只是这个阶段的打印，实际使用网络命令时设备才会被 probe。

**验证**：用 `dm tree` 命令可以看到 `eth` 设备已注册但未 probe（显示 `[ ]` 而非 `[+]`）。

### 8.5 原理图怎么看

核心板和底板之间通过连接器连接，同一个信号可能在两张原理图上以不同的名字出现。利用**网络标号**（同名标号表示电气连接）和**页面引用标记**（花括号数字表示该信号在其他页面出现）来跨页追踪。

SoC 引脚名到 GPIO 编号的转换：对于 `SNVS_TAMPERx` 系列引脚，直接对应 `GPIO5_IOx`。其他引脚需查参考手册或搜索源码中的引脚宏名（宏名末段直接标明 GPIO 编号）。
