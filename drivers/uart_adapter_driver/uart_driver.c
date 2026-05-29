#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/rational.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/proc_fs.h>

#include <asm/irq.h>

static struct uart_driver my_uart_drv;
static struct my_uart_data *g_mydata;
static void my_stop_tx(struct uart_port *port);

struct my_uart_data {
    struct uart_port    port;       /* uart_port结构体，包含硬件端口的所有信息 */
    struct clk          *clk;       /* 时钟，probe里获取，shutdown/remove时可能要关 */
};

/* IMX6ULL UART寄存器偏移量（相对于基地址）*/
#define URXD  0x00  /* 接收数据寄存器 */
#define UTXD  0x40  /* 发送数据寄存器 */
#define UCR1  0x80  /* 控制寄存器1 */
#define UCR2  0x84  /* 控制寄存器2 */
#define UCR3  0x88  /* 控制寄存器3 */
#define UCR4  0x8C  /* 控制寄存器4 */
#define UFCR  0x90  /* FIFO控制寄存器 */
#define USR1  0x94  /* 状态寄存器1 */
#define USR2  0x98  /* 状态寄存器2 */
#define UESC  0x9C  /* 转义字符寄存器 */
#define UTIM  0xA0  /* 定时器寄存器 */
#define UBIR  0xA4  /* 波特率增量寄存器 */
#define UBMR  0xA8  /* 波特率调制寄存器 */
#define UTS   0xB4  /* 测试寄存器 */

/* URXD - 接收数据寄存器 bit */
#define URXD_RX_DATA    0xFF    /* 低8位是接收到的数据 */
#define URXD_CHARRDY    (1<<15) /* 数据有效标志 */
#define URXD_ERR        (1<<14) /* 有错误发生 */
#define URXD_OVRRUN     (1<<13) /* 溢出错误 */
#define URXD_FRMERR     (1<<12) /* 帧错误 */
#define URXD_BRK        (1<<11) /* Break检测到 */
#define URXD_PRERR      (1<<10) /* 校验错误 */

/* UCR1 - 控制寄存器1 bit */
#define UCR1_UARTEN     (1<<0)  /* UART模块总使能 */
#define UCR1_RRDYEN     (1<<9)  /* RX就绪中断使能 */
#define UCR1_TRDYEN     (1<<13) /* TX就绪中断使能 */
#define UCR1_TXMPTYEN   (1<<6)  /* TX完全空中断使能（console可能用到）*/

/* UCR2 - 控制寄存器2 bit */
#define UCR2_SRST       (1<<0)  /* 软复位：0=复位中，1=复位结束/正常工作 */
#define UCR2_RXEN       (1<<1)  /* 接收器使能 */
#define UCR2_TXEN       (1<<2)  /* 发送器使能 */
#define UCR2_WS         (1<<5)  /* 字长：0=7bit，1=8bit */
#define UCR2_STPB       (1<<6)  /* 停止位：0=1个，1=2个 */
#define UCR2_PROE       (1<<7)  /* 校验类型：0=偶校验，1=奇校验 */
#define UCR2_PREN       (1<<8)  /* 校验使能：0=无校验，1=有校验 */
#define UCR2_IRTS       (1<<14) /* 忽略RTS引脚（我们不用硬件流控，设为1）*/

/* UCR3 - 控制寄存器3 bit */
#define UCR3_RXDMUXSEL  (1<<2)  /* RXD复用选择，IMX6ULL必须设为1 */

/* UFCR - FIFO控制寄存器 bit */
#define UFCR_RXTL_SHIFT 0       /* RX触发水位，bit[5:0] */
#define UFCR_RFDIV_SHIFT 7      /* 参考时钟分频，bit[9:7] */
#define UFCR_RFDIV_MASK (7<<7)
#define UFCR_TXTL_SHIFT 10      /* TX触发水位，bit[15:10] */

/* USR1 - 状态寄存器1 bit */
#define USR1_RRDY       (1<<9)  /* RX FIFO数据就绪（达到触发水位）*/
#define USR1_TRDY       (1<<13) /* TX FIFO有空位（低于触发水位）*/

