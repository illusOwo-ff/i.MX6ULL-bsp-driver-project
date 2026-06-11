#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/irq.h>               
#include <linux/irqdesc.h>            
#include <linux/interrupt.h>          
#include <linux/irqdomain.h>           
#include <linux/irqchip/chained_irq.h> 

#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/driver.h>         

/* IMX6ULL GPIO寄存器偏移量 */
#define GPIO_DR        0x00   /* 数据寄存器 */
#define GPIO_GDIR      0x04   /* 方向寄存器 */
#define GPIO_PSR       0x08   /* 引脚状态寄存器 */
#define GPIO_ICR1      0x0C   /* 中断配置寄存器1 (pin0-15) */
#define GPIO_ICR2      0x10   /* 中断配置寄存器2 (pin16-31) */
#define GPIO_IMR       0x14   /* 中断屏蔽寄存器 */
#define GPIO_ISR       0x18   /* 中断状态寄存器 */
#define GPIO_EDGE_SEL  0x1C   /* 边沿选择寄存器 */

/* ICR触发类型编码值 */
#define ICR_LOW_LEVEL    0x0   /* 00：低电平触发 */
#define ICR_HIGH_LEVEL   0x1   /* 01：高电平触发 */
#define ICR_RISE_EDGE    0x2   /* 10：上升沿触发 */
#define ICR_FALL_EDGE    0x3   /* 11：下降沿触发 */


struct my_gpio_data {
    void __iomem        *base;          /* ioremap后的寄存器虚拟基地址 */
    struct gpio_chip     gc;            /* GPIO控制器结构体 */
    struct irq_domain   *domain;        /* 中断域 */
    struct clk          *clk;           /* 时钟 */
    int                  parent_irq_low;  /* 父中断号：pin0-15 */
    int                  parent_irq_high; /* 父中断号：pin16-31 */
    spinlock_t           lock;          /* 保护寄存器读-改-写操作 */
};

static	int	my_gpio_get_direction(struct gpio_chip *chip, unsigned offset){
    struct my_gpio_data* data = gpiochip_get_data(chip);

    u32 gdir;
    gdir = readl(data->base + GPIO_GDIR);
    return !(gdir & BIT(offset));
 }

static	int	my_gpio_direction_input(struct gpio_chip *chip, unsigned offset){
    struct my_gpio_data* data = gpiochip_get_data(chip);
    unsigned long flags;
    spin_lock_irqsave(&data->lock, flags);

    u32 val = readl(data->base + GPIO_GDIR);
    val &= ~BIT(offset);
    writel(val, data->base + GPIO_GDIR);

    spin_unlock_irqrestore(&data->lock, flags);

    return 0;    
}

static	int	my_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value){
    struct my_gpio_data* data = gpiochip_get_data(chip);
    unsigned long flags;
    spin_lock_irqsave(&data->lock, flags);

    u32 dr = readl(data->base + GPIO_DR);
    if(value) dr |= BIT(offset);
    else dr &= ~BIT(offset);
    writel(dr, data->base + GPIO_DR);

    u32 gdir = readl(data->base + GPIO_GDIR);
    gdir |= BIT(offset);
    writel(gdir, data->base + GPIO_GDIR);

    spin_unlock_irqrestore(&data->lock, flags);

    return 0;
}

static int my_gpio_get(struct gpio_chip *chip, unsigned offset){
    struct my_gpio_data* data = gpiochip_get_data(chip);

    return !!(readl(data->base + GPIO_PSR) & BIT(offset));
}

static void	my_gpio_set(struct gpio_chip *chip, unsigned offset, int value){
    struct my_gpio_data* data = gpiochip_get_data(chip);
    unsigned long flags;
    
    spin_lock_irqsave(&data->lock, flags);
    u32 dr = readl(data->base + GPIO_DR);
    if(value) dr |= BIT(offset);
    else dr &= ~BIT(offset);
    writel(dr, data->base + GPIO_DR);

    spin_unlock_irqrestore(&data->lock, flags);

}

static int my_gpio_to_irq(struct gpio_chip *chip, unsigned offset){
    struct my_gpio_data* data = gpiochip_get_data(chip);

    return irq_find_mapping(data->domain, offset);
}

