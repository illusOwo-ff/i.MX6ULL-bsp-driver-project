> 本文是个人项目记录（三）根文件系统构建：基于i.MX6ULL的嵌入式Linux终端系统构建与多子系统控制器驱动开发—使用Buildroot-2020.02.12构建满足项目需求的根文件系统，记录了使用Buildroot构建满足项目需求的根文件系统并部署在在百问网100ASK IMX6ULL Pro开发板上的完整过程，是BSP移植项目（U-Boot移植 → 内核移植 → 根文件系统构建）的第三部分。包括内核配置补充修改、Buildroot配置与编译、NFS验证、U-Boot启动参数更新、eMMC部署，以及过程中遇到的所有问题和解决方法。U-Boot移植和内核移植已在前两篇文章中完成
[TOC]



## 一、前置条件

### 1.1 已完成的工作

| 组件     | 状态                                     | 关键信息                                                     |
| -------- | ---------------------------------------- | ------------------------------------------------------------ |
| U-Boot   | 已移植完成                               | 2017.03版本，源码目录 `~/my-uboot-v2017.03-4.9.88/`，板级文件 `mx6ull_myboard` |
| 内核     | 已移植完成                               | 4.9.88版本，源码目录 `~/my-linux-imx-imx_4.9.88.2.0/`，defconfig `my_imx_emmc_defconfig`，设备树 `imx6ull-my14x14-emmc.dts` |
| 启动方式 | SD卡U-Boot + tftp内核 + eMMC韦东山rootfs | bootargs: `console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw` |

### 1.2 工具链

Linaro GCC 6.2.1-2016.11，路径：

```
/home/book/100ask_imx6ull-sdk/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf
```

内核头文件版本：4.6（通过读取工具链中linux/version.h确认，LINUX_VERSION_CODE=263680，换算为4.6.0）。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/af42c1a2d1ee40ae9e974cd0af4653f2.png)
### 1.3 分析项目用到的调试和验证工具包
结合我项目后面驱动部分的需要，需要的包如下：
#### 驱动开发直接需要的调试和验证工具

**i2c-tools**（i2cdetect/i2cget/i2cset/i2cdump）—— I2C适配器驱动的验证工具。

**spi-tools** —— SPI控制器驱动的验证工具。

**devmem（或devmem2）**—— 直接从用户态读写物理地址，也就是直接读写寄存器。

**libgpiod工具集**（gpiodetect/gpioinfo/gpioget/gpioset/gpiomon）—— GPIO控制器驱动的验证工具。

**strace** —— 系统调用追踪，后面做i2c子系统控制器驱动的时候可以从用户态追踪一次i2c读写操作经过了哪些系统调用（open → ioctl → close），理解用户态请求是怎么一路走到控制器驱动的。

**trace-cmd** —— Ftrace的用户态前端工具。

**gdbserver** —— 远程调试用，在板子上跑gdbserver附加到测试程序，主机上用arm-linux-gnueabihf-gdb连过去，可以设断点、单步、查看变量。调试用户态验证程序时很有用。

#### 开发便利性工具

**dropbear（轻量SSH服务器）**—— 不用完全依赖串口终端了，可以同时开多个终端，也可以用scp传文件到板子上

**NFS客户端支持** —— 不用每次都scp拷贝

**nano或vim** —— 文本编辑器

**file和ldd** —— 排查问题用，比如交叉编译了一个测试程序放到板子上跑不起来，用file看看是文件不是ARM架构的ELF，用ldd看看动态库有没有缺失

**hexdump/xxd** —— 查看二进制数据用，比如从SPI或I2C读回一段数据想看原始字节内容。BusyBox自带hexdump
## 二、内核配置补充修改

在构建rootfs之前，还需要先回到内核defconfig检查和补充rootfs及调试工具需要的内核配置，因为这些工具的配置可能在移植内核的时候没有开启

### 2.1 检查结果

对 `my_imx_emmc_defconfig` 进行检查，分三类：

**rootfs基础设施（已开启，无需修改）：**

- CONFIG_DEVTMPFS=y 
- CONFIG_DEVTMPFS_MOUNT=y （通过内核默认值）
- CONFIG_EXT4_FS=y（通过CONFIG_EXT3_FS=y隐含）
- CONFIG_TMPFS=y 

**工具依赖（已开启，无需修改）：**

