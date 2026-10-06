// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * asustor_main.c - main part of asustor.ko, a platform driver for ASUSTOR NAS hardware
 *                  (the other parts are in asustor_gpl2.c which is GPL-2.0-only, and
 *                  asustor_power.c)
 *
 * Copyright (C) 2021 Mathias Fredriksson <mafredri@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blkdev.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio_keys.h>
#include <linux/i8253.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/libata.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/part_stat.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

#include "asustor_gpio_it87.h"

#define GPIO_IT87 "asustor_gpio_it87"
#define GPIO_ICH "gpio_ich"
#define GPIO_AS6100 "INT33FF:01"

#define DISK_ACT_LED(_name)                                                    \
	{                                                                      \
		.name            = _name ":green:disk",                        \
		.default_state   = LEDS_GPIO_DEFSTATE_ON,                      \
		.default_trigger = "disk-activity"                             \
	}
#define DISK_ERR_LED(_name)                                                    \
	{                                                                      \
		.name          = _name ":red:disk",                            \
		.default_state = LEDS_GPIO_DEFSTATE_OFF                        \
	}
#define NVME_ACT_LED(_name)                                                    \
	{                                                                      \
		.name          = _name ":green:disk",                          \
		.default_state = LEDS_GPIO_DEFSTATE_OFF                        \
	}
#define NVME_ERR_LED(_name)                                                    \
	{                                                                      \
		.name          = _name ":red:disk",                            \
		.default_state = LEDS_GPIO_DEFSTATE_OFF                        \
	}

// clang-format off

// ASUSTOR Leds.
// If ledtrig-blkdev ever lands, use that instead of disk-activity:
// https://lore.kernel.org/linux-leds/20210819025053.222710-1-arequipeno@gmail.com/
// Also, the "disk-activity" trigger does (currently?) *not* trigger for NVME devices.
// Power rails (and blue:lan, which is the front LAN LED rail on some devices) use
// retain_state_shutdown, so they are not switched off when asustor is unloaded
// (that would e.g. reboot the LCD's MCU) or at shutdown.
static struct gpio_led asustor_leds[] = {
	{ .name                  = "power:front_panel",                     // 0
	  .default_state         = LEDS_GPIO_DEFSTATE_ON,
	  .retain_state_shutdown = 1 },
	{ .name                  = "power:lcd",                             // 1
	  .default_state         = LEDS_GPIO_DEFSTATE_ON,
	  .retain_state_shutdown = 1 },
	{ .name = "blue:power", .default_state = LEDS_GPIO_DEFSTATE_ON },   // 2
	{ .name = "red:power", .default_state = LEDS_GPIO_DEFSTATE_OFF },   // 3
	{ .name = "green:status", .default_state = LEDS_GPIO_DEFSTATE_ON }, // 4
	{
		.name            = "red:status",                            // 5
		.default_state   = LEDS_GPIO_DEFSTATE_OFF,
		.panic_indicator = 1,
		.default_trigger = "panic",
	},
	{ .name = "blue:usb", .default_state = LEDS_GPIO_DEFSTATE_OFF },  // 6
	{ .name = "green:usb", .default_state = LEDS_GPIO_DEFSTATE_OFF }, // 7
	{ .name                  = "blue:lan",                              // 8
	  .default_state         = LEDS_GPIO_DEFSTATE_ON,
	  .retain_state_shutdown = 1 },
	DISK_ACT_LED("sata1"),                                            // 9
	DISK_ERR_LED("sata1"),                                            // 10
	DISK_ACT_LED("sata2"),                                            // 11
	DISK_ERR_LED("sata2"),                                            // 12
	DISK_ACT_LED("sata3"),                                            // 13
	DISK_ERR_LED("sata3"),                                            // 14
	DISK_ACT_LED("sata4"),                                            // 15
	DISK_ERR_LED("sata4"),                                            // 16
	DISK_ACT_LED("sata5"),                                            // 17
	DISK_ERR_LED("sata5"),                                            // 18
	DISK_ACT_LED("sata6"),                                            // 19
	DISK_ERR_LED("sata6"),                                            // 20
	NVME_ACT_LED("nvme1"),                                            // 21
	NVME_ERR_LED("nvme1"),                                            // 22
	{ .name = "red:side_inner", .default_state = LEDS_GPIO_DEFSTATE_ON }, // 23
	{ .name = "red:side_mid",   .default_state = LEDS_GPIO_DEFSTATE_ON }, // 24
	{ .name = "red:side_outer", .default_state = LEDS_GPIO_DEFSTATE_ON }, // 25
};

// not const: .gpio_blink_set is set in asustor_init()
static struct gpio_led_platform_data asustor_leds_pdata = {
	.leds     = asustor_leds,
	.num_leds = ARRAY_SIZE(asustor_leds),
};

static struct gpiod_lookup_table asustor_fs6700_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		// 0 - no front panel on this device
		// 1 - no LCD either
										// blue:power also controls the red LED
										// inside the power button on the side
		GPIO_LOOKUP_IDX(GPIO_IT87, 56, NULL,  2, GPIO_ACTIVE_LOW),	// blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,  8, NULL,  3, GPIO_ACTIVE_LOW),	// red:power
		GPIO_LOOKUP_IDX(GPIO_IT87, 31, NULL,  4, GPIO_ACTIVE_LOW),	// green:status
		GPIO_LOOKUP_IDX(GPIO_IT87, 49, NULL,  5, GPIO_ACTIVE_LOW),	// red:status
		// 6
		// 7
		GPIO_LOOKUP_IDX(GPIO_IT87, 55, NULL,  8, GPIO_ACTIVE_HIGH),	// blue:lan
		// LEDs 9 - 20 don't exist in this system
		GPIO_LOOKUP_IDX(GPIO_IT87, 12, NULL, 21, GPIO_ACTIVE_LOW),	// nvme1:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 13, NULL, 22, GPIO_ACTIVE_LOW),	// nvme1:red:disk
		// red LED strip next to the power button on the side
		GPIO_LOOKUP_IDX(GPIO_IT87, 46, NULL, 23, GPIO_ACTIVE_LOW),	// red:side_inner
		GPIO_LOOKUP_IDX(GPIO_IT87, 47, NULL, 24, GPIO_ACTIVE_LOW),	// red:side_mid
		GPIO_LOOKUP_IDX(GPIO_IT87, 52, NULL, 25, GPIO_ACTIVE_LOW),	// red:side_outer
		{}
	},
};

static struct gpiod_lookup_table asustor_as6702_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		// 0: AS6702T and AS5402T don't have a front panel to illuminate
		// 1: they don't have a LCD either
		GPIO_LOOKUP_IDX(GPIO_IT87, 56, NULL,  2, GPIO_ACTIVE_LOW),	// blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,  8, NULL,  3, GPIO_ACTIVE_LOW),	// red:power
		GPIO_LOOKUP_IDX(GPIO_IT87, 31, NULL,  4, GPIO_ACTIVE_LOW),	// green:status
		GPIO_LOOKUP_IDX(GPIO_IT87, 49, NULL,  5, GPIO_ACTIVE_LOW),	// red:status
		// 6
		GPIO_LOOKUP_IDX(GPIO_IT87, 21, NULL,  7, GPIO_ACTIVE_LOW),	// green:usb
		GPIO_LOOKUP_IDX(GPIO_IT87, 55, NULL,  8, GPIO_ACTIVE_HIGH),	// blue:lan
		GPIO_LOOKUP_IDX(GPIO_IT87, 12, NULL,  9, GPIO_ACTIVE_HIGH),	// sata1:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 13, NULL, 10, GPIO_ACTIVE_LOW),	// sata1:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 46, NULL, 11, GPIO_ACTIVE_HIGH),	// sata2:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 47, NULL, 12, GPIO_ACTIVE_LOW),	// sata2:red:disk
		{}
	},
};

