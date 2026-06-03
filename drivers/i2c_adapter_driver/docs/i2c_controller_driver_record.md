[TOC]



## 一、I2C子系统框架

### 框架三层结构

```
I2C 核心层（内核已实现）
    ↑ 提供 i2c_add_adapter / i2c_transfer 等API
I2C 适配器驱动（本项目要写的）
    ↑ 实现 master_xfer，操作硬件寄存器完成传输
I2C 设备驱动（如传感器驱动、EEPROM驱动）
```

APP通过I2C Controller与I2C Device传输数据。（APP通过i2c_adapter与i2c_client传输i2c_msg）

### 通信特点

I2C是同步串行总线，使用两根线：SDA（数据）和SCL（时钟）。主机产生SCL时钟，决定通信速度。

两根线都是开漏结构，外接上拉电阻。这决定了ACK/NACK的物理实现：拉低SDA=ACK，不拉SDA（上拉电阻自然拉高）=NACK。因此I2SR寄存器的RXAK位"0=ACK，1=NACK"。

![img](https://i-blog.csdnimg.cn/direct/0ef5e57617604676a9b2f99ddba3db85.png)![点击并拖拽以移动](data:image/gif;base64,R0lGODlhAQABAPABAP///wAAACH5BAEKAAAALAAAAAABAAEAAAICRAEAOw==)编辑

### 字节传输机制

I2C所有传输都是按字节进行的：

- 8个SCL时钟：传输8个数据bit（高位先发）
- 1个SCL时钟回应：ACK/NACK（接收方控制SDA）
- 共9个SCL时钟 = 传输一个完整字节![img](https://i-blog.csdnimg.cn/direct/7e183a81279b49fa9e9c3319cd8b8e18.png)![点击并拖拽以移动](data:image/gif;base64,R0lGODlhAQABAPABAP///wAAACH5BAEKAAAALAAAAAABAAEAAAICRAEAOw==)编辑

### START/STOP/Repeated START

- START：SCL高电平时SDA从高拉低。控制寄存器I2CR的MSTA位写1自动产生。
- STOP：SCL高电平时SDA从低变高。I2CR的MSTA位写0自动产生。
- Repeated START：不经过STOP直接发新START，保持总线占用。I2CR的RSTA位写1产生。

START和STOP不是字节传输，不产生中断。

### ACK/NACK的两个方向

- **主机写时**：从机决定发ACK/NACK，硬件自动采样存到I2SR的RXAK位。驱动检查RXAK判断从机是否应答。
- **主机读时**：主机决定发ACK/NACK，驱动通过I2CR的TXAK位控制（0=ACK，1=NACK）。最后一个字节发NACK告诉从机"别再发了"。

### Dummy Read

i.MX6ULL的I2C控制器设计：**读I2DR同时做两件事**——取出已接收的数据 + 触发硬件开始接收下一个字节。

切换到接收模式后，硬件不会自动开始接收，需要读一次I2DR作为"开始信号"。但此时I2DR里是之前残留的值（垃圾），所以第一次读到的数据要丢弃——这就是dummy read。

### 时钟系统

```
IPG时钟（66MHz）→ IFDR寄存器分频（查表，64种离散值）→ SCL时钟（100kHz/400kHz）
```

I2C分频是查预定义表选最接近的离散值。

### 关键结构体

#### 自定义的私有数据结构体

| 成员      | 类型                 | 作用                                                         |
| --------- | -------------------- | ------------------------------------------------------------ |
| `adapter` | `struct i2c_adapter` | I2C适配器，嵌在私有结构体里，通过container_of反推            |
| `base`    | `void __iomem *`     | 寄存器基地址（ioremap后的虚拟地址）。i2c_adapter本身没有base成员（不像uart_port有membase），所以必须在私有结构体中保存 |
| `clk`     | `struct clk *`       | 控制器时钟，probe中获取并使能                                |
| `irq`     | `int`                | 中断号                                                       |
| `queue`   | `wait_queue_head_t`  | 等待队列，master_xfer和ISR之间的同步机制                     |
| `i2csr`   | `unsigned long`      | 缓存的I2SR状态寄存器值——ISR中读出后存到这里，master_xfer中检查 |

ISR为了及时清除中断标志会将硬件I2SR清零，但master_xfer需要检查传输结果（如RXAK位判断ACK/NACK）。ISR在清零前先把I2SR的值存到`i2csr`，相当于"清除前的状态记录"。

#### i2c_adapter结构体

| 成员          | 内容                | 说明                                               |
| ------------- | ------------------- | -------------------------------------------------- |
| `owner`       | `THIS_MODULE`       | 固定                                               |
| `algo`        | `&i2c_bus_my_algo`  | 指向算法结构体                                     |
| `dev.parent`  | `&pdev->dev`        | 父设备                                             |
| `name`        | `"zxr-i2c"`         | 适配器名字，i2cdetect -l 显示的就是这个            |
| `dev.of_node` | `pdev->dev.of_node` | 关键：内核靠这个自动扫描设备树子节点创建i2c_client |

注册函数：`i2c_add_adapter()`（动态分配编号）或`i2c_add_numbered_adapter()`。动态分配时，I2C核心层内部会通过`of_alias_get_id()`从设备树aliases节点获取正确编号。

#### i2c_algorithm结构体

| 成员            | 是否实现 | 说明                                                         |
| --------------- | -------- | ------------------------------------------------------------ |
| `master_xfer`   | **必须** | 核心传输函数                                                 |
| `smbus_xfer`    | 不需要   | 不实现时核心层自动用master_xfer模拟SMBus传输                 |
| `functionality` | **必须** | 返回控制器支持的功能flags，比如返回I2C_FUNC_I2C \|I2C_FUNC_SMBUS_EMUL |

## 二、NXP官方手册

### 硬件结构（取自NXP官方芯片手册1447页）

![img](https://i-blog.csdnimg.cn/direct/5276854e564846a0817fa2ef6c31778b.png)![点击并拖拽以移动](data:image/gif;base64,R0lGODlhAQABAPABAP///wAAACH5BAEKAAAALAAAAAABAAEAAAICRAEAOw==)编辑

### 寄存器列表

I2C控制器只有5个寄存器，访问宽度为8位（用readb/writeb）。

### I2CR控制寄存器bit定义

| Bit  | 名字 | 说明                              | 使用                     |
| ---- | ---- | --------------------------------- | ------------------------ |
| 7    | IEN  | 模块使能                          | xfer开头置1，结尾清0     |
| 6    | IIEN | 中断使能                          | start中置1，stop中清0    |
| 5    | MSTA | 主从模式（写1=START，写0=STOP）   | start中置1，stop中清0    |
| 4    | MTX  | 发送/接收（1=发，0=收）           | 发送时置1，read开头清0   |
| 3    | TXAK | ACK控制（0=ACK，1=NACK）          | read中控制最后字节发NACK |
| 2    | RSTA | Repeated START，写1产生，自动清零 | xfer中非最后msg之间使用  |

### I2SR状态寄存器bit定义

| Bit  | 名字 | 说明                       | 使用                                     |
| ---- | ---- | -------------------------- | ---------------------------------------- |
| 5    | IBB  | 总线忙                     | bus_busy函数轮询等待                     |
| 4    | IAL  | 仲裁丢失（写0清除）        | 可在xfer中检查                           |
| 1    | IIF  | 中断标志（写0清除）        | ISR中判断和清除                          |
| 0    | RXAK | 收到的ACK（0=ACK，1=NACK） | start发完地址后检查，write发完每字节检查 |

### 



------



## 三、函数实现逻辑

### 注册流程

使用平台总线驱动模型：

- init函数中：`platform_driver_register()`注册platform_driver

- probe函数中：获取资源 → 初始化同步机制 → 配置硬件 → 注册adapter

  

### ISR（中断处理函数）

```
读I2SR：temp = readb(base + I2SR)

如果 temp & IIF（中断标志置位）：
    清除硬件IIF：writeb(0, base + I2SR)    ← 必须在ISR里清，因为中断是电平触发
    存状态快照：i2csr = temp               ← 清除前的值，包含RXAK等信息
    唤醒等待进程：wake_up(&queue)
    return IRQ_HANDLED

return IRQ_NONE
```

**IIF必须在ISR里清除**：i.MX6ULL的I2C中断是电平触发的，IIF=1时中断线一直有效。如果ISR不清IIF就返回，硬件会立刻再次触发中断，造成中断风暴。

### trx_complete （等待传输完成）

```
清除上次残留状态：i2csr = 0              ← 让wait_event条件为假
等待中断唤醒：wait_event_timeout(queue, i2csr & IIF, 500ms)
如果超时 → return -ETIMEDOUT
return 0
```

这个函数被start、write、read共用，流程是清i2csr → 等中断 → ISR存新的i2csr并唤醒 → 返回后检查i2csr中的RXAK等状态。

### start（发送START + 从机地址）

设置控制寄存器产生START

等IBB=1确认START成功

发送从机地址 （7位地址左移1位 | 读写位）

等待地址传输完成 →检查 i2csr & RXAK → NACK(=1)说明从机不存在，返回-ENXIO

### write（按字节写数据）

```
循环 msg->len 个字节：
{
    写数据到I2DR：writeb(msg->buf[i], base + I2DR)
      → 硬件产生9个SCL时钟，发送数据，从机回ACK/NACK

    等待传输完成：trx_complete()

    检查 i2csr & RXAK：
      RXAK=1(NACK) → return -EIO（传输失败）
      RXAK=0(ACK) → 继续下一字节
}
return 0
```

### read（按字节读数据）

**注意最后一个字节必须先STOP再读I2DR**，因为读I2DR会触发硬件开始接收下一个字节。如果先读再STOP，硬件已经开始产生不该有的SCL时钟了。先STOP让硬件停止，再读I2DR只是取数据不会触发接收。

```
第一步：切换到接收模式
  → 读I2CR，清MTX位（接收模式）
  → 如果只读1个字节：设TXAK=1（收完发NACK）
  → 否则：清TXAK=0（收完发ACK）
  → 写回I2CR

第二步：dummy read触发接收
  → i2csr = 0（清start残留状态）
  → readb(base + I2DR)    ← 丢弃，但触发第1个字节的接收

第三步：循环读取每个字节
for (i = 0; i < msg->len; i++)
{
    等待传输完成：trx_complete()
      → 1个字节接收完成，硬件已自动发了ACK或NACK

    如果是倒数第2个字节(i == len-2)：
      → I2CR置TXAK=1（下一个字节要发NACK）

    如果是最后一个字节(i == len-1)：
      如果is_lastmsg（整个传输的最后一个msg）：
        → I2CR清MSTA → 产生STOP（必须在读I2DR之前）
      否则（后面还有msg）：
        → I2CR置MTX → 切回发送模式，为Repeated START准备

    读数据：msg->buf[i] = readb(base + I2DR)
      → 取出数据 + 触发下一个字节接收（最后字节除外，因为已STOP）
}
return 0
```

### master_xfer（调度函数）

```
使能模块 → 等总线空闲 → 发START+从机地址 → 遍历每个i2c_msg（写msg调write按字节发送，读msg调read按字节接收，msg之间用Repeated START）→ 发STOP → 禁用模块。每个字节传输后通过中断+等待队列同步。
```

### functionality

```
return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA
```

I2C_FUNC_SMBUS_EMUL：表示支持SMBus模拟（因为有master_xfer，核心层能用它模拟SMBus）。

------

## 四、并发保护

I2C控制器驱动本身不需要自己加锁。I2C核心层在调用master_xfer之前已经对adapter加了bus_lock（mutex），保证同一时间只有一个线程在操作同一个控制器。这跟SPI类似——总线子系统的框架设计就是"核心层管并发，驱动层只管硬件操作"。

master_xfer和ISR之间的同步通过等待队列实现：master_xfer的wait_event_timeout等待，ISR的wake_up唤醒。i2csr作为共享变量由ISR写、master_xfer读，由于ISR中先写i2csr再wake_up，master_xfer醒来后读到的一定是最新值，不需要额外加锁。

------

## 五、设备树配置

### 官方I2C1节点（imx6ull.dtsi）

```
i2c1: i2c@021a0000 {
    #address-cells = <1>;        /* 子节点reg用1个cell表示I2C从机地址 */
    #size-cells = <0>;           /* 子节点没有地址范围 */
    compatible = "fsl,imx6ul-i2c", "fsl,imx21-i2c";
    reg = <0x021a0000 0x4000>;   /* 控制器寄存器物理地址和范围 */
    interrupts = <GIC_SPI 36 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&clks IMX6UL_CLK_I2C1>;
    status = "disabled";
};
```

### EVK覆盖（imx6ull-14x14-evk.dts）

```
&i2c1 {
    clock-frequency = <100000>;   /* 目标SCL频率100kHz */
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_i2c1>;  /* UART4_TX→I2C1_SCL, UART4_RX→I2C1_SDA */
    status = "okay";
    mag3110@0e { ... };           /* EVK板上的传感器 */
    fxls8471@1e { ... };
};
```

### 引脚选择

开发板转接板J7引出了I2C1_SDA和I2C1_SCL，对应的pinctrl已在EVK的dts中定义（UART4_TX/RX引脚复用为I2C1功能），直接复用无需重新定义。

### 我的板级覆盖（imx6ull-my14x14-emmc.dts）

![img](https://i-blog.csdnimg.cn/direct/b85351b87844439ba8369a0b974cdb51.png)![点击并拖拽以移动](data:image/gif;base64,R0lGODlhAQABAPABAP///wAAACH5BAEKAAAALAAAAAABAAEAAAICRAEAOw==)编辑

其他所有属性（reg、interrupts、clocks、clock-frequency、pinctrl、status="okay"）从dtsi和EVK继承，不需要重复写。

------

## 六、测试验证

### 加载检查

```
编辑
```

### 功能测试

测试使用板载AP3216C红外光强距离传感器（I2C地址0x1e）和i2c-tools。i2c-tools通过i2c-dev.c（`/dev/i2c-X`）直接操作I2C总线。

```
# 扫描总线
i2cdetect -y 0
# 预期：0x1e位置显示"1e"，其他显示"--"
# 验证：master_xfer → start → 发地址 → 检查ACK 链路正常

# 写入配置（软复位）
i2cset -f -y 0 0x1e 0 0x4
# 验证：write函数能正确发送多字节数据

# 写入配置（开启ALS+PS模式）
i2cset -f -y 0 0x1e 0 0x3
# 验证：写入后传感器开始采集数据

# 读取光强数据（word模式，2字节）
i2cget -f -y 0 0x1e 0x0c w
# 预期：返回光强数值，遮挡/曝光时变化
# 验证：Repeated START + 多字节read + dummy read + TXAK切换 链路正常

# 读取距离数据
i2cget -f -y 0 0x1e 0x0e w
# 预期：返回距离数值，靠近/远离时变化
# 验证：读取实时变化的传感器数据，证明每次都是真实I2C传输

# 中断计数验证
cat /proc/interrupts | grep my_i2c
# 预期：计数显著增加（每个字节传输触发一次中断）
```

### ![img](https://i-blog.csdnimg.cn/direct/17d05e769feb4ba58592d9b2cb8e63ba.png)![点击并拖拽以移动](data:image/gif;base64,R0lGODlhAQABAPABAP///wAAACH5BAEKAAAALAAAAAABAAEAAAICRAEAOw==)编辑

### 测试结果

- i2cdetect成功检测到0x1e设备 → START + 地址 + ACK检测链路通过
- i2cset写入配置寄存器成功，传感器模式切换 → write链路通过
- i2cget读到实时变化的传感器数据 → read链路通过（包括Repeated START、dummy read、多字节读）
- /proc/interrupts中断计数持续增长（1148次） → 中断机制正常工作

------

## 七、调试分析——strace追踪I2C系统调用链

### I2C ioctl命令

| strace编码        | ioctl命令                | 含义                       |
| ----------------- | ------------------------ | -------------------------- |
| `_IOC(0x7, 0x3)`  | I2C_SLAVE (0x0703)       | 设置从机地址               |
| `_IOC(0x7, 0x5)`  | I2C_FUNCS (0x0705)       | 查询适配器功能             |
| `_IOC(0x7, 0x6)`  | I2C_SLAVE_FORCE (0x0706) | 强制设置从机地址（-f参数） |
| `_IOC(0x7, 0x20)` | I2C_SMBUS (0x0720)       | 执行SMBus事务              |

### i2cget调用链（读操作）

```
strace -o i2c_trace.log i2cget -f -y 0 0x1e 0x0c w
```

核心系统调用：

```
open("/dev/i2c-0", O_RDWR)         → 打开I2C适配器设备节点
ioctl(3, I2C_FUNCS, ...)           → 查适配器能力（调functionality回调）
ioctl(3, I2C_SLAVE_FORCE, 0x1e)    → 设从机地址
ioctl(3, I2C_SMBUS, ...)           → 执行SMBus读word事务
close(3)                           → 关闭
```

I2C_SMBUS在内核中的路径：

```
ioctl(I2C_SMBUS)
  → i2c-dev.c: i2cdev_ioctl_smbus()
    → I2C核心: i2c_smbus_xfer()
      → 未实现smbus_xfer → i2c_smbus_xfer_emulated()
        → 构造2个i2c_msg: msg[0]=写寄存器地址, msg[1]=读2字节
          → master_xfer → start → write → Repeated START → start → read → stop
```

### i2cset调用链（写操作）

```
open("/dev/i2c-0", O_RDWR)         → 打开设备节点
ioctl(3, I2C_FUNCS, ...)           → 查能力
ioctl(3, I2C_SLAVE_FORCE, 0x1e)    → 设从机地址
ioctl(3, I2C_SMBUS, ...)           → 执行SMBus写字节事务
close(3)                           → 关闭
```

### i2cdetect调用链（扫描）

```
open("/dev/i2c-0", O_RDWR)         → 打开设备节点
ioctl(3, I2C_FUNCS, ...)           → 查能力
循环0x03~0x77每个地址：
  ioctl(3, I2C_SLAVE, addr)        → 设地址
  ioctl(3, I2C_SMBUS, ...)         → 尝试通信
    0x1e返回0（设备存在），其他返回-ENXIO（无设备）
close(3)                           → 关闭
```

------

## 八、遇到的问题与解决

I2C部分比较简单，遇到的问题不多，主要是函数编写的时候逻辑上有问题，以下是问题记录

### 问题1：RXAK检查读到的始终是0

**现象**：write函数中发完数据后检查从机ACK，即使从机不存在（应该NACK），也检测不到。

**原因**：trx_complete中`writeb(0, I2SR)`清除了整个I2SR（包括RXAK位）。清除后再读硬件I2SR，RXAK已经是0了。

**我的解决**：将IIF清除移到ISR中（ISR读I2SR后立即清除），检查RXAK时使用ISR存的`i2csr`状态而非直接读硬件寄存器。这也解决了电平触发中断反复调用ISR的问题。

### 问题2：read函数进入循环后不等待直接返回

**现象**：read函数进入循环后，trx_complete立即返回不等待，读到的数据错误。

**原因**：从start返回时，`i2csr`里还残留着地址字节传输的状态（IIF=1）。read进入循环调trx_complete，wait_event_timeout检查`i2csr & IIF`仍然为真，直接返回不睡眠。

**我的解决**：在trx_complete开头加`my_i2c->i2csr = 0`统一清除上次残留状态，确保wait_event_timeout条件初始为假。
