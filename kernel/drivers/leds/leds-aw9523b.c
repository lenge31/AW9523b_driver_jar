/*
	The code is released under the GPL v2 protocol.
	Write by lenge.wan@megafone.co.
*/
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include "leds.h"

#define info(fmt, ...) pr_debug("lenge--<%s-%d>" fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define err(fmt, ...) pr_err("lenge--<%s-%d>" fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)

struct aw9523b_leds_device {
	struct i2c_client *i2c_dev;
	struct aw9523b_leds *aw9523b_leds_array;
	int num_leds;
	int max_current;
	int reset_gpio;
	
	spinlock_t dev_spinlock;
	struct work_struct dev_work;
};

struct aw9523b_leds {
	struct aw9523b_leds_device *aw9523b_dev;
	struct i2c_client *i2c_dev;
	struct led_classdev led_cdev;
	int brightness_reg;
	int pin;	// example:0x12 indicate pin p1_2.
	const char *trigger;
	int index;
	int isDoWork;
};

#define ID_RED	0x10
#define ID_VALUE	0x23
#define MODE_P0_LED_GPIO_REG 0x12
#define MODE_P1_LED_GPIO_REG 0x13
#define MAX_CURRENT_REG	0x11
#define MAX_LEDS	30
#define LED_NAME_PREFIX	"aw9523b_led"

static int isNeedReset_gpio = 1;
static int isAw9523bInitOk = 0;
static char system_status[20];

static struct delayed_work aw9523b_delayed_work;
enum {
	TRIGGER_START = 0,
	TRIGGER_RUNNING,
	TRIGGER_STOPING,
	TRIGGER_STOPED
};
static spinlock_t status_spinlock;
static int loopStatus = TRIGGER_START;
static int breathStatus = TRIGGER_STOPED;
static int batteryStatus = TRIGGER_STOPED;
static int voiceStatus = TRIGGER_STOPED;
static int volumeStatus = TRIGGER_STOPED;
static int volumeLevel = 0;
static int muteStatus = TRIGGER_STOPED;
static int muteStatus_OnOff = 0;
static unsigned long muteJiffies = 0;
#define VOLUME_CYCLE_COUNT	15
static int volume_cycleCount = VOLUME_CYCLE_COUNT;

enum {
	VOICE_UNKNOWN = 0,
	VOICE_LISTENING,
	VOICE_ACTIVE_LISTENING,
	VOICE_THINKING,
	VOICE_SPEAKING,
	VOICE_MIC_OFF,
	VOICE_SYSTEM_ERROR,
	VOICE_BLUE_BACKGROUND,
	VOICE_BG_BACKGROUND,
};
static int voice_status = VOICE_LISTENING;
static int focusLed = 1;

static void aw9523b_reset(struct aw9523b_leds_device *aw9523b_dev)
{
	int ret;
	
	if(aw9523b_dev->reset_gpio < 0) return;
	
	ret = gpio_direction_output(aw9523b_dev->reset_gpio, 0);
	if(ret) err("gpio_direction_output failed(%d), aw9523b_dev->reset_gpio=%d.\n", ret, aw9523b_dev->reset_gpio);
	
	udelay(200);
	
	ret = gpio_direction_output(aw9523b_dev->reset_gpio, 1);
	if(ret) err("gpio_direction_output failed(%d), aw9523b_dev->reset_gpio=%d.\n", ret, aw9523b_dev->reset_gpio);
	
	udelay(30);
}

static int aw9523b_leds_getID(struct i2c_client *i2c_dev)
{
	int id;
	id = i2c_smbus_read_byte_data(i2c_dev, ID_RED);
	if(id < 0) err("i2c_smbus_read_byte_data failed(%d), reg=0x%x.\n", id, ID_RED);
	
	return id;
}

static int aw9523b_leds_init(struct aw9523b_leds_device *aw9523b_dev)
{
	int ret;
	int count = 5;
	
	aw9523b_reset(aw9523b_dev);
	
	while(count) {
		ret = aw9523b_leds_getID(aw9523b_dev->i2c_dev);
		if (ret != ID_VALUE) err("%d:ID(%d) not match(0x%x).\n", count--, ret, ID_VALUE);
		else {
			info("ID=0x%x.\n", ret);
			break;
		}
	}
	if (ret != ID_VALUE) return ret;
	
	// P0 port led mode
	ret = i2c_smbus_write_byte_data(aw9523b_dev->i2c_dev, MODE_P0_LED_GPIO_REG, 0x00);
	if(ret) err("i2c_smbus_write_byte_data failed(%d), reg=0x%x.\n", ret, MODE_P0_LED_GPIO_REG);
	// P1 port led mode
	ret = i2c_smbus_write_byte_data(aw9523b_dev->i2c_dev, MODE_P1_LED_GPIO_REG, 0x00);
	if(ret) err("i2c_smbus_write_byte_data failed(%d), reg=0x%x.\n", ret, MODE_P1_LED_GPIO_REG);
	
	// set max current
	ret = i2c_smbus_write_byte_data(aw9523b_dev->i2c_dev, MAX_CURRENT_REG, aw9523b_dev->max_current);
	if(ret) err("i2c_smbus_write_byte_data failed(%d), reg=0x%x.\n", ret, MAX_CURRENT_REG);
	
	return ret;
}

static void aw9523b_set_brightness(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	/*i2c_master_send/i2c_smbus_write_byte_data*/
	struct aw9523b_leds *led = NULL;
	
	led_cdev->brightness = brightness;
	
	//info("system_state=%d.\n", system_state);
	
	led = container_of(led_cdev, struct aw9523b_leds, led_cdev);

	//led->isDoWork = 1;
	//schedule_work(&led->aw9523b_dev->dev_work);
	
	i2c_smbus_write_byte_data(led->i2c_dev, led->brightness_reg, led_cdev->brightness);
}
/*
static enum led_brightness aw9523b_get_brightness(struct led_classdev *led_cdev)
{
}
*/

static void dev_work_doing(struct work_struct *work)
{
	struct aw9523b_leds_device *aw9523b_dev = NULL;
	struct aw9523b_leds *aw9523b_leds_array = NULL;
	int ret = 0;
	int ii = 0;
	
	aw9523b_dev = container_of(work, struct aw9523b_leds_device, dev_work);
	if(aw9523b_dev==NULL) return;
	aw9523b_leds_array = aw9523b_dev->aw9523b_leds_array;
	if(aw9523b_leds_array==NULL) return;
	
	for(ii=0; ii<aw9523b_dev->num_leds; ii++) {
		if(aw9523b_leds_array[ii].isDoWork) {
			aw9523b_leds_array[ii].isDoWork = 0;
			ret = i2c_smbus_write_byte_data(aw9523b_leds_array[ii].i2c_dev, aw9523b_leds_array[ii].brightness_reg, aw9523b_leds_array[ii].led_cdev.brightness);
			if(ret) err("i2c_smbus_write_byte_data failed(%d), pin=0x%x, brightness_reg=0x%x.\n", ret, aw9523b_leds_array[ii].pin, aw9523b_leds_array[ii].brightness_reg);
		}
	}
}

