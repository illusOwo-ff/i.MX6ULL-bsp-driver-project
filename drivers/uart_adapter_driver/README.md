# UART 控制器驱动开发：基于 IMX6ULL

## 概述

本部分基于 Linux serial/tty 子系统框架，为 i.MX6ULL 开发了完整的 UART 控制器驱动，通过操作 IMX6ULL 真实的 UART 寄存器（如 URXD, UTXD, UCR 等）完成了核心控制器驱动的开发，测试与调试验证功能正常。

## 驱动核心特性

* **完整的中断收发**：实现了基于 RX FIFO 就绪中断和 TX FIFO 空中断的数据传输机制。
* **动态参数配置**：实现了 `set_termios` 回调，支持根据用户态请求动态配置波特率、数据位、停止位和校验模式。
* **硬件生命周期管理**：实现了 `startup` 和 `shutdown`，规范了底层硬件模块的开启与资源释放流程。
* **Console 支持**：实现了底层轮询发送机制（非中断环境），支持内核 `printk` 打印输出。

## 文件说明

当前 UART 驱动及相关配置位于 `drivers/` 目录下：

```text
drivers/
├── dtb/
│   ├── imx6ull-my14x14-emmc.dts    # 修改后的板级设备树源码（配置引脚复用与节点）
│   └── imx6ull-my14x14-emmc.dtb    # 编译生成的设备树二进制文件
└── uart_adapter_driver/
    └── uart_driver.c               # UART 控制器核心驱动源码

```

## 设备树配置要点

为避免与官方默认驱动冲突，并正确复用引脚，对设备树做了如下调整：

1. **资源释放**：关闭了原生 `csi` 和 `ov5640` 节点，释放 `CSI_MCLK` 和 `CSI_PIXCLK` 引脚。
2. **引脚复用**：在 `&iomuxc` 根级新增 `pinctrl_my_uart6` 节点，将上述引脚复用为 `UART6_DCE_TX` 和 `UART6_DCE_RX`。
3. **驱动匹配**：覆盖 `uart6` 节点，使用自定义的 `compatible = "zxr-my_uart"` 以匹配本驱动，并挂载 pinctrl。

## 部署与编译

### 1. 编译设备树

将 `dtb/imx6ull-my14x14-emmc.dts` 替换到内核源码的 `arch/arm/boot/dts/` 目录下，然后进入内核根目录编译：

```bash
make dtbs

```

将生成的 `imx6ull-my14x14-emmc.dtb` 更新到开发板。
### 2. 编译驱动

在包含 `uart_driver.c` 的目录下创建 `Makefile` 并执行交叉编译（确保指向自己的内核源码路径）：

```bash
make 

```

## 测试与验证流程

将编译好的 `uart_driver.ko` 拷贝到开发板，并准备杜邦线。

### 1. 硬件连接

在开发板转接板（J6）上，使用杜邦线短接 UART6 的 TX 和 RX 进行回环测试：

* **UART6_TX** (pin 4) 短接 **UART6_RX** (pin 5)。

### 2. 加载驱动与状态检查

```bash
insmod uart_driver.ko
# 检查设备节点是否成功生成
ls /dev/ttyZXR0
# 查看驱动的收发初始状态
cat /proc/tty/driver/ZXR_MY_UART
# 查看中断注册情况
cat /proc/interrupts | grep my_uart

```

### 3. 数据收发回环测试

配置波特率并进行通信测试：

```bash
# 配置波特率为 115200 及原始模式
stty -F /dev/ttyZXR0 115200 raw -echo

# 开启后台进程监听接收端
cat /dev/ttyZXR0 &

# 向发送端写入数据
echo "hello" > /dev/ttyZXR0

```

**预期结果**：终端会立即打印出 `"hello"`，说明数据从 TX 寄存器发出，经短接线回到 RX 并成功触发中断读取。再次执行 `cat /proc/tty/driver/ZXR_MY_UART` 可看到 tx 与 rx 的字节数同步增加。