/* USR2 - 状态寄存器2 bit */
#define USR2_RDR        (1<<0)  /* RX FIFO非空（至少有1字节）*/
#define USR2_TXDC       (1<<3)  /* 发送彻底完成（FIFO空+移位寄存器空）*/
#define USR2_ORE        (1<<1)  /* 溢出错误 */

/* UTS - 测试寄存器 bit */
#define UTS_TXFULL      (1<<4)  /* TX FIFO满 */
#define UTS_RXEMPTY     (1<<5)  /* RX FIFO空 */
#define UTS_TXEMPTY     (1<<6)  /* TX FIFO空 */
#define UTS_LOOP        (1<<12) /* 回环模式 */


static void my_uart_rx_chars(struct uart_port *port)
{	
	struct tty_port *tport = &port->state->port;
    unsigned int ch, flag;

   while (!(readl(port->membase + UTS) & UTS_RXEMPTY)){

	    ch = readl(port->membase + URXD);  // 读数据
        flag = TTY_NORMAL;

        port->icount.rx++;

        if (ch & (URXD_OVRRUN | URXD_FRMERR | URXD_PRERR | URXD_BRK)) {
            if (ch & URXD_OVRRUN) {
				port->icount.overrun++;
				flag = TTY_OVERRUN;
				}
            if (ch & URXD_FRMERR){
				port->icount.frame++;
				flag = TTY_FRAME;
				}
            if (ch & URXD_PRERR){
				port->icount.parity++;
            	flag = TTY_PARITY;
				}
			if (ch & URXD_BRK){
				port->icount.brk++;
				flag = TTY_BREAK;
			}
   }
	
		tty_insert_flip_char(tport, ch & 0xFF, flag);	

}

}

static void my_uart_tx_chars(struct uart_port *port)
{
    struct circ_buf *xmit = &port->state->xmit;

    if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
        my_stop_tx(port);
        return;
    }

    while (!(readl(port->membase + UTS) & UTS_TXFULL)) {
        unsigned char ch = xmit->buf[xmit->tail];
        writel(ch, port->membase + UTXD);

        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
        port->icount.tx++;

        if (uart_circ_empty(xmit))
            break;
    }

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
        uart_write_wakeup(port);

    if (uart_circ_empty(xmit))
        my_stop_tx(port);
}

static irqreturn_t my_uart_irq(int irq, void *dev_id)
{
    struct uart_port *port = dev_id;
    struct tty_port *tport = &port->state->port;
    unsigned int usr1;
    unsigned long flags;

    spin_lock_irqsave(&port->lock, flags);

    usr1 = readl(port->membase + USR1);

    if (usr1 & USR1_RRDY)
        my_uart_rx_chars(port);  /* 里面只做tty_insert_flip_char，不做push */

    if (usr1 & USR1_TRDY)
        my_uart_tx_chars(port);

    spin_unlock_irqrestore(&port->lock, flags);

    /* push放在锁外面 */
    if (usr1 & USR1_RRDY)
        tty_flip_buffer_push(tport);

    return IRQ_HANDLED;
}


static void my_uart_console_write(struct console *co, const char *s, unsigned int count)
{	
	struct my_uart_data *data = g_mydata;
	struct uart_port *port = &data->port;
	int i;

	for(i=0;i<count;i++){
		while (readl(port->membase + UTS) & UTS_TXFULL)
        	barrier();  
		writel(s[i], port->membase + UTXD);

		if(s[i]=='\n'){
			while (readl(port->membase + UTS) & UTS_TXFULL)
				barrier();  
			writel('\r', port->membase + UTXD);
		}
	}
}

struct tty_driver *my_uart_console_device(struct console *co, int *index)
{
	struct uart_driver *p = co->data;
	*index = co->index;

	return p->tty_driver;
}

static int __init
my_console_setup(struct console *co, char *options)
{
	struct my_uart_data *data = g_mydata;
	int baud, parity,bits, flow;

	if (!data)
        return 0;

	if(options){

		uart_parse_options(options, &baud, &parity, &bits, &flow);

	}else{
		baud = 115200;
        parity = 'n';
        bits = 8;
        flow = 0;
	}

	return uart_set_options(&data->port, co, baud, parity, bits, flow);
}