static struct gpiod_lookup_table asustor_as6704_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87, 29, NULL,  0, GPIO_ACTIVE_HIGH),	// power:front_panel
		GPIO_LOOKUP_IDX(GPIO_IT87, 59, NULL,  1, GPIO_ACTIVE_HIGH),	// power:lcd
		GPIO_LOOKUP_IDX(GPIO_IT87, 56, NULL,  2, GPIO_ACTIVE_LOW),	// blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,  8, NULL,  3, GPIO_ACTIVE_LOW),	// red:power
		GPIO_LOOKUP_IDX(GPIO_IT87, 31, NULL,  4, GPIO_ACTIVE_LOW),	// green:status
		GPIO_LOOKUP_IDX(GPIO_IT87, 49, NULL,  5, GPIO_ACTIVE_LOW),	// red:status
		// 6
		GPIO_LOOKUP_IDX(GPIO_IT87, 21, NULL,  7, GPIO_ACTIVE_LOW),	// green:usb
		GPIO_LOOKUP_IDX(GPIO_IT87, 55, NULL,  8, GPIO_ACTIVE_HIGH),	// blue:lan
		GPIO_LOOKUP_IDX(GPIO_IT87, 12, NULL,  9, GPIO_ACTIVE_HIGH),	// sata1:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 13, NULL, 10, GPIO_ACTIVE_LOW),	// sata1:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 46, NULL, 11, GPIO_ACTIVE_HIGH),	// sata2:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 47, NULL, 12, GPIO_ACTIVE_LOW),	// sata2:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 51, NULL, 13, GPIO_ACTIVE_HIGH),	// sata3:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 52, NULL, 14, GPIO_ACTIVE_LOW),	// sata3:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 63, NULL, 15, GPIO_ACTIVE_HIGH),	// sata4:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 48, NULL, 16, GPIO_ACTIVE_LOW),	// sata4:red:disk
		// Do NOT add GP72 (line 50) to this or any other table: on AS6704T it is the
		// (active-low) power of the HDD backplane, for all bays. leds-gpio would claim
		// it as an output and could switch the disks off.
		{}
	},
};

static struct gpiod_lookup_table asustor_as6706_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87, 29, NULL,  0, GPIO_ACTIVE_HIGH),	// power:front_panel
		GPIO_LOOKUP_IDX(GPIO_IT87, 59, NULL,  1, GPIO_ACTIVE_HIGH),	// power:lcd
		GPIO_LOOKUP_IDX(GPIO_IT87, 56, NULL,  2, GPIO_ACTIVE_LOW),	// blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,  8, NULL,  3, GPIO_ACTIVE_LOW),	// red:power
		GPIO_LOOKUP_IDX(GPIO_IT87, 31, NULL,  4, GPIO_ACTIVE_LOW),	// green:status
		GPIO_LOOKUP_IDX(GPIO_IT87, 49, NULL,  5, GPIO_ACTIVE_LOW),	// red:status
		// 6
		GPIO_LOOKUP_IDX(GPIO_IT87, 21, NULL,  7, GPIO_ACTIVE_LOW),	// green:usb
		GPIO_LOOKUP_IDX(GPIO_IT87, 55, NULL,  8, GPIO_ACTIVE_HIGH),	// blue:lan
		GPIO_LOOKUP_IDX(GPIO_IT87, 12, NULL,  9, GPIO_ACTIVE_HIGH),	// sata1:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 13, NULL, 10, GPIO_ACTIVE_LOW),	// sata1:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 46, NULL, 11, GPIO_ACTIVE_HIGH),	// sata2:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 47, NULL, 12, GPIO_ACTIVE_LOW),	// sata2:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 51, NULL, 13, GPIO_ACTIVE_HIGH),	// sata3:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 52, NULL, 14, GPIO_ACTIVE_LOW),	// sata3:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 63, NULL, 15, GPIO_ACTIVE_HIGH),	// sata4:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 48, NULL, 16, GPIO_ACTIVE_LOW),	// sata4:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 61, NULL, 17, GPIO_ACTIVE_HIGH),	// sata5:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 62, NULL, 18, GPIO_ACTIVE_LOW),	// sata5:red:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 58, NULL, 19, GPIO_ACTIVE_HIGH),	// sata6:green:disk
		GPIO_LOOKUP_IDX(GPIO_IT87, 60, NULL, 20, GPIO_ACTIVE_LOW),	// sata6:red:disk
		{}
	},
};

static struct gpiod_lookup_table asustor_6100_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87,   29, NULL,  0, GPIO_ACTIVE_HIGH), // power:front_panel
		GPIO_LOOKUP_IDX(GPIO_IT87,   59, NULL,  1, GPIO_ACTIVE_HIGH), // power:lcd
		GPIO_LOOKUP_IDX(GPIO_IT87,   56, NULL,  2, GPIO_ACTIVE_LOW),  // blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,    8, NULL,  3, GPIO_ACTIVE_LOW),  // red:power
		GPIO_LOOKUP_IDX(GPIO_IT87,   31, NULL,  4, GPIO_ACTIVE_LOW),  // green:status
		GPIO_LOOKUP_IDX(GPIO_AS6100, 21, NULL,  5, GPIO_ACTIVE_HIGH), // red:status
		// 6
		GPIO_LOOKUP_IDX(GPIO_IT87,   21, NULL,  7, GPIO_ACTIVE_LOW),  // green:usb
		GPIO_LOOKUP_IDX(GPIO_IT87,   52, NULL,  8, GPIO_ACTIVE_HIGH), // blue:lan
		GPIO_LOOKUP_IDX(GPIO_AS6100, 24, NULL,  9, GPIO_ACTIVE_LOW),  // sata1:green:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 15, NULL, 10, GPIO_ACTIVE_HIGH), // sata1:red:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 22, NULL, 11, GPIO_ACTIVE_LOW),  // sata2:green:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 19, NULL, 12, GPIO_ACTIVE_HIGH), // sata2:red:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 25, NULL, 13, GPIO_ACTIVE_LOW),  // sata3:green:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 16, NULL, 14, GPIO_ACTIVE_HIGH), // sata3:red:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 18, NULL, 15, GPIO_ACTIVE_LOW),  // sata4:green:disk
		GPIO_LOOKUP_IDX(GPIO_AS6100, 17, NULL, 16, GPIO_ACTIVE_HIGH), // sata4:red:disk
		{}
	},
};

static struct gpiod_lookup_table asustor_600_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87, 29, NULL, 0, GPIO_ACTIVE_HIGH), // power:front_panel
		GPIO_LOOKUP_IDX(GPIO_IT87, 59, NULL, 1, GPIO_ACTIVE_HIGH), // power:lcd
		GPIO_LOOKUP_IDX(GPIO_IT87, 56, NULL, 2, GPIO_ACTIVE_LOW),  // blue:power
		GPIO_LOOKUP_IDX(GPIO_IT87,  8, NULL, 3, GPIO_ACTIVE_LOW),  // red:power
		GPIO_LOOKUP_IDX(GPIO_IT87, 31, NULL, 4, GPIO_ACTIVE_LOW),  // green:status
		GPIO_LOOKUP_IDX(GPIO_ICH,  27, NULL, 5, GPIO_ACTIVE_HIGH), // red:status
		GPIO_LOOKUP_IDX(GPIO_IT87, 21, NULL, 6, GPIO_ACTIVE_LOW),  // blue:usb
		// 7
		GPIO_LOOKUP_IDX(GPIO_IT87, 52, NULL, 8, GPIO_ACTIVE_HIGH), // blue:lan
		{}
	},
};

// The buzzer gate, see "The buzzer" below. ASUSTOR's firmware drives GP75 for
// the buzzer on all its Jasper Lake devices (AS6702T/AS6704T/AS6706T, AS54xxT,
// FS6706T/FS6712X) and on AS66xx; not tested on hardware yet.
static struct gpiod_lookup_table asustor_gp75_buzzer_lookup = {
	.dev_id = "asustor",
	.table = {
		GPIO_LOOKUP(GPIO_IT87, 53, "buzzer", GPIO_ACTIVE_HIGH),	// GP75
		{}
	},
};
// clang-format on

