# SPI 控制器驱动开发：基于i.MX6ULL ECSPI1

## 概述

基于 Linux SPI 子系统框架，为 i.MX6ULL 的 ECSPI 控制器编写控制器驱动。采用填充 `spi_master` 的 `transfer_one` 回调的方法，实现 PIO 轮询和中断驱动两种传输模式，通过模块参数 `use_irq` 运行时切换。配合 ADXL345 SPI 设备驱动完成端到端功能验证，使用 Ftrace 和自动化 Shell 脚本进行性能对比分析。

## 基于的框架

- 参考官方驱动：`drivers/spi/spi-imx.c`
- 使用的 SPI 控制器：ECSPI1（寄存器基地址 `0x02008000`）
- 片选方案：GPIO CS（GPIO4_IO26），非硬件 SS

## 实现内容

| 功能 | 说明 |
|------|------|
| transfer_one | 核心传输回调，根据 `use_irq` 参数分发到 PIO 或中断模式 |
| PIO 轮询模式 | SMC=1 立即启动，逐字节写 TX → 轮询 RR → 读 RX |
| 中断驱动模式 | 预填充 FIFO → 使能 RREN 中断 → ISR 批量读 RX 补 TX → completion 唤醒 |
| setup | 按 spi_device 配置 CPOL/CPHA/SS_POL/CM |
| 时钟分频 | 两级分频（PRE+POST），DIV_ROUND_UP 确保不超过目标频率 |
| 多字长支持 | 8/16/32bit 通过 write_one_word/read_one_word 辅助函数处理 |
| ADXL345 设备驱动 | SPI 设备驱动 + 字符设备接口，probe 读 Device ID 验证，read 读三轴加速度 |

## 文件说明

```
spi_driver/
├── spi_adapter.c            # SPI 控制器驱动源码（PIO + 中断双模式）
├── spi_adx1345_drv.c        # ADXL345 SPI 设备驱动
├── adx1345_app.c            # 用户态应用程序（读取加速度数据）
├── spi_bench.c              # SPI 基准测试工具（可配置字节数和迭代次数）
├── spi_perf_compare.sh      # PIO/中断模式性能对比脚本
├── Makefile
├── README.md
```

设备树修改见 `dtb/imx6ull-my14x14-emmc.dts`，ECSPI1 节点覆盖及 pinctrl 配置：

```dts
&iomuxc {
    pinctrl_my_ecspi1: myecspi1grp {
        fsl,pins = 
            MX6UL_PAD_CSI_DATA04__ECSPI1_SCLK   0x100b1
            MX6UL_PAD_CSI_DATA05__GPIO4_IO26    0x100b1
            MX6UL_PAD_CSI_DATA06__ECSPI1_MOSI   0x100b1
            MX6UL_PAD_CSI_DATA07__ECSPI1_MISO   0x100b1
        >;
    };
};

&ecspi1 {
    compatible = "zxr-my_spi_master";
    pinctrl-0 = <&pinctrl_my_ecspi1>;
    cs-gpios = <&gpio4 26 GPIO_ACTIVE_LOW>;
    status = "okay";

    adxl345@0 {
        compatible = "zxr-myadx";
        reg = <0>;
        spi-max-frequency = <1000000>;
    };
};
```

## 编译与部署

```bash
# 编译
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-
make

# 部署到板子
cp spi_adapter.ko spi_adx1345_drv.ko adxl345_app spi_bench spi_perf_compare.sh /mnt/mydriver/spi/

# 加载（PIO模式）
insmod spi_adapter.ko

# 加载（中断模式）
insmod spi_adapter.ko use_irq=1
```

设备树需重新编译并更新到板子：

```bash
make dtbs
cp arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb /boot/
```

## 验证结果

### 功能验证（ADXL345 传感器）

```bash
insmod spi_adapter.ko
insmod spi_adx1345_drv.ko       # dmesg: "ADXL345 initialized"
./adxl345_app                    # 输出: X: 12  Y: -8  Z: 256
```

### 性能对比（Shell 脚本，500 次迭代取平均）

```bash
sh spi_perf_compare.sh
```

| 数据量 | PIO 延迟 | 中断延迟 | 结论 |
|--------|---------|---------|------|
| 2B | 150 μs | 188 μs | PIO 快 25% |
| 32B | 487 μs | 498 μs | 基本持平 |
| 128B | 1729 μs | 1657 μs | 中断快 4.3% |

Ftrace 追踪 2 字节传输调用链：PIO 模式 transfer_one 耗时 125μs，中断模式含上下文切换约 3ms。