static struct console my_uart_console = {
	.name		= "ttyZXR",
	.write		= my_uart_console_write,
	.device		= my_uart_console_device,
	.setup		= my_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data       = &my_uart_drv,
};

static struct uart_driver my_uart_drv = {
	.owner          = THIS_MODULE,
	.driver_name    = "ZXR_MY_UART",
	.dev_name       = "ttyZXR",
	.major          = 0,
	.minor          = 0,
	.nr             = 1,
	.cons           = &my_uart_console,
};


static unsigned int my_tx_empty(struct uart_port *port)
{
	int usr2 = readl(port->membase + USR2);
	if(usr2 & USR2_TXDC){
		return TIOCSER_TEMT;
	}
	else
	return 0;
}


/*
 * interrupts disabled on entry
 */
static void my_start_tx(struct uart_port *port)
{	
	int ucr1;
	ucr1 = readl(port->membase + UCR1);
	ucr1 |= UCR1_TRDYEN;
	writel(ucr1, port->membase + UCR1);
	return;
}


static void
my_set_termios(struct uart_port *port, struct ktermios *termios,
		  struct ktermios *old)
{
	/*	数据格式配置:
		读取termios->c_cflag，配置UCR2：

		字长：
		if (c_cflag & CS8)     → UCR2设WS位（8bit）
		else if (c_cflag & CS7) → UCR2清WS位（7bit）

		停止位：
		if (c_cflag & CSTOPB)  → UCR2设STPB位（2个停止位）
		else                   → UCR2清STPB位（1个停止位）

		校验：
		if (c_cflag & PARENB) {        → UCR2设PREN位（使能校验）
			if (c_cflag & PARODD)      → UCR2设PROE位（奇校验）
			else                       → UCR2清PROE位（偶校验）
		} else {
			→ UCR2清PREN位（无校验）
  }*/

	tcflag_t  cflag=termios->c_cflag;
	int ucr2;
	unsigned int baud, ref_freq;

	ucr2 = readl(port->membase + UCR2);

	/* 字长 */
	switch (cflag & CSIZE) {
	case CS7:  ucr2 &= ~UCR2_WS; break;
	case CS8:
	default:   ucr2 |= UCR2_WS;  break;
	}

	/* 停止位 */
	if (cflag & CSTOPB)
		ucr2 |= UCR2_STPB;
	else
		ucr2 &= ~UCR2_STPB;

	/* 校验 */
	if (cflag & PARENB) {
		ucr2 |= UCR2_PREN;
		if (cflag & PARODD)
			ucr2 |= UCR2_PROE;
		else
			ucr2 &= ~UCR2_PROE;
	} else {
		ucr2 &= ~UCR2_PREN;
	}

	writel(ucr2, port->membase + UCR2);


	/*波特率配置:
	第一步：拿到目标波特率
 	 baud = uart_get_baud_rate(port, termios, old, 50, 4000000);
  	（这个内核函数从termios里提取波特率数值）

	第二步：算出参考时钟频率
  	ref_freq = port->uartclk;（这是probe里通过clk_get_rate拿到的）
  	再除以UFCR里RFDIV设的分频值
  	（如果RFDIV设成101b即1分频，那ref_freq就等于port->uartclk）

	第三步：设置UBIR和UBMR
 	 writel(baud * 16 - 1, port->membase + UBIR);
  	writel(ref_freq - 1, port->membase + UBMR);
  	（注意：必须先写UBIR再写UBMR，因为硬件在写UBMR时才真正更新波特率）

	第四步：调用uart_update_timeout计算并更新fifo超时时间
  	uart_update_timeout(port, c_cflag, baud);
	*/

	baud = uart_get_baud_rate(port, termios, old, 50, 4000000);
	ref_freq = (port->uartclk) / 1;

  	writel(baud * 16 - 1, port->membase + UBIR);
 	writel(ref_freq - 1, port->membase + UBMR);

	uart_update_timeout(port, cflag, baud);

   return;
}