// ASUSTOR Buttons.
// Unfortunately, gpio-keys-polled does not use gpio lookup tables.
static struct gpio_keys_button asustor_gpio_keys_table[] = {
	{
		.desc       = "USB Copy Button",
		.code       = KEY_COPY,
		.type       = EV_KEY,
		.active_low = 1,
		.gpio       = -1, // Invalid, set in init.
	},
	{
		.desc       = "Power Button",
		.code       = KEY_POWER,
		.type       = EV_KEY,
		.active_low = 1,
		.gpio       = -1, // Invalid, set in init.
	},
};

static struct gpio_keys_platform_data asustor_keys_pdata = {
	.buttons       = asustor_gpio_keys_table,
	.nbuttons      = ARRAY_SIZE(asustor_gpio_keys_table),
	.poll_interval = 50,
	.name          = "asustor-keys",
};

// clang-format off
static struct gpiod_lookup_table asustor_fs6700_gpio_keys_lookup = {
	.dev_id = "gpio-keys-polled",
	.table = {
		// 0 (There is no USB Copy Button).
		// 1 (Power Button is already handled properly via ACPI).
		{}
	},
};

static struct gpiod_lookup_table asustor_6100_gpio_keys_lookup = { // same for 6700
	.dev_id = "gpio-keys-polled",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87, 20, NULL, 0, GPIO_ACTIVE_LOW),
		// 1 (Power Button is already handled properly via ACPI).
		{}
	},
};

static struct gpiod_lookup_table asustor_600_gpio_keys_lookup = {
	.dev_id = "gpio-keys-polled",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_IT87, 20, NULL, 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GPIO_IT87, 27, NULL, 1, GPIO_ACTIVE_LOW),
		{}
	},
};
// clang-format on

struct pci_device_match {
	// match PCI devices with the given vendorID and productID (to help identify ASUSTOR systems)
	// you can get them from `lspci -nn`, for example in
	// "00:08.0 System peripheral [0880]: Intel Corporation Device [8086:4e11]"
	// 0x8086 is the vendorID and 0x4e11 is the deviceID
	uint16_t vendorID;
	uint16_t deviceID;

	int16_t min_count; // how often that device should exist at least
	int16_t max_count; // how often that device should exist at most
};

enum {
	DEVICE_COUNT_MAX = 0x7fff // INT16_MAX - used for "no upper limit"
};

// The USB port next to the USB LED (usually the front USB port), used by the
// "asustor-front-usb" LED trigger to light the USB LED while a device is plugged
// into that port.
// A USB 3 port shows up as two root hub ports of the same USB host controller:
// one on its USB 2 root hub and one on its USB 3 root hub.
// To find the values for a device, plug a USB stick into the front port and
// look at `ls /sys/bus/usb/devices/`: e.g. "2-2" is port 2 of bus 2. Then
// `cat /sys/bus/usb/devices/usb2/speed` tells if bus 2 is the USB 3 (5000 or
// more) or the USB 2 (480) root hub, and
// `readlink /sys/bus/usb/devices/usb2-port2/peer` shows the other port.
// The host controller's PCI IDs are in `lspci -nn` (the "USB controller" line).
struct asustor_usb_led {
	// PCI vendor and device ID of the USB host controller
	uint16_t vendorID;
	uint16_t deviceID;

	uint8_t usb2_port; // port on the USB 2 root hub, 0 if none
	uint8_t usb3_port; // port on the USB 3 root hub, 0 if none
};

#define ASUSTOR_MAX_DISK_BAYS 6

// The SATA ports of the drive bays, used by the "asustor-sataN" LED triggers:
// they blink the sataN:green:disk LED only for activity of the disk in that
// bay (the "disk-activity" trigger blinks all of them for activity of any disk).
// ata_port[i] is the ATA port of the bay whose LED is "sata<i+1>", numbered
// like /sys/class/ata_port/ataX/port_no of the ports on that SATA controller
// (which is also the N in /dev/disk/by-path/pci-...-ata-N).
// To find them, read from one disk (e.g. with dd) and see which bay's LED
// blinks, then look up the disk in `ls -l /dev/disk/by-path/`.
struct asustor_disk_bays {
	// PCI vendor and device ID of the SATA controller
	uint16_t vendorID;
	uint16_t deviceID;

	uint8_t num_bays;
	uint8_t ata_port[ASUSTOR_MAX_DISK_BAYS];
};

// ASUSTOR Platform.
struct asustor_driver_data {
	const char *name; // used for force_device and for some log messages

	struct pci_device_match pci_matches[4];

	struct gpiod_lookup_table *leds;
	struct gpiod_lookup_table *keys;

	// NULL if not known for this device
	const struct asustor_usb_led *usb_led;

	// NULL if not known for this device, then all sataN:green:disk LEDs
	// use the "disk-activity" trigger
	const struct asustor_disk_bays *disk_bays;

	// the buzzer gate GPIO ("buzzer" of the asustor platform device),
	// NULL if not known for this device
	struct gpiod_lookup_table *buzzer;
};

#define VALID_OVERRIDE_NAMES                                                   \
	"AS6xx, AS61xx, AS66xx, AS6702, AS6704, AS6706, FS6706, FS6712"

// NOTE: if you add another device here, update VALID_OVERRIDE_NAMES accordingly!

/*
 * Unfortunately, AS67xx and FS67xx can't be told apart by DMI, they all identify as
 * "Intel Corporation" - "Jasper Lake Client Platform", so we need to match PCI devices.
 *
 * How to tell AS67xx and FS6xx apart:
 *
 * only AS6702T/AS5402T has [8086:4dd3] Intel Corporation Jasper Lake SATA AHCI Controller
 * (only [AF]S67xx: [8086:4dc8] Intel Corporation Jasper Lake HD Audio
 *  - but for now I think AS670xT vs AS540xT doesn't matter. Not sure if AS5404T has this; AS5402T doesn't)
 *
 * only AS6704T has [1b21:1164] ASMedia Technology Inc. ASM1164 Serial ATA AHCI Controller
 * - TODO: does AS5404T also use this? until disproven, I assume it does
 * only AS6706T has [1b21:1166] ASMedia Technology Inc. ASM1166 Serial ATA Controller
 *
 * only FS6712X has [1b21:2806] ASMedia Technology Inc. ASM2806 4-Port PCIe x2 Gen3 Packet Switch
 *              (it doesn't have any SATA controller)
 * FS6706T does not have any SATA controller and no ASMedia PCIe packet switch either
 */

static struct asustor_driver_data asustor_as6702_driver_data = {
	.name = "AS6702",
	.pci_matches = {
		// SATA controller [0106]: Intel Corporation Jasper Lake SATA AHCI Controller [8086:4dd3] (rev 01)
		// Both AS6702T and AS5402T use this SATA controller (the other devices don't)
		{ 0x8086, 0x4dd3, 1, 1 }
	},
	.leds   = &asustor_as6702_gpio_leds_lookup,
	.keys   = &asustor_6100_gpio_keys_lookup,
	.buzzer = &asustor_gp75_buzzer_lookup,
};

static const struct asustor_usb_led asustor_as6704_usb_led = {
	// USB controller: Intel Corporation Jasper Lake USB 3.1 xHCI Host Controller [8086:4ded]
	// the front USB port is port 2 of both root hubs (verified on AS6704T)
	.vendorID  = 0x8086,
	.deviceID  = 0x4ded,
	.usb2_port = 2,
	.usb3_port = 2,
};

static const struct asustor_disk_bays asustor_as6704_disk_bays = {
	// SATA controller: ASMedia Technology Inc. ASM1164 Serial ATA AHCI Controller [1b21:1164]
	// bays sata1-sata4 (left to right) are ATA ports 1-4 (verified on AS6704T)
	.vendorID = 0x1b21,
	.deviceID = 0x1164,
	.num_bays = 4,
	.ata_port = { 1, 2, 3, 4 },
};

