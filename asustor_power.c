// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * asustor_power.c - part of asustor.ko: power settings of the IT8625E Super I/O
 *                   (behaviour after AC power loss, EuP), as sysfs attributes of
 *                   /sys/devices/platform/asustor/
 *
 * The register values are from ASUSTOR's firmware (ADM 5.1: It87_Set/Get_*
 * in libgeneraldrv and the /dev/it87 ioctls in its kernel), not from a
 * datasheet. Both ADM and the BIOS setup change the same settings.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

// Super I/O config space (the same ports the IT87 GPIO and hwmon drivers use)
#define SIO_REG 0x2e
#define SIO_VAL 0x2f

#define SIO_CONFIG_CTRL 0x02 // writing 0x02 exits the config mode
#define SIO_LDN 0x07
#define SIO_CHIPID 0x20 // 16 bit, high byte first

#define IT8625_ID 0x8625

// Logical device 4 (environment controller) registers used by ADM
#define SIO_LDN_EC 0x04
#define EC_PWR_LAST 0xf2 // bit 5: restore the last state after AC loss
#define EC_PWR_ON 0xf4   // bit 5: power on after AC loss, bit 6: cleared by ADM
#define EC_EUP 0xfa      // EuP: 0x1e on, 0x0c off
#define EC_PWR_ON_DELAY 0xfc // bits 7:5 power on delay according to /dev/it87

#define EC_PWR_LAST_BIT BIT(5)
#define EC_PWR_ON_BIT BIT(5)
#define EC_PWR_ON_BIT6 BIT(6)
#define EC_PWR_ON_DELAY_BITS 0xc0
#define EC_EUP_ON 0x1e
#define EC_EUP_OFF 0x0c

// used by asustor_main.c
int asustor_power_init(void);
void asustor_power_exit(void);

static bool allow_power_config;
module_param(allow_power_config, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(
	allow_power_config,
	"Make ac_power_resume and eup in /sys/devices/platform/asustor/ "
	"writable (default: read-only). Only on devices with an IT8625E.");

static struct platform_device *asustor_power_pdev;

// Enters the config mode. The muxed region locks out the other drivers
// using the Super I/O (asustor_gpio_it87, it87) until asustor_sio_exit().
static int asustor_sio_enter(void)
{
	if (!request_muxed_region(SIO_REG, 2, KBUILD_MODNAME))
		return -EBUSY;

	// ITE unlock sequence for the config port 0x2e
	outb(0x87, SIO_REG);
	outb(0x01, SIO_REG);
	outb(0x55, SIO_REG);
	outb(0x55, SIO_REG);
	return 0;
}

static void asustor_sio_exit(void)
{
	outb(SIO_CONFIG_CTRL, SIO_REG);
	outb(0x02, SIO_VAL);
	release_region(SIO_REG, 2);
}

static u8 asustor_sio_inb(u8 reg)
{
	outb(reg, SIO_REG);
	return inb(SIO_VAL);
}

static void asustor_sio_outb(u8 reg, u8 val)
{
	outb(reg, SIO_REG);
	outb(val, SIO_VAL);
}

static void asustor_sio_set_bits(u8 reg, u8 bits)
{
	asustor_sio_outb(reg, asustor_sio_inb(reg) | bits);
}

static void asustor_sio_clear_bits(u8 reg, u8 bits)
{
	asustor_sio_outb(reg, asustor_sio_inb(reg) & ~bits);
}

// Enters the config mode and selects LDN 4. Returns the LDN that was selected
// before (restored by asustor_sio_exit_ec()), or a negative error code.
static int asustor_sio_enter_ec(void)
{
	int ret, ldn;

	ret = asustor_sio_enter();
	if (ret)
		return ret;

	ldn = asustor_sio_inb(SIO_LDN);
	asustor_sio_outb(SIO_LDN, SIO_LDN_EC);
	return ldn;
}

static void asustor_sio_exit_ec(int ldn)
{
	asustor_sio_outb(SIO_LDN, ldn);
	asustor_sio_exit();
}

// What the device does when AC power comes back after it was lost. The names
// are inferred: ADM's enum is 0, 1, 2 (shipped default 1, its UPS shutdown
// sets 2), which by the register effects are "stay off", "last state" (F2
// bit 5) and "power on" (F4 bit 5, also sets FC bits 7:6, which ADM's /dev/it87
// calls the power on delay). Indexed by ADM's enum.
static const char *const asustor_ac_power_resume_modes[] = {
	"off",
	"last",
	"on",
};

static ssize_t ac_power_resume_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
	u8 pwr_last, pwr_on;
	int mode, ldn;

	ldn = asustor_sio_enter_ec();
	if (ldn < 0)
		return ldn;
	pwr_last = asustor_sio_inb(EC_PWR_LAST);
	pwr_on   = asustor_sio_inb(EC_PWR_ON);
	asustor_sio_exit_ec(ldn);

	// same as ADM's It87_Get_Power_Resume_Mode
	if (pwr_on & EC_PWR_ON_BIT)
		mode = 2;
	else if (pwr_last & EC_PWR_LAST_BIT)
		mode = 1;
	else
		mode = 0;

	return sysfs_emit(buf, "%s\n", asustor_ac_power_resume_modes[mode]);
}

