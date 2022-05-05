#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/atomic.h>

#define LED_ON                  1
#define LED_OFF                 0

#define GPIO_LED_DEV_CNT        1
#define GPIO_LED_DEV_NAME       "gpio_led"
struct gpio_led_dev_t{
    dev_t devid;/*设备ID*/
    int major;/*主设备号*/
    int minor;/*次设备号*/
    struct cdev cdev;/*字符设备*/
    struct class *class;/*类*/
    struct device *device;/*设备*/
    struct device_node *nd;/*设备树节点*/
    unsigned int nd_gpio_info;/*设备树的gpio节点的属性编号*/
    int dev_status;/*设备状态，0：设备未被使用，>0设备已经被使用*/
    spinlock_t lock;/*自旋锁*/
};
struct gpio_led_dev_t gpio_led_dev;
static int led_open(struct inode *inode, struct file *filp)
{
    unsigned long irq_flag;
    filp->private_data = &gpio_led_dev;
    spin_lock_irqsave(&gpio_led_dev.lock,irq_flag);
    if(gpio_led_dev.dev_status)/*驱动已被占用*/
    {
        spin_unlock_irqrestore(&gpio_led_dev.lock,irq_flag);
        return -EBUSY;
    }
    gpio_led_dev.dev_status++;/*标记驱动已被使用*/
    spin_unlock_irqrestore(&gpio_led_dev.lock,irq_flag);
    return 0;
}
static int led_release(struct inode *inode, struct file *filp)
{
    unsigned long irq_flag;
    struct gpio_led_dev_t *dev = filp->private_data;
    spin_lock_irqsave(&dev->lock,irq_flag);
    if(dev->dev_status)
    {
        dev->dev_status--;/*标记驱动可用*/
    }
    spin_unlock_irqrestore(&dev->lock,irq_flag);
    return 0;
}
static ssize_t led_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    int ret = 0;
    unsigned char databuf[1];
    struct gpio_led_dev_t *dev = filp->private_data;
    ret = copy_from_user(databuf,buf,count);
    if(ret<0)
    {
        ret = -EFAULT;
        return ret;
    }
    switch (databuf[0])
    {
    case LED_ON:
        gpio_direction_output(dev->nd_gpio_info,0);
        break;
    case LED_OFF:
        gpio_direction_output(dev->nd_gpio_info,1);
        break;
    default:
        break;
    }
    return 0;
}
const struct file_operations gpio_fops={
    .owner = THIS_MODULE,
    .write = led_write,
    .open = led_open,
    .release = led_release,
};
static int __init gpio_led_init(void)
{
    int ret = 0;
    /*初始化自旋锁*/
    spin_lock_init(&gpio_led_dev.lock);
    gpio_led_dev.dev_status = 0;
    /*注册驱动*/
    gpio_led_dev.major = 0;/*由linux内核分配设备ID*/
    if(gpio_led_dev.major)/*给定主设备号*/
    {
        gpio_led_dev.devid = MKDEV(gpio_led_dev.major,0);
        ret = register_chrdev_region(gpio_led_dev.devid,GPIO_LED_DEV_CNT,GPIO_LED_DEV_NAME);
    }
    else
    {
        ret = alloc_chrdev_region(&gpio_led_dev.devid,0,GPIO_LED_DEV_CNT,GPIO_LED_DEV_NAME);
    }
    if(ret<0)
    {
        goto fail_register_chrdev;
    }
    else
    {
        gpio_led_dev.major = MAJOR(gpio_led_dev.devid);
        gpio_led_dev.minor = MINOR(gpio_led_dev.devid);
        printk("major = %d,minor = %d\r\n",gpio_led_dev.major,gpio_led_dev.minor);
    }
    /*初始化cdev*/
    gpio_led_dev.cdev.owner = THIS_MODULE;
    cdev_init(&gpio_led_dev.cdev,&gpio_fops);
    ret = cdev_add(&gpio_led_dev.cdev, gpio_led_dev.devid, GPIO_LED_DEV_CNT);
    if(ret)
    {
        goto fail_cdev_add;
    }
    /*创建类*/  
    gpio_led_dev.class = class_create(THIS_MODULE,GPIO_LED_DEV_NAME);
    if(IS_ERR(gpio_led_dev.class))
    {
        ret = PTR_ERR(gpio_led_dev.class);
        goto fail_create_class;
    }
    /*创建设备*/
    gpio_led_dev.device = device_create(gpio_led_dev.class,NULL,gpio_led_dev.devid,NULL,GPIO_LED_DEV_NAME);
    if(IS_ERR(gpio_led_dev.device))
    {
        ret = PTR_ERR(gpio_led_dev.device);
        goto fail_create_device;
    }

    /*设置LED所要使用的GPIO*/
    /*获取设备树节点：gpioled*/
    gpio_led_dev.nd = of_find_node_by_path("/gpioled");
    if(gpio_led_dev.nd == NULL)
    {
        ret = -EINVAL;
        goto fail_find_node;
    }
    /*获取设备树中gpioled节点的属性编号*/
    gpio_led_dev.nd_gpio_info = of_get_named_gpio(gpio_led_dev.nd,"led-gpios",0);
    if(gpio_led_dev.nd_gpio_info<0)
    {
        ret = -EINVAL;
        goto fail_get_gpio_info;
    }
    /*申请gpio*/
    ret = gpio_request(gpio_led_dev.nd_gpio_info,"led1");
    if(ret)
    {
        printk("fail gpio request\r\n");
        goto fail_request_gpio;
    }
    /*使用IO,默认关闭LED*/
    ret = gpio_direction_output(gpio_led_dev.nd_gpio_info,1);
    if(ret<0)
    {
        goto fail_gpio_dir_out;
    }
    return 0;
fail_gpio_dir_out:
    gpio_free(gpio_led_dev.nd_gpio_info);
fail_request_gpio:

fail_get_gpio_info:

fail_find_node:
    device_destroy(gpio_led_dev.class,gpio_led_dev.devid);
fail_create_device:
    class_destroy(gpio_led_dev.class);
fail_create_class:
    cdev_del(&gpio_led_dev.cdev);
fail_cdev_add:
    unregister_chrdev_region(gpio_led_dev.devid,GPIO_LED_DEV_CNT);
fail_register_chrdev:
    return ret;
}
static void __exit gpio_led_exit(void)
{
    /*关闭LED*/
    gpio_direction_output(gpio_led_dev.nd_gpio_info,1);
    /*注销字符设备驱动*/
    cdev_del(&gpio_led_dev.cdev);
    /*释放设备号*/
    unregister_chrdev_region(gpio_led_dev.devid,GPIO_LED_DEV_CNT);
    /*摧毁设备*/
    device_destroy(gpio_led_dev.class,gpio_led_dev.devid);
    /*摧毁类*/
    class_destroy(gpio_led_dev.class);
    /*释放IO*/
    gpio_free(gpio_led_dev.nd_gpio_info);
}
module_init(gpio_led_init);
module_exit(gpio_led_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("tanminghang@goodix.com");