static struct asustor_driver_data asustor_as6704_driver_data = {
	.name = "AS6704",
	.pci_matches = {
		// SATA controller: ASMedia Technology Inc. ASM1164 Serial ATA AHCI Controller [1b21:1164] (rev 02)
		// This SATA controller is used by AS6704T, and hopefully by AS5404T as well, but
		// not by any of the other AS67xx or FS67xx devices
		{ 0x1b21, 0x1164, 1, 1 }
	},
	.leds      = &asustor_as6704_gpio_leds_lookup,
	.keys      = &asustor_6100_gpio_keys_lookup,
	.usb_led   = &asustor_as6704_usb_led,
	.disk_bays = &asustor_as6704_disk_bays,
	.buzzer    = &asustor_gp75_buzzer_lookup,
};

static struct asustor_driver_data asustor_as6706_driver_data = {
	.name = "AS6706",
	.pci_matches = {
		// SATA controller [0106]: ASMedia Technology Inc. ASM1166 Serial ATA Controller [1b21:1166] (rev 02)
		// only used by AS6706T; there (currently?) is no AS5406T
		{ 0x1b21, 0x1166, 1, 1 }
		// (BTW, AS6706T also has 5x "ASM2812 6-Port PCIe x4 Gen3 Packet Switch" [1b21:2812],
		//  which thankfully is NOT the same one that FS6712 uses. Also it allows replacing the
		//  m.2 NVME slots with a 10Gbit NIC, could be that then the packet switch goes away, IDK)
	},
	.leds   = &asustor_as6706_gpio_leds_lookup,
	.keys   = &asustor_6100_gpio_keys_lookup,
	.buzzer = &asustor_gp75_buzzer_lookup,
};

static struct asustor_driver_data asustor_fs6712_driver_data = {
	.name = "FS6712",
	.pci_matches = {
		// PCI bridge: ASMedia Technology Inc. ASM2806 4-Port PCIe x2 Gen3 Packet Switch (rev 01)
		// apparently only FS6712X uses this - 15 of those turn up in lspci, at least if
		// all m.2 NVME slots have a SSD installed. I guess it's safest to match that at least
		// one of these exist; upper limit doesn't matter, so just use DEVICE_COUNT_MAX
		{ 0x1b21, 0x2806, 1, DEVICE_COUNT_MAX },
	},
	.leds   = &asustor_fs6700_gpio_leds_lookup,
	.keys   = &asustor_fs6700_gpio_keys_lookup,
	.buzzer = &asustor_gp75_buzzer_lookup,
};

static struct asustor_driver_data asustor_fs6706_driver_data = {
	.name = "FS6706",
	.pci_matches = {
		// FS6706T doesn't have that ASMedia PCI bridge / PCIe Packet switch
		{ 0x1b21, 0x2806, 0, 0 },
		// .. it doesn't have any of the SATA controllers either
		{ 0x8086, 0x4dd3, 0, 0 }, // .. not the Intel one used by AS6702T/AS5402T
		{ 0x1b21, 0x1164, 0, 0 }, // .. neither the ASMedia one used by AS6704T
		{ 0x1b21, 0x1166, 0, 0 }, // .. nor the ASMedia one used by AS6706T
	},
	.leds   = &asustor_fs6700_gpio_leds_lookup,
	.keys   = &asustor_fs6700_gpio_keys_lookup,
	.buzzer = &asustor_gp75_buzzer_lookup,
};

/*
 * It currently looks like the older systems are easier to tell apart, at least if one doesn't insist
 * on detecting the 2 vs 4 vs 6 drives versions (I only did this for AS67xx because I had to do the
 * advanced detection anyway)
 */

static struct asustor_driver_data asustor_6600_driver_data = {
	// NOTE: This is (currently?) the same as for AS6700
	//       because it seems to use the same GPIO numbers,
	//       but listed extra for the different name
	.name = "AS66xx",
	// This (and the remaining systems) don't need to match PCI devices to be detected,
	// so they're not set here (and thus initialized to all-zero)

	// the LED GPIOs are the same as in AS67xx, so use the one from AS6704 which should work for
	// both AS6602T and AS6604T (an AS66xx with more than 4 drives doesn't seem to exist)
	.leds   = &asustor_as6704_gpio_leds_lookup,
	.keys   = &asustor_6100_gpio_keys_lookup,
	.buzzer = &asustor_gp75_buzzer_lookup,
};

static struct asustor_driver_data asustor_6100_driver_data = {
	.name = "AS61xx",
	.leds = &asustor_6100_gpio_leds_lookup,
	.keys = &asustor_6100_gpio_keys_lookup,
};

static struct asustor_driver_data asustor_600_driver_data = {
	.name = "AS6xx",
	.leds = &asustor_600_gpio_leds_lookup,
	.keys = &asustor_600_gpio_keys_lookup,
};

// NOTE: Don't use this table with dmi_first_match(), because it has several entries that
//       match the same (for AS67xx and FS67xx). find_matching_asustor_system() handles
//       that by also matching PCI devices from asustor_driver_data::pci_matches.
//       This table only exists in this form (instead of just using an array of
//       asustor_driver_data) for MODULE_DEVICE_TABLE().
static const struct dmi_system_id asustor_systems[] = {
	// NOTE: each entry in this table must have its own unique asustor_driver_data
	//       (having a unique .name) set as .driver_data

	// The following devices all use the same DMI matches and are actually told apart by
	// our custom matching logic in find_matching_asustor_system() also takes
	// driver_data->pci_matches[] into account.
	// See also the bigger comment above about AS67xx vs FS67xx
	{
		// NOTE: This not only matches (and works with) AS6702T (Lockerstor Gen2),
		//       but also AS5402T (Nimbustor Gen2)
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jasper Lake Client Platform"),
		},
		.driver_data = &asustor_as6702_driver_data,
	},
	{
		// NOTE: This not only matches (and works with) AS6704T (Lockerstor Gen2),
		//       but (hopefully!) also AS5404T (Nimbustor Gen2)
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jasper Lake Client Platform"),
		},
		.driver_data = &asustor_as6704_driver_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jasper Lake Client Platform"),
		},
		.driver_data = &asustor_as6706_driver_data,
	},
	// *F*S67xx:
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jasper Lake Client Platform"),
		},
		.driver_data = &asustor_fs6706_driver_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jasper Lake Client Platform"),
		},
		.driver_data = &asustor_fs6712_driver_data,
	},

	// older devices can be matched only by DMI
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "GeminiLake"),
		},
		.driver_data = &asustor_6600_driver_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "AS61xx"),
		},
		.driver_data = &asustor_6100_driver_data,
	},
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ASUSTOR Inc."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "AS-6xxT"),
		},
		.driver_data = &asustor_600_driver_data,
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, asustor_systems);

static struct asustor_driver_data *driver_data;
static struct platform_device *asustor_leds_pdev;
static struct platform_device *asustor_keys_pdev;

// "asustor-front-usb" LED trigger, see struct asustor_usb_led
static bool asustor_usb_led_state;

// is udev plugged directly into the port next to the USB LED?
static bool asustor_usb_led_port_matches(struct usb_device *udev)
{
	const struct asustor_usb_led *ul = driver_data->usb_led;
	struct usb_hcd *hcd;
	struct pci_dev *pdev;
	uint8_t port;

	// only devices directly connected to a root hub port (not root hubs,
	// and not devices behind a hub plugged into that port)
	if (!udev->parent || udev->parent->parent)
		return false;

	hcd = bus_to_hcd(udev->bus);
	if (!hcd->self.controller || !dev_is_pci(hcd->self.controller))
		return false;
	pdev = to_pci_dev(hcd->self.controller);
	if (pdev->vendor != ul->vendorID || pdev->device != ul->deviceID)
		return false;

	port = udev->parent->speed >= USB_SPEED_SUPER ? ul->usb3_port :
	                                                ul->usb2_port;
	return port != 0 && udev->portnum == port;
}

