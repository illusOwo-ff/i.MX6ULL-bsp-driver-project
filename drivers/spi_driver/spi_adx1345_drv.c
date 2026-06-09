#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/acpi.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include <linux/uaccess.h>



#define ADXL345_REG_DEVID        0x00
#define ADXL345_REG_POWER_CTL    0x2D
#define ADXL345_REG_DATA_FORMAT  0x31
#define ADXL345_REG_DATAX0       0x32

#define ADXL345_DEVID_VAL        0xE5

#define ADXL345_READ_FLAG        0x80    /* bit7=1表示读 */
#define ADXL345_MULTI_FLAG       0x40    /* bit6=1表示多字节 */

static struct spi_device *g_spi;         /* probe里赋值，fops里用 */
static int major;                        /* 主设备号 */
static struct class *adxl345_class;
static struct device *adxl345_device;


static int adxl345_read_reg(struct spi_device *spi, u8 reg)
{
    u8 addr = ADXL345_READ_FLAG | reg;
    u8 val;
    int ret;
    ret = spi_write_then_read(spi, &addr, 1, &val, 1);

    if(ret <0) return ret;

    return val;
}

static int adxl345_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };
    return spi_write(spi, buf, 2);
}

static int adxl345_drv_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t adxl345_drv_read (struct file *filp, char __user *buf, size_t count, loff_t *f_pos){
    if(count < 6) return -EINVAL;

    /*第一步：构造地址字节，多字节读从DATAX0开始*/
    u8 addr = ADXL345_READ_FLAG | ADXL345_MULTI_FLAG | ADXL345_REG_DATAX0;
    u8 raw[6];
    int ret = spi_write_then_read(g_spi, &addr, 1, raw, 6);
    if(ret < 0) return ret;

    /*第二步：组合成有符号16位整数*/
    int16_t accel[3];
    accel[0] = (int16_t)((raw[1] << 8) | raw[0]);   /* X */
    accel[1] = (int16_t)((raw[3] << 8) | raw[2]);   /* Y */
    accel[2] = (int16_t)((raw[5] << 8) | raw[4]);   /* Z */

    /*第三步：拷贝到用户空间*/
    if(copy_to_user(buf, accel, sizeof(accel))) return -EFAULT;

    return sizeof(accel);
}

static int adxl345_drv_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations adxl345_fops = {
    .owner   = THIS_MODULE,
    .open    = adxl345_drv_open,
    .read    = adxl345_drv_read,
    .release = adxl345_drv_release,
};

static const struct of_device_id my_spi_dev_dt_ids[] = {
	{ .compatible = "zxr-myadx" },
	{},
};

static int my_spi_dev_probe(struct spi_device *spi)
{   
    int ret;
    /*第一步：配置SPI参数*/
    spi->mode = SPI_MODE_0;
    spi->max_speed_hz = 1000000;
    spi->bits_per_word = 8;

    ret = spi_setup(spi);
    if(ret)
        return ret;

    /*第二步：验证设备ID*/
    ret = adxl345_read_reg(spi, ADXL345_REG_DEVID);
    if(ret < 0) return ret;

    if(ret != ADXL345_DEVID_VAL){
        dev_err(&spi->dev, "Wrong device ID: 0x%02x, expected 0xE5\n", ret);
        return -ENODEV;
    }

    /*第三步：初始化ADXL345*/
    adxl345_write_reg(spi, ADXL345_REG_DATA_FORMAT, 0x00);
    adxl345_write_reg(spi, ADXL345_REG_POWER_CTL, 0x08);

    major = register_chrdev(0, "adxl345", &adxl345_fops);
    if(major<0) return major;

    adxl345_class = class_create(THIS_MODULE, "adxl345_class");
    if (IS_ERR(adxl345_class)) {
        unregister_chrdev(major, "adxl345");
        return PTR_ERR(adxl345_class);
    }

    adxl345_device = device_create(adxl345_class, NULL, MKDEV(major, 0), NULL, "adxl345");
    if (IS_ERR(adxl345_device)) {
        class_destroy(adxl345_class);
        unregister_chrdev(major, "adxl345");
        return PTR_ERR(adxl345_device);
    }    
    g_spi= spi;

    dev_info(&spi->dev, "ADXL345 initialized, major=%d\n", major);

    return 0;
}

static int my_spi_dev_remove(struct spi_device *spi)
{
    adxl345_write_reg(spi, ADXL345_REG_POWER_CTL, 0x00);   /* 关闭测量 */
    device_destroy(adxl345_class, MKDEV(major, 0));
    class_destroy(adxl345_class);
    unregister_chrdev(major, "adxl345");

    return 0;
}

static struct spi_driver my_spi_driver = {
	.driver = {
		.name =		"my_spi_adx_drv",
		.of_match_table = of_match_ptr(my_spi_dev_dt_ids),
	},
	.probe =	my_spi_dev_probe,
	.remove =	my_spi_dev_remove,

};

static int __init my_spi_dev_init(void){
    int ret;
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);

    ret = spi_register_driver(&my_spi_driver);
    
    return ret;
}

static void __exit my_spi_dev_exit(void){
    printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
    spi_unregister_driver(&my_spi_driver);
}

module_init(my_spi_dev_init);
module_exit(my_spi_dev_exit);
MODULE_LICENSE("GPL");