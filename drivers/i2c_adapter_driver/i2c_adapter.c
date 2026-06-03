#include "asm-generic/errno.h"
#include "asm/delay.h"
#include "asm/io.h"
#include "linux/device.h"
#include "linux/export.h"
#include "linux/irqreturn.h"
#include "linux/jiffies.h"
#include "linux/kernel.h"
#include "linux/wait.h"
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
//#include <linux/i2c-algo-bit.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
//#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* 寄存器偏移 */
#define IADR    0x00    /* I2C Address Register —— 本机做从机时的地址*/
#define IFDR    0x04    /* I2C Frequency Divider Register —— 时钟分频 */
#define I2CR    0x08    /* I2C Control Register —— 控制寄存器 */
#define I2SR    0x0C    /* I2C Status Register —— 状态寄存器 */
#define I2DR    0x10    /* I2C Data Register —— 数据寄存器（读写共用） */

/* I2CR 控制寄存器 bits */
#define I2CR_IEN     BIT(7)   /* I2C 使能 */
#define I2CR_IIEN    BIT(6)   /* 中断使能 */
#define I2CR_MSTA    BIT(5)   /* 主机模式（置1产生START，清0产生STOP） */
#define I2CR_MTX     BIT(4)   /* 发送模式（1=发送，0=接收） */
#define I2CR_TXAK    BIT(3)   /* 发送ACK控制（1=发NACK，0=发ACK） */
#define I2CR_RSTA    BIT(2)   /* Repeated START */

/* I2SR 状态寄存器 bits */
#define I2SR_ICF     BIT(7)   /* 传输完成标志 */
#define I2SR_IBB     BIT(5)   /* 总线忙标志 */
#define I2SR_IAL     BIT(4)   /* 仲裁丢失标志 */
#define I2SR_IIF     BIT(1)   /* 中断标志（传输完一个字节置1） */
#define I2SR_RXAK    BIT(0)   /* 收到的ACK值（0=ACK，1=NACK） */

/* { 分频系数, IFDR寄存器值 } */
static const u16 i2c_clk_div[][2] = {
    { 22,  0x20 }, { 24,  0x21 }, { 26,  0x22 }, { 28,  0x23 },
    { 30,  0x00 }, { 32,  0x24 }, { 36,  0x25 }, { 40,  0x26 },
    { 42,  0x03 }, { 44,  0x27 }, { 48,  0x28 }, { 52,  0x05 },
    { 56,  0x29 }, { 60,  0x06 }, { 64,  0x2A }, { 72,  0x2B },
    { 80,  0x2C }, { 88,  0x09 }, { 96,  0x2D }, { 104, 0x0A },
    { 112, 0x2E }, { 128, 0x2F }, { 144, 0x0C }, { 160, 0x30 },
    { 192, 0x31 }, { 224, 0x32 }, { 240, 0x0F }, { 256, 0x33 },
    { 288, 0x10 }, { 320, 0x34 }, { 384, 0x35 }, { 448, 0x36 },
    { 480, 0x13 }, { 512, 0x37 }, { 576, 0x14 }, { 640, 0x38 },
    { 768, 0x39 }, { 896, 0x3A }, { 960, 0x17 }, { 1024, 0x3B },
    { 1152, 0x18 }, { 1280, 0x3C }, { 1536, 0x3D }, { 1792, 0x3E },
    { 1920, 0x1B }, { 2048, 0x3F }, { 2304, 0x1C }, { 2560, 0x1D },
    { 3072, 0x1E }, { 3840, 0x1F }
};

struct my_i2c_struct {
	struct i2c_adapter	adapter;
	struct clk		*clk;
	void __iomem		*base;
	wait_queue_head_t	queue;
	unsigned long		i2csr;
	int	irq;
};

static int i2c_my_trx_compelete(struct my_i2c_struct *my_i2c)
{	
	my_i2c->i2csr = 0;
	long ret = wait_event_timeout(my_i2c->queue, my_i2c->i2csr & I2SR_IIF, msecs_to_jiffies(500));
	if(ret == 0) return -ETIMEDOUT;

	return 0;
}

static int i2c_my_bus_busy(struct my_i2c_struct *my_i2c, int for_busy)
{	
	int i;
	for(i=0;i<500;i++){
		u8 temp = readb(my_i2c->base + I2SR);

		if(for_busy){
			if(temp & I2SR_IBB) 
				return 0;
		}
		else{
			if(!(temp & I2SR_IBB))
				return 0;
		}

		udelay(1);
	}
	return -ETIMEDOUT;
}


