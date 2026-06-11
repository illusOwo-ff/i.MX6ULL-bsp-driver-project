#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/of.h>

struct my_test_data {
    struct gpio_desc *gpiod;
    int irq;
    int count;
};

static irqreturn_t my_test_irq_handler(int irq, void *dev_id)
{
    struct my_test_data *data = dev_id;
    data->count++;
    printk("GPIO interrupt triggered! count=%d\n", data->count);
    return IRQ_HANDLED;
}

static int my_test_probe(struct platform_device *pdev)
{
    struct my_test_data *data;
    int ret;

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->gpiod = devm_gpiod_get(&pdev->dev, "test", GPIOD_IN);

    if (IS_ERR(data->gpiod))
        return PTR_ERR(data->gpiod);

    /* 获取virq（to_irq回调）*/
    data->irq = gpiod_to_irq(data->gpiod);
    if (data->irq < 0)
        return data->irq;

    /* 请求中断（触发irq_set_type + irq_unmask）*/
    ret = devm_request_irq(&pdev->dev, data->irq,
                           my_test_irq_handler,
                           IRQF_TRIGGER_FALLING,
                           "gpio-irq-test", data);
    if (ret)
        return ret;

    data->count = 0;
    platform_set_drvdata(pdev, data);
    printk("GPIO interrupt test driver probed, irq=%d\n", data->irq);
    return 0;
}

static int my_test_remove(struct platform_device *pdev)
{
    printk("GPIO interrupt test driver removed\n");
    return 0;
}


static const struct of_device_id my_test_dt_ids[] = {
    { .compatible = "zxr,gpio-irq-test" },
    { }
};

static struct platform_driver my_test_driver = {
    .probe  = my_test_probe,
    .remove = my_test_remove,
    .driver = {
        .name = "gpio-irq-test",
        .of_match_table = my_test_dt_ids,
    },
};

static int my_gpio_irq_test_init(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	return platform_driver_register(&my_test_driver);
}

static void my_gpio_irq_test_exit(void)
{
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	platform_driver_unregister(&my_test_driver);
}

module_init(my_gpio_irq_test_init);
module_exit(my_gpio_irq_test_exit);
MODULE_LICENSE("GPL");