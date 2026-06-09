#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_bitbang.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/of.h>

/* 寄存器偏移 */
#define ECSPI_RXDATA    0x00    /* 接收数据寄存器 */
#define ECSPI_TXDATA    0x04    /* 发送数据寄存器 */
#define ECSPI_CONREG    0x08    /* 控制寄存器 */
#define ECSPI_CONFIGREG 0x0C    /* 配置寄存器（SPI模式相关） */
#define ECSPI_INTREG    0x10    /* 中断控制寄存器 */
#define ECSPI_DMAREG    0x14    /* DMA控制寄存器（不用） */
#define ECSPI_STATREG   0x18    /* 状态寄存器 */
#define ECSPI_PERIODREG 0x1C    /* 采样周期寄存器（不用） */
#define ECSPI_TESTREG   0x20    /* 测试寄存器（不用） */

/* CONREG bits */
#define ECSPI_CONREG_EN         BIT(0)      /* 模块使能 */
#define ECSPI_CONREG_HT         BIT(1)      /* 硬件触发（不用） */
#define ECSPI_CONREG_XCH        BIT(2)      /* 启动一次交换/传输 */
#define ECSPI_CONREG_SMC        BIT(3)      /* 立即启动模式（设1后写TXDATA自动开始传输） */
#define ECSPI_CONREG_CM_SHIFT   4           /* 通道模式（Master/Slave），每通道1bit */
#define ECSPI_CONREG_DRCTL_SHIFT 16         /* 数据就绪控制（不用） */
#define ECSPI_CONREG_CS_SHIFT   18          /* 片选通道选择（2bit） */
#define ECSPI_CONREG_BL_SHIFT   20          /* Burst Length（传输长度-1） */
#define ECSPI_CONREG_PRE_SHIFT  12          /* 预分频值（4bit，分频系数=值+1） */
#define ECSPI_CONREG_POST_SHIFT 8           /* 后分频值（4bit，分频系数=2^值） */

/* CONFIGREG bits — 每个通道占1bit，channel 0的位在最低位 */
#define ECSPI_CONFIGREG_SCLK_PHA_SHIFT  0   /* CPHA：时钟相位 */
#define ECSPI_CONFIGREG_SCLK_POL_SHIFT  4   /* CPOL：时钟极性 */
#define ECSPI_CONFIGREG_SS_CTL_SHIFT    8   /* 片选在burst间是否保持 */
#define ECSPI_CONFIGREG_SS_POL_SHIFT    12  /* 片选极性（0=低有效，1=高有效） */

/* INTREG bits */
#define ECSPI_INTREG_TEEN       BIT(0)      /* TX FIFO空中断使能 */
#define ECSPI_INTREG_TDREN      BIT(1)      /* TX FIFO数据请求中断使能（不用） */
#define ECSPI_INTREG_THEN       BIT(2)      /* TX FIFO半空中断使能（不用） */
#define ECSPI_INTREG_RREN       BIT(3)      /* RX FIFO数据就绪中断使能 */
#define ECSPI_INTREG_RDREN      BIT(4)      /* RX FIFO数据请求中断使能（不用） */
#define ECSPI_INTREG_RFEN       BIT(5)      /* RX FIFO满中断使能（不用） */
#define ECSPI_INTREG_ROEN       BIT(6)      /* RX FIFO溢出中断使能（不用） */
#define ECSPI_INTREG_TCEN       BIT(7)      /* 传输完成中断使能 */

/* STATREG bits */
#define ECSPI_STATREG_TE        BIT(0)      /* TX FIFO空 */
#define ECSPI_STATREG_TDR       BIT(1)      /* TX FIFO数据请求（有空位） */
#define ECSPI_STATREG_TF        BIT(2)      /* TX FIFO满 */
#define ECSPI_STATREG_RR        BIT(3)      /* RX FIFO有数据就绪 */
#define ECSPI_STATREG_RDR       BIT(4)      /* RX FIFO数据请求 */
#define ECSPI_STATREG_RF        BIT(5)      /* RX FIFO满 */
#define ECSPI_STATREG_RO        BIT(6)      /* RX FIFO溢出 */
#define ECSPI_STATREG_TC        BIT(7)      /* 传输完成（写1清除） */