static int i2c_my_write(struct my_i2c_struct *my_i2c, struct i2c_msg *msg)
{
	int i, ret;
	for(i=0; i<msg->len; i++){
		writeb(msg->buf[i], my_i2c->base + I2DR);

		ret = i2c_my_trx_compelete(my_i2c);
		if(ret) return ret;

		if(my_i2c->i2csr & I2SR_RXAK)
			return -EIO;
	}
	return 0;
}

static int i2c_my_read(struct my_i2c_struct *my_i2c, struct i2c_msg *msg, bool is_lastmsg)
{	
	u8 temp;
	int i, ret;

	temp = readb(my_i2c->base + I2CR);

	temp &= ~I2CR_MTX;

	if(msg->len == 1)
		temp |= I2CR_TXAK;
	else
		temp &= ~I2CR_TXAK;
	writeb(temp, my_i2c->base + I2CR);

	my_i2c->i2csr = 0;
	readb(my_i2c->base + I2DR);

	for(i=0;i<msg->len;i++){
		ret = i2c_my_trx_compelete(my_i2c);
		if(ret) return ret;

		if(i == msg->len -2){
			temp = readb(my_i2c->base + I2CR);
			temp |=I2CR_TXAK;
			writeb(temp, my_i2c->base + I2CR);
		}
		if(i == msg->len - 1){
			if(is_lastmsg){
				temp = readb(my_i2c->base + I2CR);
				temp &= ~(I2CR_MSTA | I2CR_MTX);
				writeb(temp, my_i2c->base + I2CR);
			}
			else{
				temp = readb(my_i2c->base + I2CR);
				temp |=I2CR_MTX;
				writeb(temp, my_i2c->base + I2CR);
			}

		}	
		
		msg->buf[i] = readb(my_i2c->base + I2DR);

			}

	return 0;
}


static int i2c_my_start(struct my_i2c_struct *my_i2c, struct i2c_msg *msg)
{	
	u8 temp;
	int ret;

	temp = readb(my_i2c->base + I2CR);

	temp |= (I2CR_MSTA | I2CR_IIEN | I2CR_MTX);

	writeb(temp, my_i2c->base + I2CR);

	ret = i2c_my_bus_busy(my_i2c, 1);
	if(ret) return ret;

	my_i2c->i2csr = 0;
	u8 addr = (msg->addr << 1);
	if(msg->flags & I2C_M_RD) addr |=1;
	
	writeb(addr, my_i2c->base + I2DR);

	ret = i2c_my_trx_compelete(my_i2c);
	if(ret) return ret;

	if(my_i2c->i2csr & I2SR_RXAK) return -ENXIO;


	return 0;
}

static void i2c_my_stop(struct my_i2c_struct *my_i2c)
{
	u8 temp;
	int ret;

	temp = readb(my_i2c->base + I2CR);

	temp &= ~(I2CR_MSTA | I2CR_MTX | I2CR_TXAK);

	writeb(temp, my_i2c->base + I2CR);

	ret = i2c_my_bus_busy(my_i2c,0);
	
}

static irqreturn_t i2c_my_isr(int irq, void *dev_id)
{
	struct my_i2c_struct* my_i2c = (struct my_i2c_struct*) dev_id;
	u8 temp;

	temp = readb(my_i2c->base + I2SR);

	if(temp & I2SR_IIF){
		
		writeb(0, my_i2c->base + I2SR);
		my_i2c->i2csr = temp;
		wake_up(&my_i2c->queue);

		return IRQ_HANDLED;
	}
	
	return IRQ_NONE;
}

static int i2c_bus_my_master_xfer(struct i2c_adapter *i2c_adap,
		    struct i2c_msg msgs[], int num)
{	
	int i;
	u8 temp;

	struct my_i2c_struct* my_i2c = container_of(i2c_adap, struct my_i2c_struct, adapter);
	writeb(I2CR_IEN, my_i2c->base + I2CR);

	int ret = i2c_my_bus_busy(my_i2c, 0);
	if(ret) goto out;

	ret = i2c_my_start(my_i2c, &msgs[0]);
	if(ret) goto out;