static int aw9523b_leds_parse_child_node(struct aw9523b_leds_device *aw9523b_dev, struct aw9523b_leds *aw9523b_leds_array, struct device_node *parent_node)
{
	int index;
	struct device_node *child_node = NULL;
	struct aw9523b_leds *led = NULL;
	int ret = 0;
	
	index = -1;
	for_each_child_of_node(parent_node, child_node)
	{
		index++;
		led = &aw9523b_leds_array[index];
		led->index = index;
		led->aw9523b_dev = aw9523b_dev;
		led->i2c_dev = aw9523b_dev->i2c_dev;
		
		info("index=%d.\n", index);
		
		ret = of_property_read_string(child_node, "awinic,name", &led->led_cdev.name);
		if(ret)
		{
			err("of_property_read_string <awinic,name> failed(%d).\n", ret);
			break;
		}
		info("awinic,name=%s.\n", led->led_cdev.name);
		
		ret = of_property_read_u32(child_node, "awinic,pin", &led->pin);
		if(ret)
		{
			err("of_property_read_u32 <awinic,pin> failed(%d).\n", ret);
			break;
		}
		info("awinic,pin=0x%x.\n", led->pin);
		
		ret = of_property_read_u32(child_node, "awinic,max-brightness", &led->led_cdev.max_brightness);
		if(ret)
		{
			err("of_property_read_u32 <awinic,max-brightness> failed(%d).\n", ret);
			break;
		}
		info("awinic,max-brightness=%d.\n", led->led_cdev.max_brightness);
		
		ret = of_property_read_u32(child_node, "awinic,brightness_reg", &led->brightness_reg);
		if(ret)
		{
			err("of_property_read_u32 <awinic,brightness_reg> failed(%d).\n", ret);
			break;
		}
		info("awinic,brightness_reg=0x%x.\n", led->brightness_reg);
		
		ret = of_property_read_string(child_node, "trigger", &led->trigger);
		if(!ret) info("trigger=%s.\n", led->trigger);
		
		ret = of_property_read_string(child_node, "default_trigger", &led->led_cdev.default_trigger);
		if(!ret) info("default_trigger=%s.\n", led->led_cdev.default_trigger);
		
		led->led_cdev.brightness_set = aw9523b_set_brightness;
		//led->led_cdev.brightness_get = aw9523b_get_brightness;
		ret = led_classdev_register(&led->i2c_dev->dev, &led->led_cdev);
		if(ret)
		{
			err("led_classdev_register failed(%d).\n", ret);
			break;
		}
	}
	
	return ret;
}

static int aw9523b_app_init(void);
static int aw9523b_probe(struct i2c_client *i2c_dev, const struct i2c_device_id *id)
{
	struct aw9523b_leds_device *aw9523b_dev = NULL;
	struct aw9523b_leds *aw9523b_leds_array = NULL;
	struct device_node *of_node = NULL;
	int num_leds=0, ret=0;
	unsigned int reset_gpio = -1;

	info("i2c_dev->addr=0x%x, i2c_dev->name=%s, id->name=%s.\n", i2c_dev->addr, i2c_dev->name, id->name);
	
	of_node = i2c_dev->dev.of_node;
	if(of_node == NULL)	return -EINVAL;

	num_leds = of_get_child_count(of_node);
	info("num_leds=%d.\n", num_leds);
	if(num_leds <= 0) return -EINVAL;
	
	aw9523b_dev = devm_kzalloc(&i2c_dev->dev, sizeof(struct aw9523b_leds_device), GFP_KERNEL);
	if(aw9523b_dev == NULL)
	{
		err("devm_kzalloc aw9523b_dev failed.\n");
		return -ENOMEM;
	}
	
	aw9523b_leds_array = devm_kzalloc(&i2c_dev->dev, (sizeof(struct aw9523b_leds) * num_leds), GFP_KERNEL);
	if(aw9523b_leds_array == NULL)
	{
		err("devm_kzalloc aw9523b_leds_array failed.\n");
		devm_kfree(&i2c_dev->dev, aw9523b_dev);
		aw9523b_dev = NULL;
		return -ENOMEM;
	}
	
	spin_lock_init(&aw9523b_dev->dev_spinlock);
	INIT_WORK(&aw9523b_dev->dev_work, dev_work_doing);
	aw9523b_dev->i2c_dev = i2c_dev;
	aw9523b_dev->aw9523b_leds_array = aw9523b_leds_array;
	aw9523b_dev->num_leds = num_leds;
	aw9523b_dev->reset_gpio = -1;
	ret = of_property_read_u32(of_node, "awinic,max-current", &aw9523b_dev->max_current);
	if(ret)
	{
		err("of_property_read_u32 <awinic,max-current> failed(%d).\n", ret);
		goto free;
	}
	info("awinic,max-current=%d.\n", aw9523b_dev->max_current);
	
	reset_gpio = of_get_named_gpio_flags(of_node, "awinic,reset-gpio", 0, NULL);
	info("of_get_named_gpio_flags <awinic,reset-gpio> reset_gpio=%d.\n", reset_gpio);
	if(isNeedReset_gpio && gpio_is_valid(reset_gpio))
	{
		info("awinic,reset-gpio=%d.\n", reset_gpio);
		ret = gpio_request(reset_gpio, "leds-aw9523b_reset");
		if(ret)
		{
			err("gpio_request failed(%d).\n", ret);
			goto free;
		}
		aw9523b_dev->reset_gpio = reset_gpio;
		isNeedReset_gpio = 0;
	}
	
	ret = aw9523b_leds_init(aw9523b_dev);
	if(ret)
	{
		err("aw9523b_leds_init failed(%d).\n", ret);
		goto free;
	}
	
	
	ret = aw9523b_leds_parse_child_node(aw9523b_dev, aw9523b_leds_array, of_node);
	if(ret)
	{
		err("aw9523b_leds_parse_child_node failed(%d).\n", ret);
		goto free;
	}
	
	i2c_set_clientdata(i2c_dev, aw9523b_dev);
	
	if (isAw9523bInitOk) aw9523b_app_init();
	
	isAw9523bInitOk = 1;	
	return 0;
	
free:
	if(aw9523b_dev) cancel_work_sync(&aw9523b_dev->dev_work);
	if(aw9523b_dev && aw9523b_dev->reset_gpio>=0) gpio_free(aw9523b_dev->reset_gpio);
	if(aw9523b_dev && aw9523b_dev->aw9523b_leds_array != NULL) devm_kfree(&i2c_dev->dev, aw9523b_dev->aw9523b_leds_array);
	aw9523b_dev->aw9523b_leds_array = NULL;
	if(aw9523b_dev) devm_kfree(&i2c_dev->dev, aw9523b_dev);
	aw9523b_dev = NULL;
	
	return ret;
}

static int aw9523b_remove(struct i2c_client *i2c_dev)
{
	struct aw9523b_leds_device *aw9523b_dev = i2c_get_clientdata(i2c_dev);
	
	if(aw9523b_dev) cancel_work_sync(&aw9523b_dev->dev_work);
	if(aw9523b_dev && aw9523b_dev->reset_gpio>=0) gpio_free(aw9523b_dev->reset_gpio);
	if(aw9523b_dev && aw9523b_dev->aw9523b_leds_array != NULL) devm_kfree(&i2c_dev->dev, aw9523b_dev->aw9523b_leds_array);
	aw9523b_dev->aw9523b_leds_array = NULL;
	if(aw9523b_dev) devm_kfree(&i2c_dev->dev, aw9523b_dev);
	aw9523b_dev = NULL;
	
	return 0;
}