struct asustor_usb_led_count {
	struct usb_device *ignore; // device that is being removed
	int count;
};

static int asustor_usb_led_count_dev(struct usb_device *udev, void *data)
{
	struct asustor_usb_led_count *c = data;

	if (udev != c->ignore && asustor_usb_led_port_matches(udev))
		c->count++;
	return 0;
}

static int asustor_usb_led_activate(struct led_classdev *led_cdev)
{
	led_set_brightness(led_cdev,
	                   asustor_usb_led_state ? LED_FULL : LED_OFF);
	return 0;
}

static struct led_trigger asustor_usb_led_trigger = {
	.name     = "asustor-front-usb",
	.activate = asustor_usb_led_activate,
};

// counts the devices instead of tracking add/remove events, so the state
// can't get out of sync; removed is the device that is being removed, if any
static void asustor_usb_led_update(struct usb_device *removed)
{
	struct asustor_usb_led_count c = { .ignore = removed };

	usb_for_each_dev(&c, asustor_usb_led_count_dev);
	asustor_usb_led_state = c.count > 0;
	led_trigger_event(&asustor_usb_led_trigger,
	                  asustor_usb_led_state ? LED_FULL : LED_OFF);
}

static int asustor_usb_led_notify(struct notifier_block *nb,
                                  unsigned long action, void *data)
{
	struct usb_device *udev = data;

	if (action != USB_DEVICE_ADD && action != USB_DEVICE_REMOVE)
		return NOTIFY_DONE;
	if (!asustor_usb_led_port_matches(udev))
		return NOTIFY_DONE;

	asustor_usb_led_update(action == USB_DEVICE_REMOVE ? udev : NULL);
	return NOTIFY_OK;
}

static struct notifier_block asustor_usb_led_nb = {
	.notifier_call = asustor_usb_led_notify,
};

static int __init asustor_usb_led_init(void)
{
	int ret;

	if (!driver_data->usb_led)
		return 0;

	ret = led_trigger_register(&asustor_usb_led_trigger);
	if (ret) {
		pr_err("failed registering LED trigger %s: %d\n",
		       asustor_usb_led_trigger.name, ret);
		return ret;
	}
	usb_register_notify(&asustor_usb_led_nb);
	asustor_usb_led_update(NULL);
	return 0;
}

static void asustor_usb_led_exit(void)
{
	if (!driver_data->usb_led)
		return;

	usb_unregister_notify(&asustor_usb_led_nb);
	led_trigger_unregister(&asustor_usb_led_trigger);
}

// "asustor-sataN" LED triggers, see struct asustor_disk_bays

// Writable at runtime: the poll work picks up a change within two polls.
static bool disk_led_ready = true;
module_param(disk_led_ready, bool, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(
	disk_led_ready,
	"Keep the disk LED of a bay on while a disk is in it and blink it off on "
	"activity, like ASUSTOR's firmware (default). If false, the LED is off "
	"and blinks on activity. Can be changed at runtime. Only for devices with "
	"known disk bays.");

#define ASUSTOR_DISK_BAYS_POLL_MS 100
#define ASUSTOR_DISK_BAYS_BLINK_MS 50

struct asustor_disk_bay {
	struct led_trigger trigger;
	char name[16]; // "asustor-sataN"
	bool present;
	unsigned long ios; // completed I/Os of the disk at the last poll
};

static struct asustor_disk_bay *asustor_disk_bays;
static struct pci_dev *asustor_disk_bays_pdev;
static struct delayed_work asustor_disk_bays_work;
// disk_led_ready as last seen by the poll work, only used by the poll work
static bool asustor_disk_bays_ready;
// set by the poll work when disk_led_ready changed, see there
static bool asustor_disk_bays_reapply;

static enum led_brightness
asustor_disk_bay_brightness(const struct asustor_disk_bay *bay, bool ready)
{
	return ready && READ_ONCE(bay->present) ? LED_FULL : LED_OFF;
}

static void asustor_disk_bay_blink(struct asustor_disk_bay *bay, bool ready)
{
	unsigned long delay_on  = ASUSTOR_DISK_BAYS_BLINK_MS;
	unsigned long delay_off = ASUSTOR_DISK_BAYS_BLINK_MS;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	led_trigger_blink_oneshot(&bay->trigger, delay_on, delay_off, ready);
#else
	// before 6.5 the delays were passed as pointers
	led_trigger_blink_oneshot(&bay->trigger, &delay_on, &delay_off, ready);
#endif
}

static int asustor_disk_bay_activate(struct led_classdev *led_cdev)
{
	struct asustor_disk_bay *bay = container_of(
		led_cdev->trigger, struct asustor_disk_bay, trigger);
	bool ready = READ_ONCE(disk_led_ready);

	led_set_brightness(led_cdev, asustor_disk_bay_brightness(bay, ready));
	return 0;
}

struct asustor_disk_lookup {
	unsigned int ata_port;
	bool found;
	unsigned long ios;
};

// looks for the disk on lookup->ata_port below dev, called for the devices
// below the SATA controller
static int asustor_disk_lookup_dev(struct device *dev, void *data)
{
	struct asustor_disk_lookup *lookup = data;
	struct scsi_device *sdev;
	struct ata_port *ap;

	// the disk (gendisk) is a child of the SCSI device of its ATA port
	if (!dev->type || !dev->type->name || strcmp(dev->type->name, "disk") ||
	    !dev->parent || !scsi_is_sdev_device(dev->parent))
		return device_for_each_child(dev, data,
		                             asustor_disk_lookup_dev);

	// all SCSI hosts below the SATA controller are libata ports
	sdev = to_scsi_device(dev->parent);
	ap   = ata_shost_to_port(sdev->host);
	// ATA port numbers in sysfs start at 1, ap->port_no starts at 0
	if (ap->port_no + 1 != lookup->ata_port)
		return 0;

	lookup->found = true;
	lookup->ios   = part_stat_read_accum(dev_to_disk(dev)->part0, ios);
	return 1;
}

// Polls the I/O counters instead of hooking into libata, which has no
// per-port LED trigger. Looking the disks up each time handles hot plugging.
static void asustor_disk_bays_poll(struct work_struct *work)
{
	const struct asustor_disk_bays *bays = driver_data->disk_bays;
	bool ready, changed;
	int i;

	ready   = READ_ONCE(disk_led_ready);
	changed = ready != asustor_disk_bays_ready;

	for (i = 0; i < bays->num_bays; i++) {
		struct asustor_disk_bay *bay      = &asustor_disk_bays[i];
		struct asustor_disk_lookup lookup = {
			.ata_port = bays->ata_port[i],
		};

		device_for_each_child(&asustor_disk_bays_pdev->dev, &lookup,
		                      asustor_disk_lookup_dev);

		if (changed) {
			// disk_led_ready was changed: a running blink would
			// end in the idle state of the old setting, so stop it
			// (setting LED_OFF does) and set the new idle state in
			// the next poll, once the blink is stopped
			WRITE_ONCE(bay->present, lookup.found);
			led_trigger_event(&bay->trigger, LED_OFF);
		} else if (lookup.found != bay->present ||
		           asustor_disk_bays_reapply) {
			WRITE_ONCE(bay->present, lookup.found);
			led_trigger_event(&bay->trigger,
			                  asustor_disk_bay_brightness(bay,
			                                              ready));
		} else if (lookup.found && lookup.ios != bay->ios) {
			// with disk_led_ready the LED is on, so blink it off
			asustor_disk_bay_blink(bay, ready);
		}
		bay->ios = lookup.ios;
	}
	asustor_disk_bays_ready   = ready;
	asustor_disk_bays_reapply = changed;

	schedule_delayed_work(&asustor_disk_bays_work,
	                      msecs_to_jiffies(ASUSTOR_DISK_BAYS_POLL_MS));
}

static int __init asustor_disk_bays_init(void)
{
	const struct asustor_disk_bays *bays = driver_data->disk_bays;
	int i, ret;

	if (!bays)
		return 0;

	asustor_disk_bays_pdev =
		pci_get_device(bays->vendorID, bays->deviceID, NULL);
	if (!asustor_disk_bays_pdev) {
		pr_warn("SATA controller %04x:%04x not found, using disk-activity for disk LEDs\n",
		        bays->vendorID, bays->deviceID);
		driver_data->disk_bays = NULL;
		return 0;
	}

	asustor_disk_bays =
		kcalloc(bays->num_bays, sizeof(*asustor_disk_bays), GFP_KERNEL);
	if (!asustor_disk_bays) {
		ret = -ENOMEM;
		goto err_put;
	}

	for (i = 0; i < bays->num_bays; i++) {
		struct asustor_disk_bay *bay = &asustor_disk_bays[i];

		snprintf(bay->name, sizeof(bay->name), "asustor-sata%d", i + 1);
		bay->trigger.name     = bay->name;
		bay->trigger.activate = asustor_disk_bay_activate;

		ret = led_trigger_register(&bay->trigger);
		if (ret) {
			pr_err("failed registering LED trigger %s: %d\n",
			       bay->name, ret);
			goto err_unregister;
		}
	}

	asustor_disk_bays_ready = READ_ONCE(disk_led_ready);
	INIT_DELAYED_WORK(&asustor_disk_bays_work, asustor_disk_bays_poll);
	schedule_delayed_work(&asustor_disk_bays_work, 0);
	return 0;

err_unregister:
	while (--i >= 0)
		led_trigger_unregister(&asustor_disk_bays[i].trigger);
	kfree(asustor_disk_bays);
err_put:
	pci_dev_put(asustor_disk_bays_pdev);
	return ret;
}

static void asustor_disk_bays_exit(void)
{
	int i;

	if (!driver_data->disk_bays)
		return;

	cancel_delayed_work_sync(&asustor_disk_bays_work);
	for (i = 0; i < driver_data->disk_bays->num_bays; i++)
		led_trigger_unregister(&asustor_disk_bays[i].trigger);
	kfree(asustor_disk_bays);
	pci_dev_put(asustor_disk_bays_pdev);
}

static struct platform_device *__init asustor_create_pdev(const char *name,
                                                          const void *pdata,
                                                          size_t sz)
{
	struct platform_device *pdev;

	pdev = platform_device_register_data(NULL, name, PLATFORM_DEVID_NONE,
	                                     pdata, sz);
	if (IS_ERR(pdev))
		pr_err("failed registering %s: %ld\n", name, PTR_ERR(pdev));

	return pdev;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
static int gpiochip_match_name(struct gpio_chip *chip, void *data)
{
	const char *name = data;
	return !strcmp(chip->label, name);
}

// -1 means "not found"
static int get_gpio_base_for_chipname(const char *name)
{
	struct gpio_chip *gc = gpiochip_find((void *)name, gpiochip_match_name);
	return (gc != NULL) ? gc->base : -1;
}
#else
// DG: kernel 6.7 removed gpiochip_find() and introduced gpio_device_find()
//     and friends instead

// -1 means "not found"
static int get_gpio_base_for_chipname(const char *name)
{
	int ret                 = -1;
	struct gpio_device *dev = gpio_device_find_by_label(name);
	if (dev != NULL) {
		ret = gpio_device_get_base(dev);
		// make sure to decrement the reference count of dev
		gpio_device_put(dev);
	}
	return ret;
}
#endif

// How many PCI(e) devices with given vendor/device IDs exist in this system?
static int count_pci_device_instances(unsigned int vendor, unsigned int device)
{
	int ret = 0;
	struct pci_dev *pd, *last_pd = NULL;

	while ((pd = pci_get_device(vendor, device, last_pd)) != NULL) {
		if (last_pd) {
			pci_dev_put(last_pd);
		}
		last_pd = pd;
		++ret;
	}
	if (last_pd) {
		pci_dev_put(last_pd);
	}
	return ret;
}

// check if sys->pci_matches[] match with the PCI devices in the system
// NOTE: this only checks if the devices in sys->pci_matches[] exist in the system
//       with the expected count (or the device does *not* exist if the counts are 0),
//       PCI devices existing that aren't listed in sys->pci_matches[] is expected
//       and does not make this function fail.
static bool pci_devices_match(const struct asustor_driver_data *sys)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(sys->pci_matches); ++i) {
		int dev_cnt;
		const struct pci_device_match *pdm;
		pdm = &sys->pci_matches[i];
		if (pdm->vendorID == 0 && pdm->deviceID == 0) {
			// no more entries, the previous ones matched
			// or we would've returned false already
			return true;
		}
		dev_cnt = count_pci_device_instances(pdm->vendorID,
		                                     pdm->deviceID);
		if (dev_cnt < pdm->min_count || dev_cnt > pdm->max_count) {
			return false;
		}
	}
	return true;
}