	for(i=0; i<num;i++){
		if(i>0){
			temp = readb(my_i2c->base + I2CR);
			temp |= I2CR_RSTA;
			writeb(temp, my_i2c->base + I2CR);

			ret = i2c_my_start(my_i2c, &msgs[i]);
			if(ret) goto out;
		}

		if(msgs[i].flags & I2C_M_RD) ret = i2c_my_read(my_i2c, &msgs[i],(i==num -1));
		else ret = i2c_my_write(my_i2c, &msgs[i]);

		if(ret) goto out;
	}
	
	out:
		i2c_my_stop(my_i2c);

		writeb(0, my_i2c->base + I2CR);

		if(ret) return ret;
		else return num;
}

static u32 i2c_bus_my_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_READ_BLOCK_DATA;
}


static const struct i2c_algorithm i2c_bus_my_algo = {
	.master_xfer   = i2c_bus_my_master_xfer,
	.functionality = i2c_bus_my_func,
};

static int i2c_bus_my_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct my_i2c_struct* my_i2c;
	int ret;
	u32 freq;
	int i;
	u8 ifdr_value = 0x1F;

	my_i2c = devm_kzalloc(&pdev->dev, sizeof(*my_i2c), GFP_KERNEL);
	if (!my_i2c)
        return -ENOMEM;


	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
        return -ENODEV;

	my_i2c->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(my_i2c->base))
        return PTR_ERR(my_i2c->base);

	my_i2c->irq = platform_get_irq(pdev, 0);
	if (my_i2c->irq < 0)
        return my_i2c->irq;

	my_i2c->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(my_i2c->clk)) {
        dev_err(&pdev->dev, "failed to get i2c clk\n");
        return PTR_ERR(my_i2c->clk);
    }
	ret = clk_prepare_enable(my_i2c->clk);
	if (ret)
        return ret;

	init_waitqueue_head(&my_i2c->queue);

	ret = devm_request_irq(&pdev->dev, my_i2c->irq, i2c_my_isr, IRQF_NO_SUSPEND,  "my_i2c", my_i2c);
	if (ret) {
		clk_disable_unprepare(my_i2c->clk);
		return ret;
	}

	my_i2c->adapter.owner  = THIS_MODULE;
	my_i2c->adapter.algo = &i2c_bus_my_algo;
	my_i2c->adapter.dev.parent = &pdev->dev;
	my_i2c->adapter.nr = -1;
	strlcpy(my_i2c->adapter.name, "zxr-i2c", sizeof(my_i2c->adapter.name));
	my_i2c->adapter.dev.of_node = pdev->dev.of_node;

	/* set clk*/
	ret=  of_property_read_u32(pdev->dev.of_node, "clock-frequency", &freq);
	if (ret){
     freq = 100000;
	}

	unsigned long div = clk_get_rate(my_i2c->clk) / freq;

	for(i = 0;i<ARRAY_SIZE(i2c_clk_div);i++){
		if(i2c_clk_div[i][0]>=div){
			ifdr_value = i2c_clk_div[i][1];
			break;
		}

	}
	writeb(ifdr_value, my_i2c->base + IFDR);

	/*disable i2c*/

	writeb(0, my_i2c->base + I2CR);

	platform_set_drvdata(pdev, my_i2c);
	

	ret = i2c_add_numbered_adapter(&my_i2c->adapter);
	if (ret) {
        clk_disable_unprepare(my_i2c->clk);
        return ret;
    }
	return 0;
}

static int i2c_bus_my_remove(struct platform_device *pdev)
{
	struct my_i2c_struct *my_i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&my_i2c->adapter);
    clk_disable_unprepare(my_i2c->clk);
	return 0;
}

static const struct of_device_id i2c_bus_my_dt_ids[] = {
	{ .compatible ="i2c-bus-zxr", },
	{ /* sentinel */ }
};

static struct platform_driver i2c_bus_my_driver = {
	.driver		= {
		.name	= "i2c-zxr",
		.of_match_table	= of_match_ptr(i2c_bus_my_dt_ids),
	},
	.probe		= i2c_bus_my_probe,
	.remove		= i2c_bus_my_remove,
};


static int __init i2c_adap_my_init(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	return platform_driver_register(&i2c_bus_my_driver);
}
subsys_initcall(i2c_adap_my_init);

static void __exit i2c_adap_my_exit(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	platform_driver_unregister(&i2c_bus_my_driver);
}
module_exit(i2c_adap_my_exit);

MODULE_LICENSE("GPL");