static void pwr_bt_clear(void);
static int aw9523b_suspend(struct i2c_client *i2c_dev, pm_message_t mesg)
{
	//int ret = 0;
	//int ii = 0;
	struct aw9523b_leds_device *aw9523b_dev = i2c_get_clientdata(i2c_dev);
	struct aw9523b_leds *aw9523b_leds_array = NULL;
	
	err("i2c_dev=0x%p, mesg.event=%d.\n", i2c_dev, mesg.event);
	
	if(aw9523b_dev==NULL) return -EINVAL;
	aw9523b_leds_array = aw9523b_dev->aw9523b_leds_array;
	if(aw9523b_leds_array==NULL) return -EINVAL;
	//for(ii=0; ii<aw9523b_dev->num_leds; ii++)
	
	cancel_delayed_work(&aw9523b_delayed_work);
	cancel_work_sync(&aw9523b_dev->dev_work);

	sprintf(system_status, "%s", "suspend");
	
	aw9523b_reset(aw9523b_dev);
	
	pwr_bt_clear();
	
	return 0;
}

static int aw9523b_resume(struct i2c_client *i2c_dev)
{
	//int ret = 0;
	//int ii = 0;
	struct aw9523b_leds_device *aw9523b_dev = i2c_get_clientdata(i2c_dev);
	struct aw9523b_leds *aw9523b_leds_array = NULL;
	
	err("i2c_dev=0x%p.\n", i2c_dev);
	
	if(aw9523b_dev==NULL) return -EINVAL;
	aw9523b_leds_array = aw9523b_dev->aw9523b_leds_array;
	if(aw9523b_leds_array==NULL) return -EINVAL;
	//for(ii=0; ii<aw9523b_dev->num_leds; ii++)
	
	sprintf(system_status, "%s", "resume");
	
	aw9523b_leds_init(aw9523b_dev);
	
	schedule_delayed_work(&aw9523b_delayed_work, msecs_to_jiffies(1500));
	
	return 0;
}

static ssize_t switch_show(struct device_driver *driver, char *buf)
{
	struct led_classdev *led_cdev = NULL;
	int ii = -1;
	
	//down_read(&leds_list_lock);
	list_for_each_entry(led_cdev, &leds_list, node) {
		if(strstr(led_cdev->name, LED_NAME_PREFIX) != NULL) {
			sprintf(buf, "%s %s", buf, led_cdev->name);
			info("ii=%d, led_cdev->name=%s.\n", ++ii, led_cdev->name);
		}
	}
	//up_read(&leds_list_lock);
	sprintf(buf, "%s\n", buf);
	
	return strlen(buf);
}
static ssize_t switch_store(struct device_driver *driver, const char *buf, size_t count)
{
	struct led_classdev *led_cdev, *leds[MAX_LEDS+1];
	int ii = -1;
	char matchStr[50]; int brightness = 0;
	
	sscanf(buf, "%s %d", matchStr, &brightness);
	info("matchStr=%s, brightness=%d.\n", matchStr, brightness);
	
	//down_read(&leds_list_lock);
	list_for_each_entry(led_cdev, &leds_list, node) {
		if(strstr(led_cdev->name, matchStr)) {
			leds[++ii] = led_cdev;
			info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
		}
	}
	//up_read(&leds_list_lock);
	
	leds[++ii] = NULL;
	
	ii = -1;
	while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
	
	return count;
}

static ssize_t system_status_show(struct device_driver *driver, char *buf)
{
	sprintf(buf, "%s\n", system_status);
	
	return strlen(buf);
}
static void aw9523b_boot_completed(void);
static ssize_t system_status_store(struct device_driver *driver, const char *buf, size_t count)
{
	sscanf(buf, "%s", system_status);
	if(!isAw9523bInitOk) return count;
	
	if(strcmp(system_status, "boot_completed")==0) aw9523b_boot_completed();
	
	return count;
}
static ssize_t trigger_show(struct device_driver *driver, char *buf)
{
	sprintf(buf, "loop,breath,battery,voice\n");
	
	return strlen(buf);
}
static ssize_t trigger_store(struct device_driver *driver, const char *buf, size_t count)
{
	char matchStr[50];
	int on_off = 0;
	
	sscanf(buf, "%s %d", matchStr, &on_off);
	
	info("matchStr=%s, on_off=%d.\n", matchStr, on_off);
	
	if(strcmp(matchStr, "loop")==0) {
		if(on_off) {
			loopStatus = TRIGGER_START;
		}
		else {
			if(loopStatus!=TRIGGER_STOPED) loopStatus=TRIGGER_STOPING;
		}
	}
	else if(strcmp(matchStr, "breath") == 0) {
		if(on_off) {
			breathStatus = TRIGGER_START;
		}
		else {
			if(breathStatus!=TRIGGER_STOPED) breathStatus=TRIGGER_STOPING;
		}
	}
	else if(strcmp(matchStr, "battery") == 0) {
		if(on_off) {
			batteryStatus = TRIGGER_START;
		}
		else {
			if(batteryStatus!=TRIGGER_STOPED) batteryStatus=TRIGGER_STOPING;
		}
	}
	else if(strcmp(matchStr, "voice") == 0) {
		if(on_off) {
			sscanf(buf, "%s %d %d %d", matchStr, &on_off, &voice_status, &focusLed);
			voiceStatus = TRIGGER_START;
		}
		else {
			if(voiceStatus!=TRIGGER_STOPED) voiceStatus=TRIGGER_STOPING;
		}
	}
	else if(strcmp(matchStr, "volume") == 0) {
		if(on_off) {
			sscanf(buf, "%s %d %d", matchStr, &on_off, &volumeLevel);
			info("matchStr=%s, on_off=%d, volumeLevel=%d.\n", matchStr, on_off, volumeLevel);
			volume_cycleCount = VOLUME_CYCLE_COUNT;
			if(volumeStatus!=TRIGGER_RUNNING) volumeStatus=TRIGGER_START;
		}
		else {
			if(volumeStatus!=TRIGGER_STOPED) volumeStatus=TRIGGER_STOPING;
		}
	}
	else if(strcmp(matchStr, "mute") == 0) {
		muteJiffies = jiffies;
		muteStatus = TRIGGER_RUNNING;
		if(on_off) {
			muteStatus_OnOff = 1;
		}
		else {
			muteStatus_OnOff = 0;
		}
	}
	
	return count;
}
static DRIVER_ATTR(switch, 0666, switch_show, switch_store);
static DRIVER_ATTR(system_status, 0666, system_status_show, system_status_store);
static DRIVER_ATTR(trigger, 0666, trigger_show, trigger_store);
static struct attribute *aw9523b_leds_attributes[] = {
	&driver_attr_switch.attr,
	&driver_attr_system_status.attr,
	&driver_attr_trigger.attr,
	NULL,
};
static struct attribute_group aw9523b_leds_attr_group = {
	//.name = "aw9523b_leds_attr_group",
	.attrs = aw9523b_leds_attributes,
};
static const struct attribute_group *aw9523b_leds_attr_groups[] = {
	&aw9523b_leds_attr_group,
	NULL,
};

static const struct of_device_id aw9523b_leds_of_match[] = {
	{ .compatible = "awinic,aw9523b_leds_3_5B" },
	{ .compatible = "awinic,aw9523b_leds_5_5B" },
	{ },
};

static const struct i2c_device_id aw9523b_leds_id_table[] = {
	{"aw9523b_leds_3_5B", 0},
	{"aw9523b_leds_5_5B", 0},
	{ },
};
MODULE_DEVICE_TABLE(i2c, aw9523b_leds_id_table);