// returns true if dmi->matches match on the current system
// implemented in asustor_gpl2.c
extern bool asustor_dmi_matches(const struct dmi_system_id *dmi);

// power settings in /sys/devices/platform/asustor/, implemented in asustor_power.c
extern const struct attribute_group *asustor_power_init(void);

// find out which ASUSTOR system this is, based on asustor_systems[], including
// their linked asustor_driver_data's pci_matches
// returns NULL if this isn't a known system
static const struct dmi_system_id *find_matching_asustor_system(void)
{
	int as_idx;

	for (as_idx = 0; as_idx < ARRAY_SIZE(asustor_systems); as_idx++) {
		struct asustor_driver_data *dd;
		const struct dmi_system_id *sys;
		sys = &asustor_systems[as_idx];
		dd  = sys->driver_data;
		if (dd == NULL) {
			// no driverdata? must be at end of table
			break;
		}

		if (!asustor_dmi_matches(sys)) {
			continue;
		}

		if (!pci_devices_match(dd)) {
			continue;
		}

		// DMI and PCI devices matched => this is it
		return sys;
	}

	return NULL;
}

static char *force_device = "";
module_param(force_device, charp, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(
	force_device,
	"Don't try to detect ASUSTOR device, use the given one instead. "
	"Valid values: " VALID_OVERRIDE_NAMES);

// The buzzer.
// The buzzer of these devices is the PC speaker: PIT channel 2, switched to
// the speaker by bits 0-1 of port 0x61, which is what the kernel's pcspkr
// driver and ASUSTOR's own buzzer driver (asbuzzer_js) program. But it only
// sounds while the buzzer gate GPIO (GP75) is high: ASUSTOR's firmware sets it
// before every beep and clears it afterwards.
//
// So asustor has its own input device for the buzzer, "ASUSTOR Buzzer", whose
// event() callback plays tones like pcspkr's (asustor_buzzer_play()) and opens
// the gate while one plays. Like for any input device with EV_SND, tones come
// from:
// - EV_SND events written to its event device (e.g. by `beep -e`):
//   evdev_write() -> input_inject_event() (drivers/input/evdev.c),
// - the console bell ("\a" on a VT) and the KDMKTONE ioctl: kd_mksound()
//   sends SND_TONE (and SND_BELL to stop) to every EV_SND device the keyboard
//   handler is connected to, with input_inject_event() (kd_sound_helper(),
//   kbd_ids[] and kbd_match() in drivers/tty/vt/keyboard.c).
// The input core passes supported EV_SND events to event() right away
// (input_get_disposition() returns INPUT_PASS_TO_ALL for them, and
// input_event_dispose() calls dev->event), with dev->event_lock held and
// interrupts off. It also calls event() with value 0 on suspend and before
// hibernation's power off (input_dev_toggle() from input_dev_suspend() and
// input_dev_poweroff()), and with the previous value on resume.
// (drivers/input/input.c in Linux 6.1, 6.6, 6.12 and 6.17.)
//
// pcspkr's "PC Speaker" device still exists and plays on the same PIT channel,
// but stays silent since nothing opens the gate for it. Both drivers take
// i8253_lock for each change, so their register writes never interleave; if
// both play at once, the last change wins (a stop from either silences both).

static bool buzzer = true;
module_param(buzzer, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(
	buzzer,
	"Register the \"ASUSTOR Buzzer\" input device, which plays tones on the "
	"PC speaker and opens the buzzer gate while they play (default). If "
	"false, the buzzer gate GPIO is left alone. Only on devices with a known "
	"buzzer gate, see buzzer_gate in /sys/devices/platform/asustor/.");

// /sys/devices/platform/asustor/buzzer_gate, only on devices with a known
// buzzer gate. "active": the GPIO is claimed and the input device registered,
// "disabled": buzzer=0, "unavailable": setting it up failed (see dmesg).
static const char *asustor_buzzer_status;
static struct gpio_desc *asustor_buzzer_gpio;
static struct input_dev *asustor_buzzer_input;
static bool asustor_buzzer_tone;     // is a tone playing (gate open)?
static bool asustor_buzzer_shutdown; // rebooting or powering off: keep closed
static struct work_struct asustor_buzzer_work;

// Plays a tone with a period of count PIT ticks on the PC speaker, or stops it
// if count is 0. Exactly what pcspkr_event() in drivers/input/misc/pcspkr.c
// does (the same in Linux 6.1 to 6.17), including the locking: the PIT is
// shared with pcspkr, snd-pcsp and the kernel's PIT code, which all program
// it under i8253_lock. Like pcspkr, this doesn't request the I/O ports.
static void asustor_buzzer_play(unsigned int count)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&i8253_lock, flags);

	if (count) {
		// set command for counter 2, 2 byte write
		outb_p(0xB6, 0x43);
		// select desired HZ
		outb_p(count & 0xff, 0x42);
		outb((count >> 8) & 0xff, 0x42);
		// enable counter 2
		outb_p(inb_p(0x61) | 3, 0x61);
	} else {
		// disable counter 2
		outb(inb_p(0x61) & 0xFC, 0x61);
	}

	raw_spin_unlock_irqrestore(&i8253_lock, flags);
}

