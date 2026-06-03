# I2C 控制器驱动开发：基于i.MX6ULL I2C1

## 概述

基于 Linux I2C 子系统框架，为 i.MX6ULL 的 I2C 控制器编写适配器驱动。实现 `i2c_algorithm` 的 `master_xfer` 回调，通过中断 + 等待队列机制完成字节级总线传输，支持标准模式（100kHz）。与官方 `i2c-imx.c` 驱动并列存在，使用自定义 compatible 匹配。

## 基于的框架

- 参考官方驱动：`drivers/i2c/busses/i2c-imx.c`
- 使用的 I2C 控制器：I2C1（寄存器基地址 `0x021a0000`）

## 实现内容

| 功能 | 说明 |
|------|------|
| master_xfer | 遍历 i2c_msg 数组，按 I2C 协议完成 START → 地址 → 数据传输 → STOP |
| 中断驱动传输 | 每字节传输完成产生中断，ISR 存状态并唤醒 master_xfer |
| ACK/NACK 检测 | 写操作检查从机应答，读操作最后字节发 NACK |
| Repeated START | 支持多 msg 间不释放总线的连续传输（先写后读场景） |
| 时钟分频配置 | probe 中根据设备树 clock-frequency 查 64 项分频表配置 IFDR |
| SMBus 兼容 | 通过 I2C 核心层自动模拟，functionality 返回 I2C_FUNC_SMBUS_EMUL |

## 文件说明

```
i2c_adapter_driver/
├── i2c_adapter.c          # I2C 控制器器驱动源码
└── Makefile               
└── docs  
```

设备树修改见 `dtb/imx6ull-my14x14-emmc.dts`，I2C1 节点覆盖：

```dts
&i2c1 {
    compatible = "i2c-bus-zxr";
    /delete-node/ mag3110@0e;
    /delete-node/ fxls8471@1e;
};
```

## 编译与部署

```bash
# 交叉编译
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-
make -C <kernel_source> M=$(pwd) modules

# 部署到板子（NFS 或 scp）
cp i2c_adapter.ko /mnt/mydriver/

# 加载
insmod i2c_adapter.ko
```

设备树需重新编译并更新到板子：

```bash
make dtbs
cp arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb /boot/
```

## 验证结果

使用 i2c-tools 通过板载 AP3216C 光强距离传感器（I2C 地址 0x1e）验证：

```bash
# 检查适配器注册
i2cdetect -l                    # 显示 zxr-i2c

# 扫描总线设备
i2cdetect -y 0                  # 0x1e 位置检测到设备

# 写入配置（开启 ALS+PS 模式）
i2cset -f -y 0 0x1e 0 0x3

# 读取光强和距离数据
i2cget -f -y 0 0x1e 0x0c w     # 光强值，遮挡后变化
i2cget -f -y 0 0x1e 0x0e w     # 距离值，靠近后变化
```

调试验证：strace 追踪 I2C 系统调用链（open → ioctl → close），中断计数正常增长。

## 详细文档

- [开发记录](./docs/i2c_controller_driver_record.md)