static void my_gpio_irq_handler(struct irq_desc *desc)
{
    struct my_gpio_data* data = irq_desc_get_handler_data(desc);
    struct irq_chip* chip = irq_desc_get_chip(desc);

    chained_irq_enter(chip, desc);

    u32 pending = readl(data->base + GPIO_ISR) &readl(data->base + GPIO_IMR);

    while(pending){
        int hwirq = __ffs(pending);
        unsigned int virq = irq_find_mapping(data->domain, hwirq);
        generic_handle_irq(virq);

        pending &= ~BIT(hwirq);
    }

    chained_irq_exit(chip, desc);
}


static void my_irq_mask(struct irq_data *d)
{
    struct my_gpio_data* data = irq_data_get_irq_chip_data(d);
    unsigned long hwirq = d->hwirq;
    unsigned long flags;
    spin_lock_irqsave(&data->lock, flags);

    u32 imr = readl(data->base + GPIO_IMR);
    imr &= ~BIT(hwirq);
    writel(imr, data->base +GPIO_IMR);

    spin_unlock_irqrestore(&data->lock, flags);
}

static void my_irq_unmask(struct irq_data *d)
{
    struct my_gpio_data* data = irq_data_get_irq_chip_data(d);
    unsigned long hwirq = d->hwirq;
    unsigned long flags;

    spin_lock_irqsave(&data->lock, flags);

    u32 imr = readl(data->base + GPIO_IMR);
    imr |= BIT(hwirq);
    writel(imr, data->base + GPIO_IMR);

    spin_unlock_irqrestore(&data->lock, flags);
}

static void my_irq_ack(struct irq_data *d)
{
    struct my_gpio_data* data = irq_data_get_irq_chip_data(d);
    unsigned long hwirq = d->hwirq;
   
    writel(BIT(hwirq), data->base + GPIO_ISR);
}

static int my_irq_set_type(struct irq_data *d, unsigned int type)
{
    struct my_gpio_data* data = irq_data_get_irq_chip_data(d);
    unsigned long hwirq = d->hwirq;
    unsigned long flags, shift;
    void __iomem* thisicr;

    spin_lock_irqsave(&data->lock, flags);

    u32 edge = readl(data->base + GPIO_EDGE_SEL);
    if(type == IRQ_TYPE_EDGE_BOTH) {
        edge |= BIT(hwirq);
        writel(edge, data->base + GPIO_EDGE_SEL);
        goto unlock;
    }
    else{
        edge &= ~BIT(hwirq);
        writel(edge, data->base + GPIO_EDGE_SEL);
    }

    if(hwirq<16) {
        thisicr = data->base + GPIO_ICR1;
        shift = hwirq*2;
        }
    else{
        thisicr = data->base + GPIO_ICR2;
        shift = (hwirq -16)*2;
    }

    u32 icr =readl(thisicr);
    icr &= ~(0x3 << shift);
    switch (type)
    {
    case   IRQ_TYPE_LEVEL_LOW:
        icr |= (ICR_LOW_LEVEL << shift);
        break;
    case   IRQ_TYPE_LEVEL_HIGH:
        icr |= (ICR_HIGH_LEVEL << shift);
        break;
    case   IRQ_TYPE_EDGE_RISING:
        icr |= (ICR_RISE_EDGE << shift);
        break;
    case   IRQ_TYPE_EDGE_FALLING:
        icr |= (ICR_FALL_EDGE << shift);
        break;
    default:
        spin_unlock_irqrestore(&data->lock, flags);
        return -EINVAL;
        break;
    }

    writel(icr, thisicr);

    unlock:
        spin_unlock_irqrestore(&data->lock, flags);
    
    if(type & IRQ_TYPE_EDGE_BOTH) irq_set_handler_locked(d, handle_edge_irq);
    else irq_set_handler_locked(d, handle_level_irq);

    return 0;
}

static struct irq_chip my_irq_chip = {
    .name        = "zxr-gpio-irq",
    .irq_mask    = my_irq_mask,
    .irq_unmask  = my_irq_unmask,
    .irq_ack     = my_irq_ack,
    .irq_set_type = my_irq_set_type,
};