/* CONREG字段掩码 */
#define ECSPI_CONREG_CS_MASK    (0x3 << ECSPI_CONREG_CS_SHIFT)     /* [19:18] */
#define ECSPI_CONREG_BL_MASK    (0xFFF << ECSPI_CONREG_BL_SHIFT)   /* [31:20] */
#define ECSPI_CONREG_PRE_MASK   (0xF << ECSPI_CONREG_PRE_SHIFT)    /* [15:12] */
#define ECSPI_CONREG_POST_MASK  (0xF << ECSPI_CONREG_POST_SHIFT)   /* [11:8] */


#define ECSPI_FIFO_DEPTH  64

struct my_spi_data {
    void __iomem        *base;       /* 寄存器基地址（ioremap后的虚拟地址） */
    struct clk          *clk;        /* ECSPI模块时钟 */
    int                  irq;        /* 中断号 */

    /* 以下是中断模式专用字段，PIO模式不会用到，但定义在这里不影响 */
    struct completion    xfer_done;   /* 中断模式：等待传输完成 */
    const u8            *tx_buf;     /* 中断模式：ISR里用的发送指针 */
    u8                  *rx_buf;     /* 中断模式：ISR里用的接收指针 */
    int                  tx_remain;  /* 中断模式：剩余发送字节数 */
    int                  rx_remain;  /* 中断模式：剩余接收字节数 */

    int bytes_per_word;
};

static bool use_irq = false;

static void my_spi_configure_xfer(struct my_spi_data *myspi_data, struct spi_device *spi, struct spi_transfer *xfer){

    int post,pre;
    /*第一步：计算时钟分频值*/
    u32 target = xfer->speed_hz;
    if(target == 0) target = spi->max_speed_hz;

    unsigned long source = clk_get_rate(myspi_data->clk);

    /*计算PRE和POST*/
    for(post=0; post<16; post++){
        pre = DIV_ROUND_UP(source, target * (1 << post)) -1;

        if(pre <= 15) break;
    }
    if (post == 16)
    {
        post = 15;
        pre = 15;
    }
    
    /*第二步：计算Burst Length*/
    u8 bits_per_word = xfer->bits_per_word;
    if(bits_per_word == 0) bits_per_word = spi->bits_per_word;
    if(bits_per_word ==0) bits_per_word = 8;

    u8 bl = bits_per_word -1;

    /*第三步：写入CONREG*/
    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);

    conreg &= ~ECSPI_CONREG_PRE_MASK;
    conreg &= ~ECSPI_CONREG_POST_MASK;
    conreg &= ~ECSPI_CONREG_BL_MASK;

    conreg |=(pre << ECSPI_CONREG_PRE_SHIFT);
    conreg |=(post << ECSPI_CONREG_POST_SHIFT);
    conreg |=(bl << ECSPI_CONREG_BL_SHIFT);

    writel(conreg, myspi_data->base + ECSPI_CONREG);
}

static void my_spi_write_one_word(struct my_spi_data *myspi_data){
    
    u32 val = 0;
    if (myspi_data->tx_buf) {
        switch (myspi_data->bytes_per_word) {
        case 1: val = *myspi_data->tx_buf; break;
        case 2: val = *(u16 *)myspi_data->tx_buf; break;
        case 4: val = *(u32 *)myspi_data->tx_buf; break;
        }
        myspi_data->tx_buf += myspi_data->bytes_per_word;
    }
    writel(val, myspi_data->base + ECSPI_TXDATA);
    myspi_data->tx_remain--;
}

