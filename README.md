
i.MX6ULL BSP 构建与多子系统控制器驱动开发

在 100ASK i.MX6ULL Pro 开发板上从零构建嵌入式 Linux 平台（U-Boot / 内核 / 根文件系统移植），并在其上编写 SPI / I2C / UART / GPIO+中断 多个子系统的控制器驱动——填充内核子系统框架的硬件相关层，让子系统本身在该板上工作。配套 Ftrace / strace 调试与性能分析。

[Show Image](https://img.shields.io/badge/platform-i.MX6ULL-blue) [Show Image](https://img.shields.io/badge/kernel-4.9.88-green) [Show Image](https://img.shields.io/badge/license-GPL--3.0-orange)
项目定位
不同于"调用子系统接口写设备驱动"，本项目编写的是控制器驱动：基于内核框架结构体（spi_master / i2c_adapter / uart_port / gpio_chip + irq_chip/irq_domain），查 i.MX6ULL 参考手册操作真实寄存器，填充硬件相关层，让 SPI / I2C / UART / GPIO 子系统在本板上运行。

技术栈
C、Linux Kernel 4.9、U-Boot 2017.03、Buildroot、Device Tree、Platform Bus、SPI/I2C/UART/GPIO/Interrupt 子系统、Ftrace / strace / trace-cmd
关键成果

全链路 BSP：从 bootloader 到 shell 自主搭通。
SPI 双模式 + 性能刻画：Shell + Ftrace 量化 PIO vs 中断，得交叉点结论。
GPIO 子中断控制器：irq_chip + irq_domain + chained handler，处理共享中断线分发。
调试与性能分析：strace 追系统调用通路、Ftrace 追中断调用链并定位热点（如定位到 GPIO 中断处理中 printk 占主要耗时）。

硬件平台
项目说明开发板100ASK i.MX6ULL Pro（韦东山）SoCNXP i.MX6ULL（Cortex-A7）内存 / 存储512MB DDR3 / 4GB eMMC网络双 LAN8720A PHY（RMII，MDIO 地址 0 / 1）验证外设ADXL345（SPI）、AP3216C（I2C）、SR501（GPIO 中断）等
整体架构
        应用 / 测试   (i2c-tools · spidev app · benchmark 脚本)
                          │
══════════════  控制器驱动（本项目）  ══════════════

   spi_master   i2c_adapter   uart_port   gpio_chip + irq_chip/irq_domain
                          │  readl / writel
                          
              i.MX6ULL 寄存器 (ECSPI / I2C / UART / GPIO)
              
══════════════  BSP（本项目）  ══════════════
   U-Boot 2017.03   →   Linux 4.9.88 内核   →   Buildroot 根文件系统
目录结构
.
├── uboot/        # U-Boot 移植：patches/(板级补丁 + defconfig)、docs/(移植记录)
├── kernel/       # 内核移植：patches/(dts / defconfig / 补丁)、docs/(移植记录)
├── rootfs/       # 根文件系统：configs/(Buildroot defconfig)、overlay/、docs/
├── drivers/      # 多子系统控制器驱动
│   ├── uart_adapter_driver/        # UART 控制器驱动
│   ├── spi_driver/                 # SPI 控制器驱动 + ADXL345 设备驱动 + 性能 benchmark
│   ├── i2c_adapter_driver/         # I2C 适配器驱动
│   ├── gpio_interrupt_controller/  # GPIO + 子中断控制器驱动
│   └── dtb/                        # 设备树 dts / dtb
├── docs/         # 项目级文档
├── LICENSE
└── README.md

BSP 的改动以 patch + 自定义 defconfig/dts 形式存放（不重分发 NXP 原厂源码树），应用到对应版本的原厂源码即可复现，具体部署步骤参考uboot目录下README.md。

模块说明
BSP 构建

U-Boot 移植（uboot/）：基于 NXP U-Boot 2017.03（imx_v2017.03_4.9.88_2.0.0_ga），适配 LAN8720A 双 PHY、eMMC/SD 启动、bootargs/bootcmd。改动拆为 6 个 patch：板级头文件、板级 C 文件、LAN8720A PHY fix、板级 dts、dts Makefile、eMMC bootargs。
内核移植（kernel/）：基于 Linux 4.9.88（imx_4.9.88_2.0.0_ga），设备树适配双 PHY、eMMC 电压（固定 3.3V、关电压切换）、SD 卡检测；自定义 dts + defconfig。
根文件系统（rootfs/）：基于 Buildroot，按调试需求集成 Ftrace / strace / i2c-tools 等工具，overlay 放自定义配置。

子系统控制器驱动（drivers/）

SPI 控制器驱动：基于 spi_master 的 transfer_one，实现 PIO 轮询与中断两种传输模式、模块参数运行时切换；含 ADXL345 设备驱动、应用程序、Shell + Ftrace 性能 benchmark，量化得出小数据量 PIO 占优、大数据量中断占优的交叉结论
GPIO 与子中断控制器驱动：单一驱动内实现 gpio_chip + irq_chip/irq_domain（linear 映射 + chained handler），解决 32 引脚共享 2 根 GIC 中断线的分发，支持 5 种触发类型，与 Pinctrl 子系统联动。
I2C 适配器驱动：基于 i2c_algorithm 的 master_xfer，实现 START / Repeated START / STOP 时序与 ACK/NACK 处理、中断 + 等待队列收发，i2c-tools 验证 AP3216C。
UART 控制器驱动：基于 serial/tty 框架，实现中断收发、波特率配置、console 输出，回环测试验证。


各子系统目录下有独立 README.md 与开发记录文档。


构建与运行
U-Boot
bash# 在 NXP U-Boot 2017.03 源码中应用 patches，使用提供的 defconfig
make mx6ull_myboard_defconfig
make -j$(nproc)
# 烧写到 SD 卡（seek 值由 BootROM 规定）
sudo dd if=u-boot-dtb.imx of=/dev/sdX bs=1024 seek=1 conv=fsync
内核
bash# 在 NXP linux-imx 4.9.88 源码中应用 patch、放入 dts 与 defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- my_imx_emmc_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs -j$(nproc)
根文件系统
bash# 在 Buildroot 中放入 configs/my_imx6ull_defconfig 与 overlay/
make my_imx6ull_defconfig
make
子系统驱动
bashcd drivers/<subsystem>/
make            # 交叉编译出 .ko
# 拷贝到开发板后 insmod 加载
文档

各子系统与 BSP 的详细开发记录见对应 docs/ 目录
CSDN 博客专栏：https://blog.csdn.net/m0_75087254/category_13178248.html

License
GPL-3.0