static ssize_t ac_power_resume_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
	int mode, ldn;

	mode = sysfs_match_string(asustor_ac_power_resume_modes, buf);
	if (mode < 0)
		return mode;

	ldn = asustor_sio_enter_ec();
	if (ldn < 0)
		return ldn;

	// same writes in the same order as ADM's It87_Set_Power_Resume_Mode
	switch (mode) {
	case 0: // off
		asustor_sio_clear_bits(EC_PWR_ON, EC_PWR_ON_BIT);
		asustor_sio_clear_bits(EC_PWR_ON, EC_PWR_ON_BIT6);
		asustor_sio_clear_bits(EC_PWR_LAST, EC_PWR_LAST_BIT);
		break;
	case 1: // last
		asustor_sio_clear_bits(EC_PWR_ON, EC_PWR_ON_BIT);
		asustor_sio_clear_bits(EC_PWR_ON, EC_PWR_ON_BIT6);
		asustor_sio_set_bits(EC_PWR_LAST, EC_PWR_LAST_BIT);
		break;
	case 2: // on
		asustor_sio_set_bits(EC_PWR_ON, EC_PWR_ON_BIT);
		asustor_sio_clear_bits(EC_PWR_LAST, EC_PWR_LAST_BIT);
		asustor_sio_clear_bits(EC_PWR_ON, EC_PWR_ON_BIT6);
		asustor_sio_set_bits(EC_PWR_ON_DELAY, EC_PWR_ON_DELAY_BITS);
		break;
	}

	asustor_sio_exit_ec(ldn);
	return count;
}

// EuP ("Energy using Products") low power mode when the device is off. ADM's
// It87_Set_EuP_Mode writes the whole register.
static ssize_t eup_show(struct device *dev, struct device_attribute *attr,
                        char *buf)
{
	int ldn;
	u8 eup;

	ldn = asustor_sio_enter_ec();
	if (ldn < 0)
		return ldn;
	eup = asustor_sio_inb(EC_EUP);
	asustor_sio_exit_ec(ldn);

	if (eup == EC_EUP_ON)
		return sysfs_emit(buf, "1\n");
	if (eup == EC_EUP_OFF)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "unknown (0x%02x)\n", eup);
}

static ssize_t eup_store(struct device *dev, struct device_attribute *attr,
                         const char *buf, size_t count)
{
	bool on;
	int ret, ldn;
	u8 eup;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	ldn = asustor_sio_enter_ec();
	if (ldn < 0)
		return ldn;

	// Only overwrite the register if it holds one of the two values ADM
	// writes, as other bits in it aren't known.
	eup = asustor_sio_inb(EC_EUP);
	if (eup != EC_EUP_ON && eup != EC_EUP_OFF) {
		ret = -EIO;
		goto exit;
	}
	asustor_sio_outb(EC_EUP, on ? EC_EUP_ON : EC_EUP_OFF);
	ret = count;

exit:
	asustor_sio_exit_ec(ldn);
	return ret;
}

// Read-only unless allow_power_config is set, which picks the group at init.
static struct device_attribute asustor_ac_power_resume_ro =
	__ATTR(ac_power_resume, 0444, ac_power_resume_show, NULL);
static struct device_attribute asustor_eup_ro =
	__ATTR(eup, 0444, eup_show, NULL);
static struct device_attribute asustor_ac_power_resume_rw = __ATTR(
	ac_power_resume, 0644, ac_power_resume_show, ac_power_resume_store);
static struct device_attribute asustor_eup_rw =
	__ATTR(eup, 0644, eup_show, eup_store);

static struct attribute *asustor_power_ro_attrs[] = {
	&asustor_ac_power_resume_ro.attr,
	&asustor_eup_ro.attr,
	NULL,
};
ATTRIBUTE_GROUPS(asustor_power_ro);

static struct attribute *asustor_power_rw_attrs[] = {
	&asustor_ac_power_resume_rw.attr,
	&asustor_eup_rw.attr,
	NULL,
};
ATTRIBUTE_GROUPS(asustor_power_rw);

// Creates /sys/devices/platform/asustor/{ac_power_resume,eup} on devices
// with an IT8625E (the register meanings are only known for that chip).
int __init asustor_power_init(void)
{
	struct platform_device *pdev;
	u16 chip_id;
	int ret;

	ret = asustor_sio_enter();
	if (ret)
		return ret;
	chip_id = asustor_sio_inb(SIO_CHIPID) << 8;
	chip_id |= asustor_sio_inb(SIO_CHIPID + 1);
	asustor_sio_exit();

	if (chip_id != IT8625_ID) {
		pr_info("no IT8625E (chip ID %04x), no power settings\n",
		        chip_id);
		return 0;
	}

	pdev = platform_device_alloc("asustor", PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;
	pdev->dev.groups = allow_power_config ? asustor_power_rw_groups :
	                                        asustor_power_ro_groups;

	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		return ret;
	}

	asustor_power_pdev = pdev;
	return 0;
}

void asustor_power_exit(void)
{
	if (!asustor_power_pdev)
		return;
	platform_device_unregister(asustor_power_pdev);
	asustor_power_pdev = NULL;
}