static int my_startup(struct uart_port *port)
{
   	int ucr1;
	int i = 100;

	/*第一步：禁用UART（先关再配置，防止配置过程中产生异常）
  → UCR1：清除UARTEN位（bit0写0）*/

	ucr1 = readl(port->membase + UCR1);
	ucr1 &= ~UCR1_UARTEN;
	writel(ucr1, port->membase + UCR1);

	/*第二步：软复位
  → UCR2：清除SRST位（bit0写0），等待硬件复位完成
  → 然后等UCR2的SRST位自动变回1（硬件复位完成后会自动置1）*/

	int ucr2 = readl(port->membase + UCR2);
	ucr2 &= ~UCR2_SRST;
	writel(ucr2, port->membase + UCR2);

	while (!(readl(port->membase + UCR2) & UCR2_SRST) && (--i > 0))
		udelay(1);

	/*第三步：配置基本参数
  → UCR3：设置RXDMUXSEL位（bit2写1）——IMX6ULL硬性要求
  → UCR2：设置IRTS位（bit14写1）——忽略硬件流控
  → UCR2：设置WS位（bit5写1）——默认8bit数据位
  → UCR2：设置RXEN和TXEN（bit1和bit2写1）——使能收发
  → UFCR：设置RXTL=1（收到1字节就中断）、TXTL=2（FIFO空位>=2时中断）
  → UFCR：设置RFDIV（参考时钟分频因子，设为1分频=101b）*/

	int ucr3 = readl(port->membase + UCR3);
	ucr3 |= UCR3_RXDMUXSEL;
	writel(ucr3, port->membase + UCR3);

	ucr2 = readl(port->membase + UCR2);
	ucr2 |= UCR2_IRTS | UCR2_WS | UCR2_RXEN | UCR2_TXEN;
	writel(ucr2, port->membase + UCR2);

	int ufcr = readl(port->membase + UFCR);
	ufcr &= ~(0x3F<<UFCR_RXTL_SHIFT);
	ufcr |= (1<<UFCR_RXTL_SHIFT);

	ufcr &= ~(0x3F<<UFCR_TXTL_SHIFT);
	ufcr |= (2<<UFCR_TXTL_SHIFT);

	ufcr &= ~UFCR_RFDIV_MASK;
	ufcr |= (5<<UFCR_RFDIV_SHIFT);
	writel(ufcr, port->membase + UFCR);

	ucr1 = readl(port->membase + UCR1);
	ucr1 |= UCR1_UARTEN | UCR1_RRDYEN;
	writel(ucr1, port->membase + UCR1);

   return 0;
}

static void my_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	return;
}