static struct i2c_driver aw9523b_leds_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "aw9523b_leds",
		.of_match_table = aw9523b_leds_of_match,
		.groups = aw9523b_leds_attr_groups,
	},
	.probe = aw9523b_probe,
	.remove = aw9523b_remove,
	.suspend = aw9523b_suspend,
	.resume = aw9523b_resume,
	.id_table = aw9523b_leds_id_table,
};

module_i2c_driver(aw9523b_leds_driver);
/*
static int __init aw9523b_leds_module_init(void)
{
	return i2c_add_driver(&aw9523b_leds_driver);
}
late_initcall(aw9523b_leds_module_init);

static void __exit aw9523b_leds_module_exit(void)
{
	i2c_del_driver(&aw9523b_leds_driver);
}
module_exit(aw9523b_leds_module_exit);
*/

/*^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^leds driver^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^*/
/********************************************************************************/
/**********************************leds boundary*********************************/
/********************************************************************************/
/*-------------------------------- leds app-------------------------------------*/

#include <linux/keyboard.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <linux/power_supply.h>

#define LED_PWR_R	(16 + 911)
#define LED_PWR_G	(22 + 911)
#define LED_PWR_B	(17 + 911)
#define LED_BT_R	(23 + 911)
#define LED_BT_G	(33 + 911)
#define LED_BT_B	(27 + 911)