- CONFIG_I2C_CHARDEV=y （i2c-tools需要）
- CONFIG_SPI_SPIDEV=y （spidev_test需要）
- CONFIG_GPIO_SYSFS=y （GPIO验证需要）
- CONFIG_DEVMEM=y （devmem工具需要，默认开启）
- CONFIG_NFS_FS=y + CONFIG_ROOT_NFS=y （NFS启动需要）
- CONFIG_INPUT_EVDEV=y （evtest需要）

**调试基础设施（需要修改）：**

- `# CONFIG_FTRACE is not set` ← **Ftrace被显式关闭，必须开启**

### 2.2 修改操作

```bash
cd ~/my-linux-imx-imx_4.9.88.2.0
make my_imx_emmc_defconfig
make menuconfig
```

在 Kernel hacking → Tracers 中开启：

- CONFIG_FUNCTION_TRACER=y（函数级追踪）
- CONFIG_FUNCTION_GRAPH_TRACER=y（函数调用图，显示调用层级和耗时）
- CONFIG_IRQSOFF_TRACER=y（关中断延迟追踪）
- CONFIG_SCHED_TRACER=y（调度延迟追踪）

在 Kernel hacking → printk and dmesg options 中开启：

- CONFIG_DYNAMIC_DEBUG=y（运行时动态开关pr_debug输出）

确认 CONFIG_DEVMEM=y（搜索DEVMEM确认为 `[=y]`）。

保存并更新defconfig：

```bash
make savedefconfig
cp defconfig arch/arm/configs/my_imx_emmc_defconfig
```

### 2.3 验证

重新编译内核，tftp启动后检查：