static void my_spi_read_one_word(struct my_spi_data *myspi_data)
{
    u32 val = readl(myspi_data->base + ECSPI_RXDATA);
    if (myspi_data->rx_buf) {
        switch (myspi_data->bytes_per_word) {
        case 1: *myspi_data->rx_buf = val & 0xFF; break;
        case 2: *(u16 *)myspi_data->rx_buf = val & 0xFFFF; break;
        case 4: *(u32 *)myspi_data->rx_buf = val; break;
        }
        myspi_data->rx_buf += myspi_data->bytes_per_word;
    }
    myspi_data->rx_remain--;
}

static irqreturn_t my_spi_irq(int irq, void *dev_id)
{
    struct my_spi_data* myspi_data = (struct my_spi_data*) dev_id;
    /*第一步：批量读取所有已完成的RX数据*/
    while(readl(myspi_data->base + ECSPI_STATREG) & ECSPI_STATREG_RR){
        my_spi_read_one_word(myspi_data);
    }

    /*第二步：补充TX FIFO*/
    while(myspi_data->tx_remain > 0){

        if(readl(myspi_data->base + ECSPI_STATREG) & ECSPI_STATREG_TF) break;

        my_spi_write_one_word(myspi_data);
    }

    /*第三步：检查是否全部完成*/
    if(myspi_data->rx_remain == 0){
        writel(0, myspi_data->base + ECSPI_INTREG);
        complete(&myspi_data->xfer_done);
    }

    return IRQ_HANDLED;
}

static int my_spi_transfer_irq(struct spi_master *master, struct spi_device *spi, struct spi_transfer *transfer){

    struct my_spi_data* myspi_data = spi_master_get_devdata(master);
    int i;

    /*第一步：设SMC=1*/
    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg |= ECSPI_CONREG_SMC;
    writel(conreg, myspi_data->base + ECSPI_CONREG);

    /*第二步：保存传输信息*/
    myspi_data->tx_buf = transfer->tx_buf;
    myspi_data->rx_buf = transfer->rx_buf;
    myspi_data->tx_remain = transfer->len / myspi_data->bytes_per_word;
    myspi_data->rx_remain = myspi_data->tx_remain;

    /*第三步：重置completion*/
    reinit_completion(&myspi_data->xfer_done);

    /*第四步：预填充TX FIFO*/
    int prefill = min(myspi_data->tx_remain, ECSPI_FIFO_DEPTH);
    for(i=0; i<prefill; i++){
        my_spi_write_one_word(myspi_data);
    }

    /*第五步：使能RREN中断*/
    writel(ECSPI_INTREG_RREN, myspi_data->base + ECSPI_INTREG);

    /*第六步：等待完成*/
    unsigned long timeout = wait_for_completion_timeout(&myspi_data->xfer_done, msecs_to_jiffies(1000));

    if(timeout==0){
        writel(0, myspi_data->base + ECSPI_INTREG);
        return -ETIMEDOUT;
    }

    writel(ECSPI_STATREG_TC, myspi_data->base + ECSPI_STATREG);

    return 0;
}