static void pwr_bt_init(void)
{
	int ret = 0;

	ret = gpio_request(LED_PWR_R, "LED_PWR_R");
	if (ret)	{
		err("gpio_request LED_PWR_R failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_PWR_R, 0);
	}

	ret = gpio_request(LED_PWR_G, "LED_PWR_G");
	if (ret)	{
		err("gpio_request LED_PWR_G failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_PWR_G, 0);
	}
	
	ret = gpio_request(LED_PWR_B, "LED_PWR_B");
	if (ret)	{
		err("gpio_request LED_PWR_B failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_PWR_B, 0);
	}

	ret = gpio_request(LED_BT_R, "LED_BT_R");
	if (ret)	{
		err("gpio_request LED_BT_R failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_BT_R, 0);
	}
	
	ret = gpio_request(LED_BT_G, "LED_BT_G");
	if (ret)	{
		err("gpio_request LED_BT_G failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_BT_G, 0);
	}
	
	ret = gpio_request(LED_BT_B, "LED_BT_B");
	if (ret)	{
		err("gpio_request LED_BT_B failed(%d).\n", ret);
	} else {
		gpio_direction_output(LED_BT_B, 0);
	}
}

static void pwr_bt_clear(void)
{
	gpio_set_value(LED_PWR_R, 0);
	gpio_set_value(LED_PWR_G, 0);
	gpio_set_value(LED_PWR_B, 0);
	gpio_set_value(LED_BT_R, 0);
	gpio_set_value(LED_BT_G, 0);
	gpio_set_value(LED_BT_B, 0);
}

#define PWR_BT_CYCLE_COUNT	2
static void pwr_bt_loop(void)
{
	static int cycleCount = 0;
	struct power_supply *power_supply_battery = NULL;
	union power_supply_propval battery_propval_status;
	union power_supply_propval battery_propval_capacity;
	static unsigned long last_jiffies = INITIAL_JIFFIES;
	struct task_struct *p = NULL;
	
	if(cycleCount++ % PWR_BT_CYCLE_COUNT) return;

	power_supply_battery = power_supply_get_by_name("battery");
	if(!power_supply_battery || 
			power_supply_battery->get_property(power_supply_battery, POWER_SUPPLY_PROP_STATUS, &battery_propval_status))
		return;
	
	switch (battery_propval_status.intval) {
		case POWER_SUPPLY_STATUS_FULL:
			gpio_set_value(LED_PWR_R, 0);
			gpio_set_value(LED_PWR_G, 1);
			gpio_set_value(LED_PWR_B, 0);
			break;
		case POWER_SUPPLY_STATUS_CHARGING:
			if(!power_supply_battery->get_property(power_supply_battery, POWER_SUPPLY_PROP_CAPACITY, &battery_propval_capacity)) {
				if (battery_propval_capacity.intval >= 95) {
					gpio_set_value(LED_PWR_R, 0);
					gpio_set_value(LED_PWR_G, 1);
					gpio_set_value(LED_PWR_B, 0);
				} else if (battery_propval_capacity.intval > 15) {
					gpio_set_value(LED_PWR_R, 1);
					gpio_set_value(LED_PWR_G, 1);
					gpio_set_value(LED_PWR_B, 0);
				} else {
					gpio_set_value(LED_PWR_R, 1);
					gpio_set_value(LED_PWR_G, 0);
					gpio_set_value(LED_PWR_B, 0);
				}
			} else {
				gpio_set_value(LED_PWR_R, 0);
				gpio_set_value(LED_PWR_G, 1);
				gpio_set_value(LED_PWR_B, 1);
			}
			break;
		default:
			gpio_set_value(LED_PWR_R, 0);
			gpio_set_value(LED_PWR_G, 0);
			gpio_set_value(LED_PWR_B, 0);
			power_supply_battery->get_property(power_supply_battery, POWER_SUPPLY_PROP_CAPACITY, &battery_propval_capacity);
			if (battery_propval_capacity.intval <= 15 && time_after(jiffies, last_jiffies + 3*HZ)) {
				last_jiffies = jiffies;
				gpio_set_value(LED_PWR_R, 1);
			}
			break;
	}

	for_each_process(p) {
		if (cycleCount == 200) info("p->comm:%s\n", p->comm);
		if (strcmp(p->comm, "dun-server") == 0) break;
	}

	if (strcmp(p->comm, "dun-server") == 0) {
		gpio_set_value(LED_BT_B, 1);
	} else {
		gpio_set_value(LED_BT_B, 0);
	}
}

#define MAX_LOOP_STAGE	6
#define LOOP_CYCLE_COUNT	2
static void loop_one(void)
{
	static int loop_stage = 0;
	static int cycleCount = 0;
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	struct aw9523b_leds *led = NULL;
	struct led_classdev *led_cdev = NULL;
	int ii = -1;
	char matchStr[50];
	int brightness = LED_FULL;

	if(cycleCount++%LOOP_CYCLE_COUNT && loopStatus!=TRIGGER_STOPING) return;
	
	switch(loopStatus) {
	case TRIGGER_START:
		loopStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		
		if(++loop_stage>MAX_LOOP_STAGE) loop_stage=1;
		sprintf(matchStr, "%s%ds", "loop", loop_stage);
		
		ii = -1;
		//down_read(&leds_list_lock);
		list_for_each_entry(led_cdev, &leds_list, node) {
			if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
			led = container_of(led_cdev, struct aw9523b_leds, led_cdev);
			if(led->trigger && strstr(led->trigger, matchStr)) {
				leds[++ii] = led_cdev;
				//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
			}
		}
		//up_read(&leds_list_lock);
		
		leds[++ii] = NULL;
		
		ii = -1;
		while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
		
		if(ii <= 0) loop_stage=0;
		
		break;
	case TRIGGER_STOPING:
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		loopStatus = TRIGGER_STOPED;
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

static unsigned char breath_steps[] = {
	0,0,3,6,9,12,15,18,21,24,27,30,34,38,42,46,50,54,60,66,72,78,84,92,102,115,125,135,145,160,180,210,255,
	255,210,180,160,145,135,125,115,102,92,84,78,72,66,60,54,50,46,42,38,34,30,27,24,21,18,15,12,9,6,3,0,0
};

static void breath_one(void)
{
	static int breath_step = -1;
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	struct aw9523b_leds *led = NULL;
	struct led_classdev *led_cdev = NULL;
	static int *restoreStatus_loop = NULL;
	int ii = -1;
	const char *matchStr = "breath";
	int brightness = LED_FULL;
	
	switch(breathStatus) {
	case TRIGGER_START:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		
		breathStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		
		if(++breath_step >= ARRAY_SIZE(breath_steps)) breath_step = 0;
		brightness = breath_steps[breath_step];
		
		ii = -1;
		//down_read(&leds_list_lock);
		list_for_each_entry(led_cdev, &leds_list, node) {
			if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
			led = container_of(led_cdev, struct aw9523b_leds, led_cdev);
			if(led->trigger && strstr(led->trigger, matchStr)) {
				leds[++ii] = led_cdev;
				//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
			}
		}
		//up_read(&leds_list_lock);
		
		leds[++ii] = NULL;
		
		ii = -1;
		while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
		break;
	case TRIGGER_STOPING:
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		breathStatus = TRIGGER_STOPED;
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

#define BATTERY_CYCLE_COUNT 3
static void batteryStatus_one(void)
{
	static int cycleCount = 0;
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	struct aw9523b_leds *led = NULL;
	struct led_classdev *led_cdev = NULL;
	static int *restoreStatus_loop = NULL;
	static int *restoreStatus_breath = NULL;
	int ii = -1;
	const char *matchStr = NULL;
	static int brightness = LED_FULL;
	static int battery_supply_status = POWER_SUPPLY_STATUS_UNKNOWN;
	static struct power_supply *power_supply_battery = NULL;
	static union power_supply_propval battery_propval_status;
	static union power_supply_propval battery_propval_capacity;
	
	if(cycleCount++%BATTERY_CYCLE_COUNT && batteryStatus!=TRIGGER_STOPED){ 
		power_supply_battery = power_supply_get_by_name("battery");
		if(!power_supply_battery || 
			power_supply_battery->get_property(power_supply_battery, POWER_SUPPLY_PROP_STATUS, &battery_propval_status))
			return;
	}
	
	switch(batteryStatus) {
	case TRIGGER_START:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		if(breathStatus == TRIGGER_RUNNING) {
			breathStatus = TRIGGER_STOPING;
			restoreStatus_breath = &breathStatus;
			break;
		}
		else if(breathStatus == TRIGGER_START) {
			breathStatus = TRIGGER_STOPED;
			restoreStatus_breath = &breathStatus;
		}
		
		batteryStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		if(battery_propval_status.intval==battery_supply_status) break;
		switch (battery_propval_status.intval) {
		case POWER_SUPPLY_STATUS_FULL:
			if(loopStatus == TRIGGER_RUNNING) {
				loopStatus = TRIGGER_STOPING;
				restoreStatus_loop = &loopStatus;
				break;
			}
			else if(loopStatus == TRIGGER_START) {
				loopStatus = TRIGGER_STOPED;
				restoreStatus_loop = &loopStatus;
			}
			if(breathStatus == TRIGGER_RUNNING) {
				breathStatus = TRIGGER_STOPING;
				restoreStatus_breath = &breathStatus;
				break;
			}
			else if(breathStatus == TRIGGER_START) {
				breathStatus = TRIGGER_STOPED;
				restoreStatus_breath = &breathStatus;
			}
			
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "battery-full";
			brightness = 100;
			
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				led = container_of(led_cdev, struct aw9523b_leds, led_cdev);
				if(led->trigger && strstr(led->trigger, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);
			
			leds[++ii] = NULL;
			
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			battery_supply_status = battery_propval_status.intval;
			break;
		case POWER_SUPPLY_STATUS_CHARGING:
			if(!power_supply_battery->get_property(power_supply_battery, POWER_SUPPLY_PROP_CAPACITY, &battery_propval_capacity))
				brightness = battery_propval_capacity.intval;
			info("brightness=%d.\n", brightness);
			if(loopStatus == TRIGGER_RUNNING) {
				loopStatus = TRIGGER_STOPING;
				restoreStatus_loop = &loopStatus;
				break;
			}
			else if(loopStatus == TRIGGER_START) {
				loopStatus = TRIGGER_STOPED;
				restoreStatus_loop = &loopStatus;
			}
			if(breathStatus == TRIGGER_RUNNING) {
				breathStatus = TRIGGER_STOPING;
				restoreStatus_breath = &breathStatus;
				break;
			}
			else if(breathStatus == TRIGGER_START) {
				breathStatus = TRIGGER_STOPED;
				restoreStatus_breath = &breathStatus;
			}
			
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "battery-charging";
			
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				led = container_of(led_cdev, struct aw9523b_leds, led_cdev);
				if(led->trigger && strstr(led->trigger, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);
			
			leds[++ii] = NULL;
			
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			battery_supply_status = battery_propval_status.intval;
			break;
		default:
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			battery_supply_status = battery_propval_status.intval;
			if(restoreStatus_loop) {
				*restoreStatus_loop = TRIGGER_START;
				restoreStatus_loop = NULL;
			}
			if(restoreStatus_breath) {
				*restoreStatus_breath = TRIGGER_START;
				restoreStatus_breath = NULL;
			}
			break;
		}
		break;
	case TRIGGER_STOPING:
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		batteryStatus = TRIGGER_STOPED;
		battery_supply_status = POWER_SUPPLY_STATUS_UNKNOWN;
		if(restoreStatus_loop) {
			*restoreStatus_loop = TRIGGER_START;
			restoreStatus_loop = NULL;
		}
		if(restoreStatus_breath) {
			*restoreStatus_breath = TRIGGER_START;
			restoreStatus_breath = NULL;
		}
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

#define BLUE_BACKGROUND_BRIGHTNESS	10
static void voiceStatus_one(void)
{
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	struct led_classdev *led_cdev = NULL;
	static int *restoreStatus_loop = NULL;
	static int *restoreStatus_breath = NULL;
	static int *restoreStatus_battery = NULL;
	int ii = -1, jj = -1, kk = -1;
	char *matchStr = NULL;
	char strBuf[50];
	int brightness = LED_FULL;
	struct led_classdev *focus_led_cdev_G = NULL;
	struct led_classdev *focus_led_cdev_B = NULL;
	struct led_classdev *focus_led_cdev_L_G = NULL;
	struct led_classdev *focus_led_cdev_L_B = NULL;
	struct led_classdev *focus_led_cdev_L_G_1 = NULL;
	struct led_classdev *focus_led_cdev_L_B_1 = NULL;
	struct led_classdev *focus_led_cdev_R_G = NULL;
	struct led_classdev *focus_led_cdev_R_B = NULL;
	struct led_classdev *focus_led_cdev_R_G_1 = NULL;
	struct led_classdev *focus_led_cdev_R_B_1 = NULL;
	static int cycleCount = 3;
	
	switch(voiceStatus) {
	case TRIGGER_START:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		if(breathStatus == TRIGGER_RUNNING) {
			breathStatus = TRIGGER_STOPING;
			restoreStatus_breath = &breathStatus;
			break;
		}
		else if(breathStatus == TRIGGER_START) {
			breathStatus = TRIGGER_STOPED;
			restoreStatus_breath = &breathStatus;
		}
		
		if(batteryStatus == TRIGGER_RUNNING) {
			batteryStatus = TRIGGER_STOPING;
			restoreStatus_battery = &batteryStatus;
			break;
		}
		else if(batteryStatus == TRIGGER_START) {
			batteryStatus = TRIGGER_STOPED;
			restoreStatus_battery = &batteryStatus;
		}

		cycleCount = 3;
		voiceStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		if(breathStatus == TRIGGER_RUNNING) {
			breathStatus = TRIGGER_STOPING;
			restoreStatus_breath = &breathStatus;
			break;
		}
		else if(breathStatus == TRIGGER_START) {
			breathStatus = TRIGGER_STOPED;
			restoreStatus_breath = &breathStatus;
		}
		
		if(batteryStatus == TRIGGER_RUNNING) {
			batteryStatus = TRIGGER_STOPING;
			restoreStatus_battery = &batteryStatus;
			break;
		}
		else if(batteryStatus == TRIGGER_START) {
			batteryStatus = TRIGGER_STOPED;
			restoreStatus_battery = &batteryStatus;
		}
		
		switch (voice_status) {
		case VOICE_LISTENING:
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			brightness = LED_FULL;
			jj = 5;
			while(--jj>=0) {
				if(focus_led_cdev_L_G) focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, LED_OFF);
				if(focus_led_cdev_R_G) focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, LED_OFF);
				if(focus_led_cdev_L_B) focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, BLUE_BACKGROUND_BRIGHTNESS);
				if(focus_led_cdev_R_B) focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, BLUE_BACKGROUND_BRIGHTNESS);
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_G = led_cdev;
						//info("jj=%d, focus_led_cdev_L_G->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_G = led_cdev;
						//info("jj=%d, focus_led_cdev_R_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_B = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_B = led_cdev;
						//info("jj=%d, focus_led_cdev_R_B->name=%s.\n", jj, led_cdev->name);
					}
				}
				if(focus_led_cdev_L_G) focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, brightness);
				if(focus_led_cdev_R_G) focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, brightness);
				if(focus_led_cdev_L_B) focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, brightness);
				if(focus_led_cdev_R_B) focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, brightness);
				//mdelay(5);
			}
			ii = -1;
			while(leds[++ii]); 
			leds[ii] = focus_led_cdev_L_G;
			leds[++ii] = NULL;

			voice_status = VOICE_UNKNOWN;
			break;
		case VOICE_ACTIVE_LISTENING:
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			brightness = LED_FULL;
			jj = 1;
			while(--jj>=0) {
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_G = led_cdev;
						//info("jj=%d, focus_led_cdev_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_B = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}
				}
				if(focus_led_cdev_G) focus_led_cdev_G->brightness_set(focus_led_cdev_G, brightness);
				if(focus_led_cdev_B) focus_led_cdev_B->brightness_set(focus_led_cdev_B, brightness);
				//mdelay(5);
			}

			jj = 3;
			while(--jj>0) {
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						if(jj==1) focus_led_cdev_L_G = led_cdev;
						if(jj==2) focus_led_cdev_L_G_1 = led_cdev;
						//info("jj=%d, focus_led_cdev_L_G->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						if(jj==1) focus_led_cdev_R_G = led_cdev;
						if(jj==2) focus_led_cdev_R_G_1 = led_cdev;
						//info("jj=%d, focus_led_cdev_R_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						if(jj==1) focus_led_cdev_L_B = led_cdev;
						if(jj==2) focus_led_cdev_L_B_1 = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						if(jj==1) focus_led_cdev_R_B = led_cdev;
						if(jj==2) focus_led_cdev_R_B_1 = led_cdev;
						//info("jj=%d, focus_led_cdev_R_B->name=%s.\n", jj, led_cdev->name);
					}
				}
			}

			jj = 3;
			while(--jj>=0) {
				brightness = LED_FULL;
				if(focus_led_cdev_L_G) focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, brightness);
				if(focus_led_cdev_R_G) focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, brightness);
				if(focus_led_cdev_L_B) focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, brightness);
				if(focus_led_cdev_R_B) focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, brightness);
				mdelay(50);
				if(focus_led_cdev_L_G_1) focus_led_cdev_L_G_1->brightness_set(focus_led_cdev_L_G_1, brightness);
				if(focus_led_cdev_R_G_1) focus_led_cdev_R_G_1->brightness_set(focus_led_cdev_R_G_1, brightness);
				if(focus_led_cdev_L_B_1) focus_led_cdev_L_B_1->brightness_set(focus_led_cdev_L_B_1, brightness);
				if(focus_led_cdev_R_B_1) focus_led_cdev_R_B_1->brightness_set(focus_led_cdev_R_B_1, brightness);
				mdelay(50);
				brightness = LED_OFF;
				if(focus_led_cdev_L_G_1) focus_led_cdev_L_G_1->brightness_set(focus_led_cdev_L_G_1, brightness);
				if(focus_led_cdev_R_G_1) focus_led_cdev_R_G_1->brightness_set(focus_led_cdev_R_G_1, brightness);
				if(focus_led_cdev_L_B_1) focus_led_cdev_L_B_1->brightness_set(focus_led_cdev_L_B_1, BLUE_BACKGROUND_BRIGHTNESS);
				if(focus_led_cdev_R_B_1) focus_led_cdev_R_B_1->brightness_set(focus_led_cdev_R_B_1, BLUE_BACKGROUND_BRIGHTNESS);
				mdelay(50);
				if(focus_led_cdev_L_G) focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, brightness);
				if(focus_led_cdev_R_G) focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, brightness);
				if(focus_led_cdev_L_B) focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, BLUE_BACKGROUND_BRIGHTNESS);
				if(focus_led_cdev_R_B) focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, BLUE_BACKGROUND_BRIGHTNESS);
				mdelay(50);
			}

			ii = -1;
			brightness = LED_FULL;
			jj = -1;
			while(++jj <= 5) {
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_G = led_cdev;
						//info("jj=%d, focus_led_cdev_L_G->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_G = led_cdev;
						//info("jj=%d, focus_led_cdev_R_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_B = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_B = led_cdev;
						//info("jj=%d, focus_led_cdev_R_B->name=%s.\n", jj, led_cdev->name);
					}
				}
				if(focus_led_cdev_L_G) {
					leds[++ii] = focus_led_cdev_L_G;
					focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, brightness);
				}
				if(focus_led_cdev_R_G) {
					leds[++ii] = focus_led_cdev_R_G;
					focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, brightness);
				}
				if(focus_led_cdev_L_B) {
					leds[++ii] = focus_led_cdev_L_B;
					focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, brightness);
				}
				if(focus_led_cdev_R_B) {
					leds[++ii] = focus_led_cdev_R_B;
					focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, brightness);
				}
				mdelay(100);
			}
			leds[++ii] = NULL;

			voice_status = VOICE_UNKNOWN;
			break;
		case VOICE_THINKING:
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			brightness = LED_FULL;
			jj = 1;
			while(--jj>=0) {
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_G = led_cdev;
						//info("jj=%d, focus_led_cdev_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_B = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}
				}
				if(focus_led_cdev_G) focus_led_cdev_G->brightness_set(focus_led_cdev_G, brightness);
				if(focus_led_cdev_B) focus_led_cdev_B->brightness_set(focus_led_cdev_B, brightness);
				//mdelay(5);
			}

			ii = -1;
			brightness = LED_FULL;
			jj = -1;
			while(++jj <= 5) {
				list_for_each_entry(led_cdev, &leds_list, node) {
					if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;

					matchStr = "_G";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_G = led_cdev;
						//info("jj=%d, focus_led_cdev_L_G->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_G = led_cdev;
						//info("jj=%d, focus_led_cdev_R_G->name=%s.\n", jj, led_cdev->name);
					}

					matchStr = "_B";
					kk = focusLed+jj;
					if(kk>10) kk=kk%10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_L_B = led_cdev;
						//info("jj=%d, focus_led_cdev_L_B->name=%s.\n", jj, led_cdev->name);
					}

					kk = focusLed-jj;
					if(kk<=0) kk=kk+10;
					sprintf(strBuf, "led%d%s", kk, matchStr);
					if(strstr(led_cdev->name, strBuf)) {
						focus_led_cdev_R_B = led_cdev;
						//info("jj=%d, focus_led_cdev_R_B->name=%s.\n", jj, led_cdev->name);
					}
				}
				if(focus_led_cdev_L_G) {
					leds[++ii] = focus_led_cdev_L_G;
					focus_led_cdev_L_G->brightness_set(focus_led_cdev_L_G, brightness);
				}
				if(focus_led_cdev_R_G) {
					leds[++ii] = focus_led_cdev_R_G;
					focus_led_cdev_R_G->brightness_set(focus_led_cdev_R_G, brightness);
				}
				if(focus_led_cdev_L_B) {
					leds[++ii] = focus_led_cdev_L_B;
					focus_led_cdev_L_B->brightness_set(focus_led_cdev_L_B, brightness);
				}
				if(focus_led_cdev_R_B) {
					leds[++ii] = focus_led_cdev_R_B;
					focus_led_cdev_R_B->brightness_set(focus_led_cdev_R_B, brightness);
				}
				mdelay(100);
			}
			leds[++ii] = NULL;
			cycleCount--;
			if (cycleCount==0) {
				cycleCount = 3;
				voice_status = VOICE_BLUE_BACKGROUND;
			}
			break;
		case VOICE_SPEAKING:
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness); 
			matchStr = "_G";
			--ii;
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds[++ii] = NULL;