```bash
ls /sys/kernel/debug/tracing/       # 确认tracing目录存在
cat /sys/kernel/debug/tracing/available_tracers
# 输出：function_graph wakeup_dl wakeup_rt wakeup irqsoff function nop
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/da5f0c2f571d413e84368bbaffc55714.png)



function_graph和irqsoff都在available_tracers中，Ftrace配置正确。

---

## 三、Buildroot配置与编译

### 3.1 获取Buildroot源码

选择2020.02.12版本（LTS），和韦东山SDK中的Buildroot_2020.02.x版本一致，便于复用dl目录：

```bash
cd ~
tar -xzf buildroot-2020.02.12.tar.gz
mkdir -p ~/buildroot-2020.02.12/dl
cp -r ~/100ask_imx6ull-sdk/Buildroot_2020.02.x/dl/* ~/buildroot-2020.02.12/dl/
```

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/2f1f5f273b1e4e93a9ed7dc58e968a91.png)

### 3.2 选择配置起点

使用Buildroot自带的 `imx6ulevk_defconfig` 作为起点（NXP官方的i.MX6UL EVK的配置，架构和i.MX6ULL完全一致）：

```bash
cd ~/buildroot-2020.02.12
make imx6ulevk_defconfig
make menuconfig
```

### 3.3 官方defconfig分析与修改
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/0657aa872a0a4945be518f4a5ccdd48d.png)


官方defconfig中每项的处理：

**保留不动（架构，和我的SoC一致）：**

```
BR2_arm=y                      # ARM架构
BR2_cortex_a7=y                # Cortex-A7核心
BR2_ARM_FPU_NEON_VFPV4=y      # NEON/VFPv4硬浮点
```

**删除（内核编译，我是自己编译了内核4.9.88）：**

```
BR2_PACKAGE_HOST_LINUX_HEADERS_CUSTOM_5_4=y
BR2_LINUX_KERNEL=y 及其所有子项（共7行）
```

**删除（U-Boot编译，我是自己编译了uboot 2017.03）：**

```
BR2_TARGET_UBOOT=y 及其所有子项（共8行）
```

**删除（完整镜像生成工具，我是分步部署不需要）：**

```
BR2_PACKAGE_HOST_DOSFSTOOLS=y
BR2_PACKAGE_HOST_GENIMAGE=y
BR2_PACKAGE_HOST_MTOOLS=y
BR2_ROOTFS_POST_IMAGE_SCRIPT="board/freescale/common/imx/post-image.sh"
```

**保留并补充（串口终端配置）：**

```
BR2_TARGET_GENERIC_GETTY_PORT="ttymxc0"   # 保留（和bootargs的console一致）
```

**新增（工具链，从Internal改为External）：**
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/c5c31a04207342d4807409bbd886989c.png)


```
BR2_TOOLCHAIN_EXTERNAL=y
BR2_TOOLCHAIN_EXTERNAL_CUSTOM=y
BR2_TOOLCHAIN_EXTERNAL_PATH="/home/book/100ask_imx6ull-sdk/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf"
BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX="$(ARCH)-linux-gnueabihf"
BR2_TOOLCHAIN_EXTERNAL_GCC_6=y
BR2_TOOLCHAIN_EXTERNAL_HEADERS_4_6=y
BR2_TOOLCHAIN_EXTERNAL_CUSTOM_GLIBC=y
BR2_TOOLCHAIN_EXTERNAL_CXX=y
```

**新增（系统配置）：**

```
BR2_TARGET_GENERIC_HOSTNAME="my-imx6ull"
BR2_TARGET_GENERIC_ISSUE="Welcome to my imx6ull"
BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_MDEV=y
BR2_TARGET_GENERIC_ROOT_PASSWD="123456"
BR2_ROOTFS_OVERLAY="board/my_imx6ull/rootfs-overlay"
```

**新增（软件包）：**

```
BR2_PACKAGE_BUSYBOX_SHOW_OTHERS=y   # 显示和busybox重叠的包（否则i2c-tools被隐藏）
BR2_PACKAGE_GDB=y                    # GDB（含gdbserver）
BR2_PACKAGE_STRACE=y                 # 系统调用追踪
BR2_PACKAGE_TRACE_CMD=y              # Ftrace用户态前端
BR2_PACKAGE_NFS_UTILS=y              # NFS客户端
BR2_PACKAGE_EVTEST=y                 # Input事件测试
BR2_PACKAGE_I2C_TOOLS=y              # I2C调试工具
BR2_PACKAGE_SPI_TOOLS=y              # SPI调试工具
BR2_PACKAGE_DROPBEAR=y               # SSH服务器
BR2_PACKAGE_NANO=y                   # 文本编辑器
```

**保留并补充（文件系统镜像格式）：**

```
BR2_TARGET_ROOTFS_EXT2=y
BR2_TARGET_ROOTFS_EXT2_4=y
# 新增：tar格式（NFS测试用）和ext4兼容性选项（下面遇到问题时添加）
```

**关于libgpiod的说明：** 搜索后发现 `BR2_PACKAGE_LIBGPIOD` 依赖 `BR2_TOOLCHAIN_HEADERS_AT_LEAST_4_8 [=n]`，由于工具链的内核头文件是4.6，无法启用。退回使用GPIO sysfs接口（`/sys/class/gpio/`）验证GPIO驱动，内核已开启CONFIG_GPIO_SYSFS=y。

### 3.4 配置overlay

创建overlay目录和PS1配置脚本，这步是让进入开发板后默认名字可以显示用户名和所在路径。

```bash
mkdir -p ~/buildroot-2020.02.12/board/my_imx6ull/rootfs-overlay/etc/profile.d/
cat > ~/buildroot-2020.02.12/board/my_imx6ull/rootfs-overlay/etc/profile.d/ps1.sh << 'EOF'
export PS1='[\u@\h:\w]# '
EOF
```

在menuconfig中设置 Root filesystem overlay directories = `board/my_imx6ull/rootfs-overlay`。

### 3.5 保存配置

把menuconfig配置好的.config反向保存回defconfig，方便之后可以直接使用

```bash
make savedefconfig BR2_DEFCONFIG=configs/my_imx6ull_defconfig
make show-targets    # 确认目标包列表
```

show-targets输出确认所有工具在列，无linux和uboot：

```
busybox dropbear evtest gdb i2c-tools nano nfs-utils spi-tools strace trace-cmd ... rootfs-ext2 rootfs-tar
```

### 3.6 手动下载dl目录缺失的包，加速编译时间

检查dl目录中缺失的包（韦东山SDK的Buildroot用openssh不用dropbear，也没有nano）：

```bash
# dropbear
grep -E 'DROPBEAR_VERSION|DROPBEAR_SITE' package/dropbear/dropbear.mk
# → https://matt.ucc.asn.au/dropbear/releases/dropbear-2019.78.tar.bz2

# nano
grep -E 'NANO_VERSION|NANO_SITE' package/nano/nano.mk
# → https://www.nano-editor.org/dist/v4/nano-4.7.tar.xz
```

用浏览器下载后放到 `dl/dropbear/` 和 `dl/nano/` 目录下。

### 3.7 编译

```bash
sudo make
```

编译耗时约5分钟（外部工具链+dl缓存，大部分包不需要下载和编译工具链）。

产物：`output/images/rootfs.ext2`（ext4格式）、`rootfs.ext4`（软链接）、`rootfs.tar`。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/45022a9eefe44959a8b5789ea9b8109f.png)


---

## 四、NFS验证

### 4.1 搭建NFS环境

```bash
# 虚拟机上
mkdir -p ~/nfs_rootfs/my_rootfs
sudo tar xf ~/buildroot-2020.02.12/output/images/rootfs.tar -C ~/nfs_rootfs/my_rootfs

# 配置NFS导出
echo '/home/book/nfs_rootfs/my_rootfs *(rw,sync,no_root_squash,no_subtree_check)' | sudo tee -a /etc/exports
sudo exportfs -ra && sudo systemctl restart nfs-kernel-server
```

### 4.2 NFS启动

U-Boot命令行：

```bash
setenv bootargs 'console=ttymxc0,115200 root=/dev/nfs nfsroot=192.168.5.11:/home/book/nfs_rootfs/my_rootfs,v3 ip=192.168.5.9:192.168.5.11:192.168.5.1:255.255.255.0::eth0:off rw'
tftp 80800000 zImage
tftp 83000000 imx6ull-my14x14-emmc.dtb
bootz 80800000 - 83000000
```

### 4.3 验证结果

系统成功启动，显示 `Welcome to my imx6ull`，PS1提示符 `[root@my-imx6ull:~]#` 正确。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/32a08834435042aba0a3457fa4b7ecd7.png)


各工具验证结果：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/3fa21dddb0834906bc85041c83dd5dbd.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/fae2ae91d61348edae8b2f489a183353.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/32dbd15435fe4f90a1062f39050eda6b.png)


关于网卡命名，使用自己构建的rootfs（BusyBox init + mdev）后，没有韦东山rootfs中的udev持久化命名规则，网卡按内核默认命名（eth0/eth1），之前从eth0重命名为eth2的问题自然消失。

---

## 五、U-Boot启动参数更新

### 5.1 检查eMMC分区布局和文件位置

在NFS启动的板子上：

```bash
fdisk -l /dev/mmcblk1
# mmcblk1p1: 1000MB Linux分区
# mmcblk1p2: 1500MB Linux分区（rootfs）
# mmcblk1p3: 500MB FAT32分区

mount /dev/mmcblk1p2 /mnt/emmc
ls /mnt/emmc/boot/
# 韦东山原有的zImage和dtb文件
```

确认：内核和dtb放在eMMC分区2（ext4格式）的/boot/目录下。

### 5.2 分析U-Boot当前配置

检查头文件 `include/configs/mx6ull_myboard.h` 中的启动变量链：

```bash
grep -n 'fdt_file\|mmcroot\|mmcdev\|mmcpart\|CONFIG_BOOTCOMMAND\|loadimage\|loadfdt' include/configs/mx6ull_myboard.h
```

沿CONFIG_BOOTCOMMAND → mmcboot → loadimage/loadfdt/mmcargs的调用链展开，对比实际情况：

| 变量                         | 展开后的值                    | 实际需要                       | 不匹配原因                        |
| ---------------------------- | ----------------------------- | ------------------------------ | --------------------------------- |
| CONFIG_SYS_MMC_IMG_LOAD_PART | 1                             | 2                              | 内核在分区2不是分区1              |
| fdt_file                     | undefined→imx6ull-myboard.dtb | imx6ull-my14x14-emmc.dtb       | 需要内核的设备树，不是U-Boot的    |
| loadimage                    | fatload ... ${image}          | ext2load ... /boot/${image}    | 分区是ext4不是FAT，文件在/boot/下 |
| loadfdt                      | fatload ... ${fdt_file}       | ext2load ... /boot/${fdt_file} | 同上                              |
| mmcdev                       | 1（eMMC）                     | 1                              | 不用改                            |
| mmcroot                      | /dev/mmcblk1p2                | /dev/mmcblk1p2                 | 不用改                            |

### 5.3 修改头文件

修改 `include/configs/mx6ull_myboard.h` 共4处：

```c
// 第70行：加载分区从1改为2
#define CONFIG_SYS_MMC_IMG_LOAD_PART	2

// 第133行：设备树文件名改为内核的设备树
"fdt_file=imx6ull-my14x14-emmc.dtb\0" \

// 第151行：内核加载命令改为ext2load，加/boot/路径
"loadimage=ext2load mmc ${mmcdev}:${mmcpart} ${loadaddr} /boot/${image}\0" \

// 第152行：设备树加载命令同样修改
"loadfdt=ext2load mmc ${mmcdev}:${mmcpart} ${fdt_addr} /boot/${fdt_file}\0" \
```

### 5.4 重新编译U-Boot

```bash
cd ~/my-uboot-v2017.03-4.9.88
make distclean
make mx6ull_myboard_defconfig
make -j$(nproc)
```

产出 `u-boot-dtb.imx`。

---

## 六、eMMC部署

### 6.1 遇到的问题一：ext4 metadata_csum兼容性

**现象：** 通过dd将rootfs.ext2写入eMMC分区2后，mount失败：

```
EXT4-fs (mmcblk1p2): VFS: Found ext4 filesystem with invalid superblock checksum.
EXT2-fs (mmcblk1p2): error: couldn't mount because of unsupported optional features (244)
```

**原因：** 虚拟机上的e2fsprogs版本较新，mke2fs默认创建带有metadata_csum特性的ext4文件系统。4.9.88内核的ext4驱动与新版metadata_csum存在兼容性问题，拒绝挂载。

**我的解决：** 在Buildroot menuconfig中，Filesystem images → ext2/3/4 → additional mke2fs options，将原来的 `-O ^64bit` 改为 `-O ^64bit,^metadata_csum`。重新 `sudo make` 编译。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/4064944783a04767b8ca01dbe79065e5.png)


### 6.2 遇到的问题二：烧录工具写入失败

**现象：** 使用韦东山100ask_imx6ull_flashing_tool的"更新内核"和"更新设备树"功能时，工具日志显示download成功但ext4write失败：

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/449df4c3dcb04c85b0e4561cb003ea1b.png)

**原因：** 和问题一相同——第一次dd的rootfs.ext2带有metadata_csum特性，U-Boot 2017.03的ext4write命令同样不支持这个特性，所以无法往分区中写入文件。

**我的解决：** 在使用修复后的rootfs.ext2（去掉了metadata_csum）重新dd，然后手动通过NFS系统拷贝内核和设备树到/boot目录，不使用烧录工具的"更新内核/设备树"功能。

### 6.3 成功部署流程

**虚拟机上——准备文件到NFS目录：**

```bash
cp ~/buildroot-2020.02.12/output/images/rootfs.ext2 ~/nfs_rootfs/my_rootfs/root/
cp ~/my-linux-imx-imx_4.9.88.2.0/arch/arm/boot/zImage ~/nfs_rootfs/my_rootfs/root/
cp ~/my-linux-imx-imx_4.9.88.2.0/arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb ~/nfs_rootfs/my_rootfs/root/
```

**板子上（NFS启动状态）——写入eMMC：**

```bash
# 烧写rootfs
dd if=/root/rootfs.ext2 of=/dev/mmcblk1p2 bs=1M
sync

# 挂载并放入内核和设备树
mount /dev/mmcblk1p2 /mnt/emmc
mkdir -p /mnt/emmc/boot
cp /root/zImage /mnt/emmc/boot/
cp /root/imx6ull-my14x14-emmc.dtb /mnt/emmc/boot/
sync
umount /mnt/emmc
```

**U-Boot烧写到eMMC：** 使用韦东山烧录工具的"更新Uboot"功能，将u-boot-dtb.imx烧写到eMMC。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/821a91a6893d42aca8ae535fb19ff17a.png)

### 6.4 最终验证


拔掉SD卡，切换拨码开关到eMMC启动模式。上电启动日志确认：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/9bebc939054749f89a46bbe43ecd0351.jpeg)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/6ee8d7666ef849eaa180508ee2ee10fb.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/2807f02ff2144c52a741d27e1ad27f6b.png)

```
U-Boot 2017.03 (May 23 2026 - 11:42:46 -0400)
Board: MX6ULL MYBOARD
...
8938120 bytes read in 509 ms (16.7 MiB/s)          ← zImage从eMMC加载成功
36760 bytes read in 121 ms (295.9 KiB/s)            ← dtb从eMMC加载成功
...
Kernel command line: console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw
...
EXT4-fs (mmcblk1p2): mounted filesystem with ordered data mode  ← rootfs挂载成功
...
Welcome to my imx6ull
my-imx6ull login: root
Password:
[root@my-imx6ull:~]#                               ← 系统完全启动
```

全链路验证通过：eMMC上的U-Boot → 加载eMMC上的内核和设备树 → 挂载eMMC上的rootfs → 进入shell。整个系统从引导加载到用户态环境全部是自己构建的。

---