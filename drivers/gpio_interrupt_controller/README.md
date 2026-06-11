# GPIO 控制器与中断控制器驱动开发：基于 i.MX6ULL GPIO4/GPIO5

## 概述

基于 Linux GPIO 子系统和中断子系统框架，为 i.MX6ULL 编写 GPIO 控制器驱动。在同一驱动文件中实现 `gpio_chip`（方向控制与电平读写）和子中断控制器（`irq_chip` + `irq_domain` + chained handler），通过链式中断处理分辨 32 个引脚共享 2 根 GIC 中断线的问题，支持 5 种触发类型。通过 `gpio-ranges` 实现与 Pinctrl 子系统的联动。与官方 `gpio-mxc.c` 驱动并列存在，使用自定义 compatible 匹配。

## 基于的框架

- 参考官方驱动：`drivers/gpio/gpio-mxc.c`
- 使用的 GPIO 控制器：GPIO5（验证 GPIO 功能，LED）、GPIO4（验证中断功能，SR501）

## 实现内容

**GPIO 控制器部分（gpio_chip）：**

| 功能                     | 说明                                                         |
| ------------------------ | ------------------------------------------------------------ |
| direction_input / output | 操作 GDIR 寄存器控制引脚方向，output 时先写 DR 再改 GDIR 避免毛刺 |
| get / set                | 读 PSR 获取真实电平，读-改-写 DR 设置输出电平                |
| get_direction            | 读 GDIR 返回方向（注意内核约定与寄存器值相反）               |
| request / free           | 填充 gpiochip_generic_request/free，通过 gpio-ranges 联动 Pinctrl 自动复用 |
| to_irq                   | 调用 irq_find_mapping 实现 GPIO 引脚号到 Linux 中断号的转换  |

**中断控制器部分（irq_chip + irq_domain + chained handler）：**

| 功能                  | 说明                                                         |
| --------------------- | ------------------------------------------------------------ |
| irq_mask / irq_unmask | 读-改-写 IMR 寄存器屏蔽/使能单个引脚中断                     |
| irq_ack               | 写 ISR 对应 bit 清除中断状态（write-1-to-clear）             |
| irq_set_type          | 写 ICR1/ICR2 设置触发编码 + EDGE_SEL 处理双边沿 + 切换流控 handler |
| irq_domain            | linear 映射，probe 时预创建 32 个 hwirq→virq 映射            |
| chained handler       | 读 ISR & IMR 得到 pending，遍历分发 generic_handle_irq       |

**并发保护：** spinlock 保护所有读-改-写操作（GPIO 框架和 IRQ 框架不自动保护共享寄存器）。

## 文件说明

```
gpio_interrupt_controller/
├── gpio_interrupt_adapter.c    # GPIO + 中断控制器驱动源码
├── gpio_irq_test_driver.c      # 中断功能验证用的测试设备驱动
├── Makefile
└── README.md
```

设备树修改见 `dtb/imx6ull-my14x14-emmc.dts`，GPIO4/GPIO5 节点覆盖 + 测试节点：

```dts
&gpio5 {
    compatible = "zxr-my_gpio_interrupt_adapter";
    gpio-ranges = <&iomuxc_snvs 0 0 10>;
};

&gpio4 {
    compatible = "zxr-my_gpio_interrupt_adapter";
    gpio-ranges = <&iomuxc 0 94 17>, <&iomuxc 17 117 12>;
};

&reg_gpio_dvfs {
    status = "disabled";
};
```

## 编译与部署

```bash

# 加载控制器驱动（GPIO4 和 GPIO5 各触发一次 probe）
insmod gpio_interrupt_adapter.ko

# 加载测试驱动（验证中断功能）
insmod gpio_irq_test_driver.ko
```

设备树需重新编译并更新到板子：

```bash
make dtbs
cp arch/arm/boot/dts/imx6ull-my14x14-emmc.dtb /boot/
```

## 验证结果

**GPIO 功能验证（sysfs + 板载 LED，GPIO5_IO03）：**

```bash
cat /sys/kernel/debug/gpio          # 确认 zxr-my_gpio 控制器注册，GPIO5 base=448
echo 451 > /sys/class/gpio/export   # 448 + 3 = 451
echo out > /sys/class/gpio/gpio451/direction
echo 0 > /sys/class/gpio/gpio451/value   # LED 亮（低电平有效）
echo 1 > /sys/class/gpio/gpio451/value   # LED 灭
```

**中断功能验证（SR501 人体红外模块，GPIO4_IO19）：**

```bash
insmod gpio_irq_test_driver.ko
# dmesg: "GPIO interrupt test driver probed, irq=194"

cat /proc/interrupts | grep gpio-irq-test
# 194:  0  zxr-gpio-irq  19 Edge  gpio-irq-test

# SR501 触发后：
# dmesg: "GPIO interrupt triggered! count=1" ... "count=2" ... "count=3"
# /proc/interrupts 计数增加到 3
```

**调试验证：** 使用 Ftrace trace_event 追踪中断 handler 执行时间，使用 function_graph 从 chained handler 入口追踪完整函数调用链（ISR 读取 → irq_find_mapping → handle_edge_irq → irq_ack → 设备 handler → EOI），驱动本身处理耗时约 127μs。