static int my_spi_transfer_pio(struct spi_master *master, struct spi_device *spi, struct spi_transfer *transfer){

    struct my_spi_data* myspi_data = spi_master_get_devdata(master);
    int i;

    /*第一步：设置SMC立即启动模式*/
    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg |= ECSPI_CONREG_SMC;
    writel(conreg, myspi_data->base + ECSPI_CONREG);

    /*第二步：逐字节传输循环*/
    myspi_data->tx_buf = transfer->tx_buf;
    myspi_data->rx_buf = transfer->rx_buf;
    myspi_data->tx_remain = transfer->len / myspi_data->bytes_per_word;
    myspi_data->rx_remain = myspi_data->tx_remain;

    for(i=0; i< transfer->len / myspi_data->bytes_per_word; i++){

            /* 调试
            u32 tx_val = myspi_data->tx_buf ? myspi_data->tx_buf[0] : 0;
    printk("SPI TX[%d]: 0x%02x, CONREG=0x%08x, STATREG=0x%08x\n", 
           i, tx_val,
           readl(myspi_data->base + ECSPI_CONREG),
           readl(myspi_data->base + ECSPI_STATREG));
            */

        my_spi_write_one_word(myspi_data);

        int timeout = 1000;
        while (!(readl(myspi_data->base + ECSPI_STATREG)& ECSPI_STATREG_RR))
        {
            if(--timeout==0){
                return -ETIMEDOUT;
            }
            cpu_relax();
        }

        /*调试

    u32 rx_raw = readl(myspi_data->base + ECSPI_RXDATA);
    printk("SPI RX[%d]: 0x%08x\n", i, rx_raw);
    
    if (myspi_data->rx_buf) {
        switch (myspi_data->bytes_per_word) {
        case 1: *myspi_data->rx_buf = rx_raw & 0xFF; break;
        case 2: *(u16 *)myspi_data->rx_buf = rx_raw & 0xFFFF; break;
        case 4: *(u32 *)myspi_data->rx_buf = rx_raw; break;
        }
        myspi_data->rx_buf += myspi_data->bytes_per_word;
    }
    myspi_data->rx_remain--;

        */

        my_spi_read_one_word(myspi_data);
        
    }

    /*第三步：等待传输完全结束 + 清TC标志*/
    while(!(readl(myspi_data->base + ECSPI_STATREG) & ECSPI_STATREG_TC))
        cpu_relax();
    writel(ECSPI_STATREG_TC, myspi_data->base + ECSPI_STATREG);

    return 0;
}

static	void my_spi_set_cs(struct spi_device *spi, bool enable){

        struct my_spi_data* myspi_data = spi_master_get_devdata(spi->master);

        u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
        conreg &= ~ECSPI_CONREG_CS_MASK;
        conreg |= (spi->chip_select << ECSPI_CONREG_CS_SHIFT);
        writel(conreg, myspi_data->base + ECSPI_CONREG);

}

static int my_spi_transfer_one(struct spi_master *master, struct spi_device *spi, struct spi_transfer *transfer){
    
    struct my_spi_data* myspi_data = spi_master_get_devdata(master);

    my_spi_configure_xfer(myspi_data, spi, transfer);

    int bpw = transfer->bits_per_word ?transfer->bits_per_word : spi->bits_per_word;
    if (!bpw) bpw = 8;
    myspi_data->bytes_per_word = bpw / 8; 

    if(use_irq) return my_spi_transfer_irq(master, spi, transfer);
    else return my_spi_transfer_pio(master, spi, transfer);

}


static int	my_spi_setup(struct spi_device *spi){
    struct my_spi_data* myspi_data = spi_master_get_devdata(spi->master);
    u8 channel = spi->chip_select;

    /*第一步：配置CONFIGREG（SPI模式）*/
    u32 configreg = readl(myspi_data->base + ECSPI_CONFIGREG);
    configreg &= ~BIT(ECSPI_CONFIGREG_SCLK_PHA_SHIFT + channel);
    if(spi->mode & SPI_CPHA)  configreg |= BIT(ECSPI_CONFIGREG_SCLK_PHA_SHIFT + channel);

    configreg &= ~BIT(ECSPI_CONFIGREG_SCLK_POL_SHIFT + channel);
    if(spi->mode & SPI_CPOL) configreg |= BIT(ECSPI_CONFIGREG_SCLK_POL_SHIFT + channel);

    configreg &= ~BIT(ECSPI_CONFIGREG_SS_POL_SHIFT + channel);
    if(spi->mode & SPI_CS_HIGH) configreg |= BIT(ECSPI_CONFIGREG_SS_POL_SHIFT + channel);

    configreg &= ~BIT(ECSPI_CONFIGREG_SS_CTL_SHIFT + channel);

    writel(configreg, myspi_data->base + ECSPI_CONFIGREG);

    /*第二步：配置CONREG（设为Master模式）*/
    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg |=BIT(ECSPI_CONREG_CM_SHIFT + channel);
    writel(conreg, myspi_data->base + ECSPI_CONREG);

    return 0;
}