#define SPEAKING_CYCLE_COUNT	10

			jj = SPEAKING_CYCLE_COUNT;
			while(jj-->0) {
				mdelay(50);
				brightness += (LED_FULL-BLUE_BACKGROUND_BRIGHTNESS)/SPEAKING_CYCLE_COUNT;
				ii = -1;
				while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			}
			jj = SPEAKING_CYCLE_COUNT;
			while(jj-->1) {
				mdelay(50);
				brightness -= (LED_FULL-BLUE_BACKGROUND_BRIGHTNESS)/SPEAKING_CYCLE_COUNT;
				ii = -1;
				while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			}

			mdelay(100);
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}

			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			mdelay(200);

			cycleCount--;
			if (cycleCount==0) {
				cycleCount = 3;
				voice_status = VOICE_BG_BACKGROUND;
			}
			break;
		case VOICE_MIC_OFF:
			matchStr = "led";
			brightness = LED_OFF;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			matchStr = "_R";
			brightness = LED_FULL;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			voice_status = VOICE_UNKNOWN;
			break;
		case VOICE_SYSTEM_ERROR:
			matchStr = "led";
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}

			matchStr = "_G";
			brightness = 0xA5;
			ii = -1;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			matchStr = "_R";
			brightness = LED_FULL;
			jj = --ii;
			//down_read(&leds_list_lock);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++jj] = led_cdev;
					//info("jj=%d, led_cdev->name=%s.\n", jj, led_cdev->name);
				}
			}
			//up_read(&leds_list_lock);	
			leds[++jj] = NULL;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

			voice_status = VOICE_UNKNOWN;
			break;
		case VOICE_BLUE_BACKGROUND:
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			brightness = BLUE_BACKGROUND_BRIGHTNESS;
			ii = -1;
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds[++ii] = NULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			voice_status = VOICE_UNKNOWN;
			break;
		case VOICE_BG_BACKGROUND:
			ii = -1;
			matchStr = "_B";
			while(leds[++ii]) if(!strstr(leds[ii]->name, matchStr)) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			
			matchStr = "_B";
			ii = -1;
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			matchStr = "_G";
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds[++ii] = NULL;
			brightness = LED_FULL;
			ii = -1;
			while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);
			voice_status = VOICE_UNKNOWN;
			break;
		default:
			cycleCount = 3;
			break;
		}
		break;
	case TRIGGER_STOPING:
		cycleCount = 3;
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		voiceStatus = TRIGGER_STOPED;
		if(restoreStatus_loop) {
			*restoreStatus_loop = TRIGGER_START;
			restoreStatus_loop = NULL;
		}
		if(restoreStatus_breath) {
			*restoreStatus_breath = TRIGGER_START;
			restoreStatus_breath = NULL;
		}
		if(restoreStatus_battery) {
			*restoreStatus_battery = TRIGGER_START;
			restoreStatus_battery = NULL;
		}
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