static unsigned int my_get_mctrl(struct uart_port *port)
{
   return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void my_stop_tx(struct uart_port *port)
{
	int ucr1;
	ucr1 = readl(port->membase + UCR1);
	ucr1 &= ~UCR1_TRDYEN;
	writel(ucr1, port->membase + UCR1);
	return;
}

static void my_stop_rx(struct uart_port *port)
{
	int ucr1;
	ucr1 = readl(port->membase + UCR1);
	ucr1 &= ~UCR1_RRDYEN;
	writel(ucr1, port->membase + UCR1);
	return;
}

static void my_shutdown(struct uart_port *port)
{
	/*第一步：禁止所有中断
  → UCR1：清除RRDYEN和TRDYEN

	第二步：禁用收发
  → UCR2：清除RXEN和TXEN

	第三步：关闭UART
  → UCR1：清除UARTEN*/

	int ucr1 = readl(port->membase + UCR1);
	ucr1 &= ~(UCR1_RRDYEN | UCR1_TRDYEN);
	writel(ucr1, port->membase + UCR1);

	int ucr2 = readl(port->membase + UCR2);
	ucr2 &= ~(UCR2_RXEN | UCR2_TXEN);
	writel(ucr2, port->membase + UCR2);

	ucr1 = readl(port->membase + UCR1);
	ucr1 &= ~UCR1_UARTEN;
	writel(ucr1, port->membase + UCR1);

	return;
}

static void my_config_port(struct uart_port *port, int flags)
{
    if (flags & UART_CONFIG_TYPE)
        port->type = PORT_IMX;
}

static const char *my_type(struct uart_port *port)
{
   return "ZXR_my_UART";
}



static const struct uart_ops my_pops = {
	.tx_empty	= my_tx_empty,
	.set_mctrl	= my_set_mctrl,
	.get_mctrl	= my_get_mctrl,
	.stop_tx	= my_stop_tx,
	.start_tx	= my_start_tx,
	.stop_rx	= my_stop_rx,
//	.enable_ms	= imx_enable_ms,
//	.break_ctl	= imx_break_ctl,
	.startup	= my_startup,
	.shutdown	= my_shutdown,
//	.flush_buffer	= imx_flush_buffer,
	.set_termios	= my_set_termios,
	.type		= my_type,
	.config_port	= my_config_port,
//	.verify_port	= imx_verify_port,

};


static int my_uart_probe(struct platform_device *pdev)
{	
	struct my_uart_data *data;
	struct resource *res;
	void __iomem *base;
    unsigned int irq;
    int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
        return -ENOMEM;

	g_mydata = data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
        return -ENODEV;

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
        return PTR_ERR(base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
        return irq;

	data->clk = devm_clk_get(&pdev->dev, "ipg");
	if (IS_ERR(data->clk)) {
        dev_err(&pdev->dev, "failed to get uart clk\n");
        return PTR_ERR(data->clk);
    }
	ret = clk_prepare_enable(data->clk);
	if (ret)
        return ret;

	data->port.membase   = base;              /* ioremap后的虚拟基地址，函数里用这个读写寄存器 */
    data->port.mapbase   = res->start;        /* 物理基地址，内核内部记录用 */
    data->port.irq       = irq;               /* 中断号 */
    data->port.uartclk   = clk_get_rate(data->clk); /* 输入时钟频率，算波特率要用 */
    data->port.fifosize  = 32;                /* IMX6ULL UART的TX/RX FIFO都是32字节 */
    data->port.ops       = &my_pops;          /* 指向uart_ops */
    data->port.dev       = &pdev->dev;        /* 关联platform_device */
    data->port.type      = PORT_IMX;          /* 端口类型，用内核已定义的IMX类型 */
    data->port.iotype    = UPIO_MEM;          /* IO类型：内存映射（相对于端口IO） */
    data->port.flags     = UPF_BOOT_AUTOCONF; /* 启动时自动配置 */
    data->port.line      = 0;                 /* 端口索引号，只有一个端口所以是0 */

	platform_set_drvdata(pdev, data);
	
	ret = devm_request_irq(&pdev->dev, irq, my_uart_irq, IRQF_SHARED, "my_uart", &data->port);
	if (ret) {
		clk_disable_unprepare(data->clk);
		return ret;
	}

	ret = uart_add_one_port(&my_uart_drv, &data->port);
	if (ret) {
        clk_disable_unprepare(data->clk);
        return ret;
    }

    return 0;
}
static int my_uart_remove(struct platform_device *pdev)
{
	struct my_uart_data *data = platform_get_drvdata(pdev);

    uart_remove_one_port(&my_uart_drv, &data->port);
    clk_disable_unprepare(data->clk);
    return 0;
}

static const struct of_device_id my_uart_of_match[] = {
	{ .compatible = "zxr-my_uart", },
	{ },
};


static struct platform_driver my_uart_driver = {
	.probe		= my_uart_probe,
	.remove		= my_uart_remove,
	.driver		= {
		.name	= "zxr_my_uart",
		.of_match_table = of_match_ptr(my_uart_of_match),
	}
};



static int __init my_uart_init(void)
{	
    int ret = uart_register_driver(&my_uart_drv);
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);

	if (ret)
		return ret;

	ret = platform_driver_register(&my_uart_driver);
	if (ret != 0)
		uart_unregister_driver(&my_uart_drv);

	return ret;
}


static void __exit my_uart_exit(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
    platform_driver_unregister(&my_uart_driver);
	uart_unregister_driver(&my_uart_drv);
}



module_init(my_uart_init);
module_exit(my_uart_exit);
MODULE_LICENSE("GPL");