// The IT87 GPIO can sleep, so the gate is set from a work item. It sets the
// latest state, so if several tone changes come in before it runs, the last
// one wins. The gate therefore opens a little after the tone starts.
static void asustor_buzzer_work_fn(struct work_struct *work)
{
	gpiod_set_value_cansleep(asustor_buzzer_gpio,
	                         READ_ONCE(asustor_buzzer_tone) &&
	                                 !READ_ONCE(asustor_buzzer_shutdown));
}

// The input device's event() callback, see "The buzzer" above for who calls
// it. Runs in atomic context, so the gate is only queued to change.
static int asustor_buzzer_event(struct input_dev *dev, unsigned int type,
                                unsigned int code, int value)
{
	unsigned int count = 0;

	// the same as pcspkr_event(): SND_BELL is 1000 Hz, SND_TONE the given
	// frequency in Hz, 0 (or one out of range) stops the tone
	if (type != EV_SND)
		return -EINVAL;

	switch (code) {
	case SND_BELL:
		if (value)
			value = 1000;
		break;
	case SND_TONE:
		break;
	default:
		return -EINVAL;
	}

	if (value > 20 && value < 32767)
		count = PIT_TICK_RATE / value;

	asustor_buzzer_play(count);

	WRITE_ONCE(asustor_buzzer_tone, count != 0);
	schedule_work(&asustor_buzzer_work);
	return 0;
}

// Silences the buzzer before a reboot or power off, like pcspkr's shutdown()
// callback (the input core only does that for suspend and hibernation), and
// closes the gate for good: idle low, as ADM leaves it, in case the IT87
// keeps its state over a reboot. Tones played after this stay silent.
static int asustor_buzzer_reboot(struct notifier_block *nb,
                                 unsigned long action, void *data)
{
	WRITE_ONCE(asustor_buzzer_shutdown, true);
	asustor_buzzer_play(0);
	cancel_work_sync(&asustor_buzzer_work);
	gpiod_set_value_cansleep(asustor_buzzer_gpio, 0);
	return NOTIFY_DONE;
}

static struct notifier_block asustor_buzzer_reboot_nb = {
	.notifier_call = asustor_buzzer_reboot,
};

static ssize_t buzzer_gate_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", asustor_buzzer_status);
}
static DEVICE_ATTR_RO(buzzer_gate);

static struct attribute *asustor_buzzer_attrs[] = {
	&dev_attr_buzzer_gate.attr,
	NULL,
};

static const struct attribute_group asustor_buzzer_group = {
	.attrs = asustor_buzzer_attrs,
};

// Claims the buzzer gate GPIO of dev (the asustor platform device) and
// registers the "ASUSTOR Buzzer" input device as its child. Optional: if this
// fails, there is no buzzer.
static void __init asustor_buzzer_init(struct device *dev)
{
	struct input_dev *input;
	struct gpio_desc *gpio;
	int ret, base;

	if (!buzzer) {
		asustor_buzzer_status = "disabled";
		pr_info("buzzer disabled (buzzer=0)\n");
		return;
	}
	asustor_buzzer_status = "unavailable";

	gpiod_add_lookup_table(driver_data->buzzer);
	gpio = gpiod_get(dev, "buzzer", GPIOD_OUT_LOW);
	if (IS_ERR(gpio)) {
		ret  = PTR_ERR(gpio);
		base = get_gpio_base_for_chipname(GPIO_IT87);
		if (ret == -EBUSY)
			pr_warn("buzzer gate GPIO (%s line 53, gpio %d) is already in use, e.g. exported in /sys/class/gpio by a script: unexport it and reload asustor, no buzzer until then\n",
			        GPIO_IT87, base >= 0 ? base + 53 : -1);
		else if (ret == -EPROBE_DEFER)
			pr_warn("no buzzer: GPIO chip %s not found (module not loaded?)\n",
			        GPIO_IT87);
		else
			pr_warn("no buzzer: getting the buzzer gate GPIO failed: %d\n",
			        ret);
		goto err_lookup;
	}
	asustor_buzzer_gpio = gpio;
	INIT_WORK(&asustor_buzzer_work, asustor_buzzer_work_fn);

	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		goto err_input;
	}
	input->name       = "ASUSTOR Buzzer";
	input->phys       = "asustor/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = dev;
	input->evbit[0]   = BIT_MASK(EV_SND);
	input->sndbit[0]  = BIT_MASK(SND_BELL) | BIT_MASK(SND_TONE);
	input->event      = asustor_buzzer_event;

	ret = input_register_device(input);
	if (ret) {
		input_free_device(input);
		goto err_input;
	}
	asustor_buzzer_input = input;

	// can't fail (always returns 0)
	register_reboot_notifier(&asustor_buzzer_reboot_nb);

	asustor_buzzer_status = "active";
	pr_info("buzzer: \"%s\" is %s\n", input->name, dev_name(&input->dev));
	return;

err_input:
	pr_warn("no buzzer: registering the input device failed: %d\n", ret);
	gpiod_put(asustor_buzzer_gpio);
	asustor_buzzer_gpio = NULL;
err_lookup:
	gpiod_remove_lookup_table(driver_data->buzzer);
}

static void asustor_buzzer_exit(void)
{
	if (!asustor_buzzer_input)
		return;

	unregister_reboot_notifier(&asustor_buzzer_reboot_nb);
	// The input core doesn't call asustor_buzzer_event() once this returns.
	input_unregister_device(asustor_buzzer_input);
	asustor_buzzer_input = NULL;
	// turn off the speaker, like pcspkr_remove()
	asustor_buzzer_play(0);
	cancel_work_sync(&asustor_buzzer_work);
	gpiod_set_value_cansleep(asustor_buzzer_gpio, 0);
	gpiod_put(asustor_buzzer_gpio);
	asustor_buzzer_gpio = NULL;
	gpiod_remove_lookup_table(driver_data->buzzer);
}