static int my_irq_domain_map(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw){
    struct my_gpio_data* data = d->host_data;
    int ret;
    irq_set_chip_and_handler(virq, &my_irq_chip, handle_level_irq);

    ret = irq_set_chip_data(virq, data);
    if(ret) return ret;

    return 0;
}

static const struct irq_domain_ops my_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
    .map = my_irq_domain_map,
};


static const struct of_device_id gpio_interrupt_my_dt_ids[] = {
	{ .compatible = "zxr-my_gpio_interrupt_adapter", },
	{ /* sentinel */ }
};

static int gpio_interrupt_my_probe(struct platform_device *pdev)
{
    struct resource *res;
	struct my_gpio_data* data;
	int ret, i;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
        return -ENOMEM;


	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
        return -ENODEV;

	data->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->base))
        return PTR_ERR(data->base);

	data->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(data->clk)) {
    data->clk = NULL;   /* 时钟是always-on的，不需要显式管理 */
} else {
    ret = clk_prepare_enable(data->clk);
    if (ret)
        return ret;
}
    
    data->parent_irq_low = platform_get_irq(pdev, 0);
    if (data->parent_irq_low < 0) {
        ret = data->parent_irq_low;
        goto err_clk;
    }

    data->parent_irq_high = platform_get_irq(pdev, 1);
    if (data->parent_irq_high < 0) {
        ret = data->parent_irq_high;
        goto err_clk;
    }

    spin_lock_init(&data->lock);

    data->gc.label = "zxr-my_gpio";
    data->gc.parent = &pdev->dev;
    data->gc.owner = THIS_MODULE;
    data->gc.base = -1;
    data->gc.ngpio = 32;
    data->gc.direction_input = my_gpio_direction_input;
    data->gc.direction_output = my_gpio_direction_output;
    data->gc.get = my_gpio_get;
    data->gc.set = my_gpio_set;
    data->gc.get_direction = my_gpio_get_direction;
    data->gc.request = gpiochip_generic_request;
    data->gc.free = gpiochip_generic_free;
    data->gc.to_irq = my_gpio_to_irq;
    data->gc.of_node = pdev->dev.of_node;
    data->gc.can_sleep = false;

    ret = gpiochip_add_data(&data->gc, data);
    if(ret){
        goto err_clk;
    }

    data->domain = irq_domain_add_linear(pdev->dev.of_node, 32, &my_domain_ops, data);
    if (!data->domain) {
        ret = -ENOMEM;
        goto err_gpiochip;
    }

    for (i = 0; i < 32; i++)
    irq_create_mapping(data->domain, i);

	irq_set_chained_handler_and_data(data->parent_irq_low, my_gpio_irq_handler, data);
    irq_set_chained_handler_and_data(data->parent_irq_high, my_gpio_irq_handler, data);
    

    platform_set_drvdata(pdev, data);
	
	return 0;

    err_gpiochip:
        gpiochip_remove(&data->gc);
    err_clk:
        clk_disable_unprepare(data->clk);
    return ret;
}

static int gpio_interrupt_my_remove(struct platform_device *pdev)
{   
    struct my_gpio_data* data = platform_get_drvdata(pdev);
    irq_set_chained_handler_and_data(data->parent_irq_low, NULL, NULL);
    irq_set_chained_handler_and_data(data->parent_irq_high, NULL, NULL);
    
    irq_domain_remove(data->domain);

    gpiochip_remove(&data->gc);

    if (data->clk)
    clk_disable_unprepare(data->clk);

    return 0;
}


static struct platform_driver gpio_interrupt_my_driver = {
	.probe = gpio_interrupt_my_probe,
	.remove = gpio_interrupt_my_remove,
	.driver = {
		.name = "zxr_gpio_interrupt",
		.of_match_table = gpio_interrupt_my_dt_ids,
	},
};

static int my_gpio_interrupt_init(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	return platform_driver_register(&gpio_interrupt_my_driver);
}

static void my_gpio_interrupt_exit(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	platform_driver_unregister(&gpio_interrupt_my_driver);
}

module_init(my_gpio_interrupt_init);
module_exit(my_gpio_interrupt_exit);
MODULE_LICENSE("GPL");