#define MUTE_CYCLE_COUNT	5
static void muteStatus_one(void)
{
	static int cycleCount = 0;
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	struct led_classdev *led_cdev = NULL;
	int ii = -1;
	char matchStr[50];
	int brightness = LED_FULL;
	static int tmpStatus = 0;

	if(cycleCount++%MUTE_CYCLE_COUNT && muteStatus!=TRIGGER_STOPING) return;
	
	switch(muteStatus) {
	case TRIGGER_START:
		muteStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		if (tmpStatus != muteStatus_OnOff) {
			ii = -1;
			while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
			tmpStatus = muteStatus_OnOff;
		}

		if (muteStatus_OnOff)
			sprintf(matchStr, "_R");
		else
			sprintf(matchStr, "_G");
		
		ii = -1;
		//down_read(&leds_list_lock);
		list_for_each_entry(led_cdev, &leds_list, node) {
			if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
			if(strstr(led_cdev->name, matchStr)) {
				leds[++ii] = led_cdev;
				//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
			}
		}
		//up_read(&leds_list_lock);
		
		leds[++ii] = NULL;
		
		ii = -1;
		while(leds[++ii]) leds[ii]->brightness_set(leds[ii], brightness);

		if (time_after(jiffies, muteJiffies + 2*HZ)) muteStatus = TRIGGER_STOPING;

		break;
	case TRIGGER_STOPING:
		ii = -1;
		while(leds[++ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

static void volumeStatus_one(void)
{
	static struct led_classdev *leds[MAX_LEDS+1] = {NULL};
	static struct led_classdev *leds_RGB[4] = {NULL};
	struct led_classdev *led_cdev = NULL;
	static int *restoreStatus_loop = NULL;
	static int *restoreStatus_breath = NULL;
	static int *restoreStatus_battery = NULL;
	static int *restoreStatus_voice = NULL;
	int ii = -1, jj = -1;
	char matchStr[50] = {'\0'};
	int brightness = LED_FULL;
	
	switch(volumeStatus) {
	case TRIGGER_START:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		if(breathStatus == TRIGGER_RUNNING) {
			breathStatus = TRIGGER_STOPING;
			restoreStatus_breath = &breathStatus;
			break;
		}
		else if(breathStatus == TRIGGER_START) {
			breathStatus = TRIGGER_STOPED;
			restoreStatus_breath = &breathStatus;
		}
		if(batteryStatus == TRIGGER_RUNNING) {
			batteryStatus = TRIGGER_STOPING;
			restoreStatus_battery = &batteryStatus;
			break;
		}
		else if(batteryStatus == TRIGGER_START) {
			batteryStatus = TRIGGER_STOPED;
			restoreStatus_battery = &batteryStatus;
		}
		if(voiceStatus == TRIGGER_RUNNING) {
			voiceStatus = TRIGGER_STOPING;
			restoreStatus_voice = &voiceStatus;
			break;
		}
		else if(voiceStatus == TRIGGER_START) {
			voiceStatus = TRIGGER_STOPED;
			restoreStatus_voice = &voiceStatus;
		}

		volumeStatus = TRIGGER_RUNNING;
		break;
	case TRIGGER_RUNNING:
		if(loopStatus == TRIGGER_RUNNING) {
			loopStatus = TRIGGER_STOPING;
			restoreStatus_loop = &loopStatus;
			break;
		}
		else if(loopStatus == TRIGGER_START) {
			loopStatus = TRIGGER_STOPED;
			restoreStatus_loop = &loopStatus;
		}
		if(breathStatus == TRIGGER_RUNNING) {
			breathStatus = TRIGGER_STOPING;
			restoreStatus_breath = &breathStatus;
			break;
		}
		else if(breathStatus == TRIGGER_START) {
			breathStatus = TRIGGER_STOPED;
			restoreStatus_breath = &breathStatus;
		}
		if(batteryStatus == TRIGGER_RUNNING) {
			batteryStatus = TRIGGER_STOPING;
			restoreStatus_battery = &batteryStatus;
			break;
		}
		else if(batteryStatus == TRIGGER_START) {
			batteryStatus = TRIGGER_STOPED;
			restoreStatus_battery = &batteryStatus;
		}
		if(voiceStatus == TRIGGER_RUNNING) {
			voiceStatus = TRIGGER_STOPING;
			restoreStatus_voice = &voiceStatus;
			break;
		}
		else if(voiceStatus == TRIGGER_START) {
			voiceStatus = TRIGGER_STOPED;
			restoreStatus_voice = &voiceStatus;
		}

		if(volumeLevel>10) volumeLevel=10;
		if(volumeLevel<0) volumeLevel=0;
		if(volumeLevel == 1) {
			jj = 0;
			sprintf(matchStr, "led1_B");
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds_RGB[jj++] = led_cdev;
					led_cdev->brightness_set(led_cdev, 64);
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds_RGB[jj] = NULL;
		}else if(volumeLevel == 2) {
			jj = 0;
			sprintf(matchStr, "led1_B");
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds_RGB[jj++] = led_cdev;
					led_cdev->brightness_set(led_cdev, 128);
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
			leds_RGB[jj] = NULL;
		}
		else {
			jj = -1;
			while(leds_RGB[++jj]) {leds_RGB[jj]->brightness_set(leds_RGB[jj], LED_OFF); leds_RGB[jj]=NULL;}
		}

		ii = -1;
		jj = 0;
		while(jj < volumeLevel-2) {
			sprintf(matchStr, "led%d_B", ++jj);
			list_for_each_entry(led_cdev, &leds_list, node) {
				if(strstr(led_cdev->name, LED_NAME_PREFIX)==0) continue;
				if(strstr(led_cdev->name, matchStr)) {
					leds[++ii] = led_cdev;
					//info("ii=%d, led_cdev->name=%s.\n", ii, led_cdev->name);
				}
			}
		}

		ii = -1;
		while(++ii<jj) if(leds[ii]) leds[ii]->brightness_set(leds[ii], brightness);
		ii--;
		while(++ii<ARRAY_SIZE(leds) && ii>0) if(leds[ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}

		if(volume_cycleCount--<=0) volumeStatus = TRIGGER_STOPING;
		break;
	case TRIGGER_STOPING:
		ii = -1;
		while(++ii<ARRAY_SIZE(leds)) if(leds[ii]) {leds[ii]->brightness_set(leds[ii], LED_OFF); leds[ii]=NULL;}
		jj = -1;
		while(leds_RGB[++jj]) {leds_RGB[jj]->brightness_set(leds_RGB[jj], LED_OFF); leds_RGB[jj]=NULL;}
		volumeStatus = TRIGGER_STOPED;
		if(restoreStatus_loop) {
			*restoreStatus_loop = TRIGGER_START;
			restoreStatus_loop = NULL;
		}
		if(restoreStatus_breath) {
			*restoreStatus_breath = TRIGGER_START;
			restoreStatus_breath = NULL;
		}
		if(restoreStatus_battery) {
			*restoreStatus_battery = TRIGGER_START;
			restoreStatus_battery = NULL;
		}
		if(restoreStatus_voice) {
			*restoreStatus_voice = TRIGGER_START;
			restoreStatus_voice = NULL;
		}
		break;
	case TRIGGER_STOPED:
		break;
	}
	
	return;
}

static void aw9523b_delayed_work_doing(struct work_struct *work)
{
	muteStatus_one();

	volumeStatus_one();

	voiceStatus_one();

	batteryStatus_one();

	breath_one();

	loop_one();

	pwr_bt_loop();

	schedule_delayed_work(to_delayed_work(work), msecs_to_jiffies(100));
	
	return;
}

static int aw9523b_keyboard_notifier_call(struct notifier_block *blk, unsigned long code, void *_param)
{
	struct keyboard_notifier_param *param = _param;
	int ret = NOTIFY_STOP;
	
	// code==KBD_KEYCODE, param->value==KEY_POWER, param->down==1
	info("code=0x%x, param->value=%d.\n", (unsigned int)code, param->value);
	if(param->down==1 && code==KBD_KEYCODE)
		if(param->value == KEY_POWER) {
		}
	
	return ret;
}
static int aw9523b_netlink_notifier_call(struct notifier_block *blk, unsigned long event, void *ptr)
{
	struct netlink_notify *notify = ptr;
	int ret = NOTIFY_DONE;
	
	info("event=0x%x, notify->portid=%d, notify->protocol=%d.\n", (unsigned int)event, notify->portid, notify->protocol);
	
	return ret;
}

static struct notifier_block aw9523b_keyboard_notifier_block = {
	.notifier_call = aw9523b_keyboard_notifier_call,
};

static struct notifier_block aw9523b_netlink_notifier_block = {
	.notifier_call = aw9523b_netlink_notifier_call,
};

static void aw9523b_boot_completed(void)
{
	unsigned long flags;
	
	spin_lock_irqsave(&status_spinlock, flags);
	loopStatus = TRIGGER_STOPING;
	//breathStatus = TRIGGER_START;
	//batteryStatus = TRIGGER_START;
	spin_unlock_irqrestore(&status_spinlock, flags);
}

static int aw9523b_app_init(void)
{
	info("isAw9523bInitOk=%d.\n", isAw9523bInitOk);
	spin_lock_init(&status_spinlock);
	if(!isAw9523bInitOk) return -1;
	
	pwr_bt_init();

	INIT_DELAYED_WORK(&aw9523b_delayed_work, aw9523b_delayed_work_doing);
	schedule_delayed_work(&aw9523b_delayed_work, msecs_to_jiffies(10));

	register_keyboard_notifier(&aw9523b_keyboard_notifier_block);
	netlink_register_notifier(&aw9523b_netlink_notifier_block);

	return 0;
}
/*
static void aw9523b_app_exit(void)
{
	info("enter.\n");
	return;
}

late_initcall(aw9523b_app_init);
//module_init(aw9523b_app_init);
module_exit(aw9523b_app_exit);
*/
MODULE_AUTHOR("lenge.wan@megafone.co");
MODULE_DESCRIPTION("AW9523B leds driver");
MODULE_LICENSE("GPL V2");