static const struct of_device_id spi_my_dt_ids[] = {
	{ .compatible = "zxr-my_spi_master", },
	{ /* sentinel */ }
};

static int spi_my_probe(struct platform_device *pdev)
{
   struct spi_master *master; 
   struct resource *res;
   struct my_spi_data* myspi_data;
   int ret;

    master = spi_alloc_master(&pdev->dev, sizeof(struct my_spi_data));
	if (!master)
        return -ENOMEM;
    myspi_data = spi_master_get_devdata(master);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
        return -ENODEV;
        
	myspi_data->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(myspi_data->base))
        return PTR_ERR(myspi_data->base);

	myspi_data->irq = platform_get_irq(pdev, 0);
	if (myspi_data->irq < 0)
        return myspi_data->irq;

	myspi_data->clk = devm_clk_get(&pdev->dev, "per");
	if (IS_ERR(myspi_data->clk)) {
        dev_err(&pdev->dev, "failed to get spi clk\n");
        return PTR_ERR(myspi_data->clk);
    }
	ret = clk_prepare_enable(myspi_data->clk);
	if (ret)
        return ret;

    master->bus_num = pdev->id;
    master->num_chipselect = 4;
    master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
    master->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(32);
    master->dev.of_node = pdev->dev.of_node;
    master->setup = my_spi_setup;
    master->set_cs = my_spi_set_cs;
    master->transfer_one = my_spi_transfer_one;


    platform_set_drvdata(pdev, master);

    /*使能ECSPI硬件*/

    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg &= ~ECSPI_CONREG_EN;
    writel(conreg, myspi_data->base + ECSPI_CONREG);
    udelay(1);

    conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg |=  ECSPI_CONREG_EN;
    writel(conreg, myspi_data->base + ECSPI_CONREG);

	if(use_irq){
	
	ret = devm_request_irq(&pdev->dev, myspi_data->irq, my_spi_irq, 0, dev_name(&pdev->dev), myspi_data);
	if (ret) {
		clk_disable_unprepare(myspi_data->clk);
		return ret;
	}

    init_completion(&myspi_data->xfer_done);

    writel(0, myspi_data->base + ECSPI_INTREG);

    dev_info(&pdev->dev, "Using interrupt mode\n");

    }else{
        dev_info(&pdev->dev, "Using PIO polling mode\n");
    }


	ret = devm_spi_register_master(&pdev->dev, master);

	if (ret) {
        clk_disable_unprepare(myspi_data->clk);
        return ret;
    }

    return 0;
}

static int spi_my_remove(struct platform_device *pdev)
{
    struct my_spi_data* myspi_data;
    struct spi_master* master = platform_get_drvdata(pdev);
    myspi_data = spi_master_get_devdata(master);

    writel(0,myspi_data->base + ECSPI_INTREG);

    /*关闭ECSPI*/

    u32 conreg = readl(myspi_data->base + ECSPI_CONREG);
    conreg &= ~ECSPI_CONREG_EN;
    writel(conreg, myspi_data->base + ECSPI_CONREG);

    clk_disable_unprepare(myspi_data->clk);
    return 0;

}

static struct platform_driver spi_my_driver = {
	.probe = spi_my_probe,
	.remove = spi_my_remove,
	.driver = {
		.name = "zxr_spi",
		.of_match_table = spi_my_dt_ids,
	},
};

static int my_master_init(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	return platform_driver_register(&spi_my_driver);
}

static void my_master_exit(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	platform_driver_unregister(&spi_my_driver);
}

module_param(use_irq, bool, 0444);
module_init(my_master_init);
module_exit(my_master_exit);
MODULE_PARM_DESC(use_irq, "Use interrupt-driven transfer instead of PIO polling");
MODULE_LICENSE("GPL");