// The asustor platform device: /sys/devices/platform/asustor/ has the power
// settings (asustor_power.c) and buzzer_gate, and it's the consumer of the
// buzzer gate GPIO and the parent of the buzzer's input device. Only created
// if the device has either.
static struct platform_device *asustor_pdev;
static const struct attribute_group *asustor_pdev_groups[3];

static int __init asustor_pdev_init(void)
{
	const struct attribute_group *power = asustor_power_init();
	struct platform_device *pdev;
	int ret, n = 0;

	if (power)
		asustor_pdev_groups[n++] = power;
	if (driver_data->buzzer)
		asustor_pdev_groups[n++] = &asustor_buzzer_group;
	if (n == 0)
		return 0;

	pdev = platform_device_alloc("asustor", PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;
	pdev->dev.groups = asustor_pdev_groups;

	// The buzzer's input device is a child of this device, so it can only be
	// registered once this device is added. Hold back the device's "add"
	// uevent until then (like device_add_disk() does), so buzzer_gate has
	// its final value when userspace learns about the device.
	dev_set_uevent_suppress(&pdev->dev, true);
	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		return ret;
	}

	if (driver_data->buzzer)
		asustor_buzzer_init(&pdev->dev);

	dev_set_uevent_suppress(&pdev->dev, false);
	kobject_uevent(&pdev->dev.kobj, KOBJ_ADD);

	asustor_pdev = pdev;
	return 0;
}

static void asustor_pdev_exit(void)
{
	asustor_buzzer_exit();
	if (asustor_pdev)
		platform_device_unregister(asustor_pdev);
	asustor_pdev = NULL;
}

static int __init asustor_init(void)
{
	const struct dmi_system_id *system;
	const struct gpiod_lookup *keys_table;
	int ret, i;

	driver_data = NULL;
	// allow overriding detection with force_device kernel parameter
	if (force_device && *force_device) {
		for (i = 0; i < ARRAY_SIZE(asustor_systems); i++) {
			struct asustor_driver_data *dd =
				asustor_systems[i].driver_data;
			if (dd && dd->name &&
			    strcmp(force_device, dd->name) == 0) {
				driver_data = dd;
				break;
			}
		}
		if (driver_data == NULL) {
			pr_err("force_device parameter set to invalid value \"%s\"!\n",
			       force_device);
			pr_info("  valid force_device values are: %s\n",
			        VALID_OVERRIDE_NAMES);
			return -EINVAL;
		}
		pr_info("force_device parameter is set to \"%s\", treating your machine as "
		        "that device instead of trying to detect it!\n",
		        force_device);
	} else { // try to detect the ASUSTOR system
		system = find_matching_asustor_system();
		if (!system) {
			pr_info("No supported ASUSTOR mainboard found");
			return -ENODEV;
		}

		driver_data = system->driver_data;

		pr_info("Found %s or similar (%s/%s)\n", driver_data->name,
		        system->matches[0].substr, system->matches[1].substr);
	}

	gpiod_add_lookup_table(driver_data->leds);
	gpiod_add_lookup_table(driver_data->keys);

	for (i = 0; i < ARRAY_SIZE(asustor_gpio_keys_table); i++) {
		// This is here simply because gpio-keys-polled does
		// not support gpio lookups.
		keys_table = driver_data->keys->table;
		for (; keys_table->key != NULL; keys_table++) {
			if (i == keys_table->idx) {
				// add the GPIO chip's base, so we get the absolute (global) gpio number
				const char *cn = keys_table->key;
				int gpio_base  = get_gpio_base_for_chipname(cn);
				if (gpio_base == -1)
					continue;
				asustor_gpio_keys_table[i].gpio =
					gpio_base + keys_table->chip_hwnum;
			}
		}
	}

	// Hardware blinking (e.g. with the "timer" trigger) for LEDs on the IT87
	// GPIO chip, see asustor_gpio_it87.h. Looked up with symbol_get() instead
	// of linking against it, so asustor.ko still builds and loads without
	// asustor_gpio_it87.ko (e.g. with DKMS); LEDs then blink in software.
	asustor_leds_pdata.gpio_blink_set = symbol_get(it87_gpio_led_blink_set);
	if (!asustor_leds_pdata.gpio_blink_set)
		pr_info("asustor_gpio_it87 not loaded, no hardware LED blinking\n");

	// Register the trigger before the LEDs, so the USB LED gets it right away.
	ret = asustor_usb_led_init();
	if (ret)
		goto err;
	if (driver_data->usb_led) {
		for (i = 0; i < ARRAY_SIZE(asustor_leds); i++) {
			if (!strcmp(asustor_leds[i].name, "blue:usb") ||
			    !strcmp(asustor_leds[i].name, "green:usb"))
				asustor_leds[i].default_trigger =
					asustor_usb_led_trigger.name;
		}
	}

	// Register the triggers before the LEDs, so the disk LEDs get them right away.
	ret = asustor_disk_bays_init();
	if (ret)
		goto err_usb_led;
	if (driver_data->disk_bays) {
		for (i = 0; i < driver_data->disk_bays->num_bays; i++) {
			char name[24];
			int j;

			snprintf(name, sizeof(name), "sata%d:green:disk",
			         i + 1);
			for (j = 0; j < ARRAY_SIZE(asustor_leds); j++) {
				if (!strcmp(asustor_leds[j].name, name))
					asustor_leds[j].default_trigger =
						asustor_disk_bays[i].name;
			}
		}
	}

	// TODO(mafredri): Handle number of disk slots -> enabled LEDs.
	asustor_leds_pdev = asustor_create_pdev(
		"leds-gpio", &asustor_leds_pdata, sizeof(asustor_leds_pdata));
	if (IS_ERR(asustor_leds_pdev)) {
		ret = PTR_ERR(asustor_leds_pdev);
		goto err_disk_bays;
	}

	asustor_keys_pdev =
		asustor_create_pdev("gpio-keys-polled", &asustor_keys_pdata,
	                            sizeof(asustor_keys_pdata));
	if (IS_ERR(asustor_keys_pdev)) {
		ret = PTR_ERR(asustor_keys_pdev);
		platform_device_unregister(asustor_leds_pdev);
		goto err_disk_bays;
	}

	// optional, so the LEDs and buttons still work if this fails
	ret = asustor_pdev_init();
	if (ret)
		pr_warn("failed registering the asustor device (power settings, buzzer gate): %d\n",
		        ret);

	return 0;

err_disk_bays:
	asustor_disk_bays_exit();
err_usb_led:
	asustor_usb_led_exit();
err:
	if (asustor_leds_pdata.gpio_blink_set)
		symbol_put(it87_gpio_led_blink_set);
	gpiod_remove_lookup_table(driver_data->leds);
	gpiod_remove_lookup_table(driver_data->keys);
	return ret;
}

static void __exit asustor_cleanup(void)
{
	asustor_pdev_exit();
	platform_device_unregister(asustor_leds_pdev);
	platform_device_unregister(asustor_keys_pdev);
	asustor_disk_bays_exit();
	asustor_usb_led_exit();

	if (asustor_leds_pdata.gpio_blink_set)
		symbol_put(it87_gpio_led_blink_set);
	gpiod_remove_lookup_table(driver_data->leds);
	gpiod_remove_lookup_table(driver_data->keys);
}

module_init(asustor_init);
module_exit(asustor_cleanup);

MODULE_AUTHOR("Mathias Fredriksson <mafredri@gmail.com>");
MODULE_DESCRIPTION("Platform driver for ASUSTOR NAS hardware");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:asustor");
MODULE_SOFTDEP("pre: asustor-it87 asustor-gpio-it87 gpio-ich"
               " platform:leds-gpio"
               " platform:gpio-keys-polled");
