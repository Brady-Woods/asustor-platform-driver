# asustor-platform-driver

Linux kernel platform driver for ASUSTOR NAS hardware (leds, buttons).

On many systems, ASUSTOR uses a mix of IT87 and CPU GPIOs to control leds and buttons. Adding support for more systems should be fairly trivial, but may require some reverse engineering to figure out which GPIOs are responsible for what.

**WARNING:** Changing GPIO input/outputs (as done by this module) without knowledge of their effects can be dangerous and lead to instability, corrupted data or a broken system. **Use at your own risk.**

## About this fork

This is a fork of [mafredri/asustor-platform-driver](https://github.com/mafredri/asustor-platform-driver).
Its `main` is upstream `main` plus three changes that have been submitted
upstream but aren't merged yet. They're combined and tested together here
because the hardware they're for needs all three at once.

It runs an ASUSTOR AS6704T (LOCKERSTOR 4 Gen2) under TrueNAS SCALE, which
doesn't ship this driver. The status daemon
[truenas-asustor-chassisd](https://github.com/Brady-Woods/truenas-asustor-chassisd)
drives the front panel through it and depends on these changes.

| Change | Upstream PR | Branch here |
|---|---|---|
| Hardware LED blinking through the IT8625E's GP LED blink units, plus `asustor_gpio_it87` fixes (Simple I/O switches, re-selecting the GPIO LDN on every access, locking); removes the `gpled*` sysfs files | [#46](https://github.com/mafredri/asustor-platform-driver/pull/46) | `gpio-it87-hw-blink` |
| `asustor-front-usb` LED trigger, so the front USB LED follows the front port | [#47](https://github.com/mafredri/asustor-platform-driver/pull/47) | `front-usb-led` |
| Per-bay disk activity LED triggers (`asustor-sataN`) and the `disk_led_ready` parameter, writable at runtime | [#48](https://github.com/mafredri/asustor-platform-driver/pull/48) | `per-bay-disk-led` |

There's also a `.gitignore` for out-of-tree build artifacts.

Each change stays on its own branch for review upstream. The three PRs
conflict with each other, so `main` (also kept as `combined-46-47-48`)
resolves those conflicts once and is the version that's actually built and
tested on the hardware. Once the PRs are merged upstream, this fork can go
back to tracking upstream `main`.

## Dependencies

**Note:** The following dependencies from the mainline linux kernel are required, if they're not included by your distribution you may need to compile them yourself (note that some modules are only required on specific ASUSTOR models):

- `gpio-ich` (AS6)
- `hwmon-vid` (for the contained `asustor-it87` module)

### Optional

- `it87` (AS6, AS61, AS62, AS66XX, AS67XX, AS54XX, FS67XX)
  - This project includes a patched version of `it87` called `asustor-it87` which skips fan pwm sanity checks
    and supports more variants of IT86XX and the IT87XX chips than the kernels `it87` driver.
  - Also includes a patched version of `gpio-it87` called `asustor-gpio-it87`, which supports
    the IT8625E chip that is used in several newer ASUSTOR devices, including
    [hardware blinking](#hardware-led-blinking) of up to two LEDs.
  - May require adding `acpi_enforce_resources=lax` to kernel boot arguments for full functionality
  - Temperature monitoring (`lm-sensors`)
  - Fan speed regulation via `pwm1`
    - See [`example/fancontrol`](./example/fancontrol) for an example `/etc/fancontrol` config for a AS62 system
    - `pwm1` etc should be in `/sys/devices/platform/asustor_it87.*/hwmon/hwmon*/`
  - Front panel LED brightness adjustment via `pwm3`

## Compatibility

- AS604T
- AS6104T (NOT TESTED!)
- AS6204T
- AS6602T, AS6604T (NOT TESTED!)
- AS6702T, AS6704T, AS6706T
- AS5402T, AS5404T (NOT TESTED!)
- FS6706T, FS6712X
- .. possibly more, if they're similar enough.

The following DMI system-manufacturer / system-product-name combinations are currently supported
(see `sudo dmidecode -s system-manufacturer` and `sudo dmidecode -s system-product-name`):
* "ASUSTOR Inc." / "AS-6xxT"
    - Identified by `asustor` kernel module as **"AS6xx"**
* "Insyde" / "AS61xx"
    - Identified by `asustor` kernel module as **"AS61xx"**
* "Insyde" / "GeminiLake"
    - These are the *Lockerstor* AS66xxT devices, like AS6604T
        - *maybe also others like Nimbustor AS520xT?*
    - Identified by `asustor` kernel module as **"AS66xx"**
* "Intel Corporation" / "Jasper Lake Client Platform"
    - These are the *Lockerstor Gen2* AS67xxT (AS6702T etc), *Nimbustor Gen2* AS54xxT (AS5402T etc)
      and *Flashstor* FS6706T/FS6712X devices.
    - Identified by `asustor` kernel module as:
        * **"AS6702"** for *Lockerstor Gen2* and *Nimbustor Gen2* with *two* SATA drives (AS6702T, AS5402T)
        * **"AS6704"** for *Lockerstor Gen2* and *Nimbustor Gen2* with *four* SATA drives (AS6704T, AS5404T)
        * **"AS6706"** for *Lockerstor Gen2* with *six* SATA drives (AS6706T)
        * **"FS6706"** for *Flashtor* with *six* slots for m.2 NVME SSDs (FS6706T)
        * **"FS6712"** for *Flashtor* with *twelve* slots for m.2 NVME SSDs (FS6712X)

## Features

- LEDs (front panel, disk)
  - Represented as subdirectories in `/sys/class/leds/`
    - In the subdirectories, you can set `brightness` to 0 or 1 to switch the LED off or on
      (for example `echo 1 | sudo tee blue:power/brightness`). Similarly, the `trigger` can
      also be configured, see [below](#set-triggers-for-leds).
    - Sometimes the name of an LED doesn't exactly represent its color, for example, on the
      *Flashstor FS6712X*, the `blue:lan` LED is actually purple when connected with 10GBit
      (but blue when connected with 1GBit). Also, sometimes two LEDs physically appear as one, so
      enabling both will create a third color (e.g. if both `nvme1:green` and `nvme1:red` are enabled,
      it will look orange).
  - See [asustor_main.c](asustor_main.c).
- Buttons
  - USB Copy Button
  - Power Button (AS6)
- Power (`/sys/class/leds/power:*`)
  - LCD
  - Front panel
- Buzzer (AS66xx, AS67xx, AS54xx, FS67xx) as the "ASUSTOR Buzzer" input device, see [below](#buzzer)
- Power settings (AC power loss, EuP), see [below](#power-settings-ac-power-loss-and-eup)

## Installation

```
git clone https://github.com/mafredri/asustor-platform-driver
cd asustor-platform-driver
make
sudo make install
```

### NixOS
Include the platform drivers in your `flake.nix` as follows:
```
{
  inputs.asustor-platform-driver.url = "github:mafredri/asustor-platform-driver";
  # optional, not necessary for the module
  #inputs.asustor-platform-driver.inputs.nixpkgs.follows = "nixpkgs";

  outputs = { self, nixpkgs, asustor-platform-driver }: {
    # change `yourhostname` to your actual hostname
    nixosConfigurations.yourhostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        asustor-platform-driver.nixosModules.default
        { hardware.asustor.enable = true; }
      ];
    };
  };
}
```

## Tips

### Hardware LED blinking

The firmware leaves the green status LED blinking (in hardware) after boot. `asustor-gpio-it87`
stops that when the LEDs are set up, so the status LED is simply on once the driver is loaded.

On devices with the IT8625E chip, `asustor-gpio-it87` can also let the chip itself blink LEDs,
using the kernel's `timer` LED trigger (see also [below](#set-triggers-for-leds)):
```
sudo modprobe ledtrig-timer
echo timer | sudo tee /sys/class/leds/green\:status/trigger
```
By default this blinks 500ms on, 500ms off. Other times (in milliseconds) can be set with
`delay_on` and `delay_off`, for example 3s on, 1s off:
```
echo 3000 | sudo tee /sys/class/leds/green\:status/delay_on
echo 1000 | sudo tee /sys/class/leds/green\:status/delay_off
```

The chip supports the following on/off times, which are from ASUSTOR's GPL source:
* **125** / **125**, **250** / **250**, **500** / **500**, **1000** / **1000**, **2000** / **2000**, **4000** / **4000**
* **1000** / **3000** and **3000** / **1000**
* **2000** / **6000** and **6000** / **2000**
* **2000** / **500** for LEDs that are lit when their GPIO is low (like `green:status`),
  or **500** / **2000** for LEDs that are lit when their GPIO is high (like `sata1:green:disk`)

Only LEDs on GPIO pins `it87_gp10` to `it87_gp57` can blink in hardware (see
`sudo cat /sys/kernel/debug/gpio` for which pin an LED uses), and at most two at a time.
Otherwise, or with other times, the kernel blinks the LED in software instead, which works just
as well but needs the CPU.

*Note:* Older versions of this project had `gpled1_blink` and `gpled1_blink_freq` (and `gpled2_*`)
in `/sys/devices/platform/asustor_it87.*/hwmon/hwmon*/` for this, those have been removed.

### Set triggers for LEDs

Linux allows controlling LEDs with "triggers", which means that they will blink on specific events.
By default, the trigger is "none" (which means "always on") for most LEDs, but there are others that
you may enable (likely in a script that's run after boot), for example:
```
# make green USB LED blink on USB traffic
echo usb-host > /sys/class/leds/green\:usb/trigger
# make LAN led light up if the first network link is up:
echo r8169-0-200:00:link > /sys/class/leds/blue\:lan
```

`cat /sys/class/leds/green\:usb/trigger` will list the available triggers, with the currently used
one being marked with square brackes (e.g. `[none]  kbd-scrolllock kbd-numlock kbd-capslock ...`).

On devices where the front USB port is known (currently AS6704T, see `struct asustor_usb_led` in
[asustor_main.c](asustor_main.c) for how to add others), the USB LED uses the `asustor-front-usb`
trigger by default, which lights it while a USB device is plugged into the front port.

On devices where the SATA ports of the drive bays are known (currently AS6704T, see
`struct asustor_disk_bays` in [asustor_main.c](asustor_main.c) for how to add others), each
`sataN:green:disk` LED uses its own `asustor-sataN` trigger by default: like with ASUSTOR's
firmware, the LED is on while a disk is in the bay and blinks off when that disk is accessed.
With the `disk_led_ready=0` module parameter, the LED is off and blinks on when the disk is accessed.
The parameter can also be changed at runtime, which takes effect within a fraction of a second:
`echo 0 | sudo tee /sys/module/asustor/parameters/disk_led_ready` (`1` to switch back).
On other devices, all disk LEDs use `disk-activity`, which blinks them all for activity of any disk.

Note that currently the disk-related triggers (like `disk-activity`) do **not** work with NVME drives.
That's a general limitation of the Linux kernel that is independent of this project.
If this feature is ever implemented in the kernel, it will automatically work with this driver.

### Buzzer

The buzzer is the PC speaker (PIT channel 2, switched on through port 0x61), but it only sounds
while an IT87 GPIO (GP75, the "buzzer gate") is high: ASUSTOR's firmware sets it before every beep
and clears it afterwards. The `asustor` module claims that GPIO (low while idle) and registers its
own input device for the buzzer, **"ASUSTOR Buzzer"**, which plays tones on the PC speaker exactly
like the kernel's `pcspkr` driver and opens the gate while a tone plays. `pcspkr` isn't needed.

To beep, send `EV_SND` events to its event device. It's `/dev/input/by-path/platform-asustor-event`
(from udev's `60-persistent-input.rules`), or find it by name:
```sh
grep -l '^ASUSTOR Buzzer$' /sys/class/input/event*/device/name   # .../eventN/device/name -> /dev/input/eventN
```
- With the `beep` tool: `beep -e /dev/input/by-path/platform-asustor-event -f 2000 -l 200`.
  ASUSTOR's firmware beeps at about 2 kHz, 200 ms for a short and 800 ms for a long beep.
- Or write `struct input_event`s yourself: type `EV_SND` (0x12), code `SND_TONE` (0x02) and the
  frequency in Hz as value (21 to 32766) starts a tone, value 0 stops it (`SND_BELL`, 0x01, with
  value 1 plays 1000 Hz). No `SYN_REPORT` is needed. For example in Python:
  ```python
  import struct, time
  def tone(hz): return struct.pack("llHHi", 0, 0, 0x12, 0x02, hz)  # 64-bit struct input_event
  with open("/dev/input/by-path/platform-asustor-event", "wb", buffering=0) as f:
      f.write(tone(2000)); time.sleep(0.2); f.write(tone(0))
  ```
- The console bell (`printf '\a'` on a virtual console, or the `KDMKTONE` ioctl) beeps too: the
  kernel sends it to every input device that can play sounds.

`/sys/devices/platform/asustor/buzzer_gate` tells whether the buzzer works. It only exists on
devices with a known buzzer gate (and with versions of the driver that have it), and reads:
- `active`: the gate GPIO is claimed and the "ASUSTOR Buzzer" input device is registered,
- `disabled`: the module parameter `buzzer=0` is set: no input device, the gate GPIO is left alone,
- `unavailable`: setting it up failed, `dmesg` says why. Usually the GPIO is in use already
  (`-EBUSY`), e.g. exported through `/sys/class/gpio` by a script: unexport it
  (`echo <gpio> | sudo tee /sys/class/gpio/unexport`, the number is in the message) and
  reload `asustor`.

The module parameter `buzzer` (default `1`) can be set to `0` to leave the buzzer alone, e.g. with
`options asustor buzzer=0` in `/etc/modprobe.d/asustor.conf`.

Notes:
- The console bell is audible now. `setterm --blength 0` (run on that console) silences it for a
  virtual console, or load `asustor` with `buzzer=0`.
- If `pcspkr` is loaded, its "PC Speaker" input device still exists, but stays silent: the gate
  only opens for "ASUSTOR Buzzer". Both drive the same PIT channel (under the kernel's
  `i8253_lock`, so they never mix up its registers): if both play at once, the last tone change
  wins, and either one stopping its tone stops both.
- The gate GPIO is set from a work item (the IT87 GPIO driver can sleep), so it opens a fraction
  of a millisecond to a few milliseconds after the tone starts; very short tones may be cut short
  or not sound at all.
- The tone stops and the gate closes when `asustor` is unloaded and before a reboot or power off;
  on suspend the kernel stops the tone.
- Not tested on hardware yet. GP75 and its polarity are from ASUSTOR's firmware, which drives it on
  all its Jasper Lake devices and on AS66xx.

### Power settings: AC power loss and EuP

On devices with the IT8625E chip (e.g. AS6704T), the driver shows two power settings
that are otherwise only in the BIOS setup (and ADM), in `/sys/devices/platform/asustor/`:

- `ac_power_resume`: what the device does when AC power comes back after it was lost:
  `off` (stay off), `last` (restore the state from before the power loss) or `on` (always power on).
  `on` also sets bits of a "power on delay" field, like ASUSTOR's firmware does.
- `eup`: the EuP (low power when off) mode, `1` (on) or `0` (off). If the register holds a value
  other than the two ASUSTOR's firmware uses, it reads as `unknown (0x..)` and can't be written.
  With EuP on, the device very likely can't be woken from "off" by Wake-on-LAN or the RTC alarm.

Both are read-only, unless the module parameter `allow_power_config=1` is set when loading the
driver (e.g. `options asustor allow_power_config=1` in `/etc/modprobe.d/asustor.conf`), then
root can write them, e.g. `echo last | sudo tee /sys/devices/platform/asustor/ac_power_resume`.

*Note:* The register values and the meaning of the settings are from disassembling ASUSTOR's
firmware, not from a datasheet. The BIOS may set them again from its own setup when booting, so
a written value might only last until the next boot (not verified).

### GPIO pin configuration (IT8625E)

The GPIO lookup tables assume that the IT8625E pins they use are in GPIO function and not
inverted. ASUSTOR's firmware sets them like that, and so does the BIOS on AS6704T, so
`asustor-gpio-it87` doesn't change them. If a pin is configured differently when it's requested,
it logs a warning (once per pin) with the register and its value. With the module parameter
`fix_pin_config=1`, it instead sets the pin like ASUSTOR's firmware (GPIO function, if the pin's
pin mux bit is known, and non-inverted polarity) and logs each change. Pull-ups and the extra
setup ASUSTOR's firmware does for GP7x/GP8x pins are not changed.

With debugfs, `sudo cat /sys/kernel/debug/asustor_gpio_it87/regs` shows the pin configuration
registers (read-only).

### `it87` and PWM polarity

This project includes a patched version of the `it87` module that is part of mainline kernel (`asustor-it87`). It skips PWM sanity checks for the fan because ASUSTOR firmware correctly initializes fans in active low polarity and can be used straight with `fancontrol` or similar tools.

Note that `it87` conflicts with `asustor-it87`, you may wish to add `it87` to the module blocklist or explicitly load `asustor-it87` instead.

~~You may want to use [`patches/001-ignore-pwm-polarity-it87.patch`](patches/001-ignore-pwm-polarity-it87.patch) for the `it87` kernel module if it complains about PWM polarity. In this case, it's possible to use `fix_pwm_polarity=1`, however, it may reverse the polarity which is unwanted (i.e. high is low, low is high). It works fine when left as configured by the firmware.~~

### Override detection of ASUSTOR device by `asustor` kernel module

If the `asustor` kernel module doesn't detect your device correctly, you can force it to treat your
ASUSTOR device as one of the supported devices by setting the `force_device` module parameter.

This can be done manually with `sudo modprobe asustor force_device=AS66xx`, or by creating
`/etc/modprobe.d/asustor.conf` with the following content:

```
# override device detection of the asustor kernel module
options asustor force_device=FS6712
```

Please replace "FS6712" with the device you want to try.  
See the [Compatibility](#compatibility)-section above for how the `asustor` kernel module identifies devices.
Alternatively, can use the following command to print module parameters, including the currently supported device names for `force_device`:

```console
$ sudo modinfo -p asustor
```

_**NOTE:** If you need to use the `force_device` parameter to make your device work, please open an issue
so the detection logic in the `asustor` kernel module can be fixed to properly support it._

### Misc

- `blue:power` and `red:power` can be turned on simultaneously for a pink-ish tint
- `green:status` and `red:status` can be turned on simultaneously for a orange-ish tint

## Support

If you would like additional hardware to be supported, pull requests are more than welcome. Alternatively, you can install these prerequisites:

```
sudo apt-get install -y gpiod
```

And then open an issue and attach outputs from the following commands:

```
sudo dmesg
sudo dmidecode -s system-manufacturer
sudo dmidecode -s system-product-name
sudo dmidecode -s bios-vendor
sudo dmidecode -s bios-version
sudo dmidecode -s bios-release-date
sudo dmidecode -s bios-revision
sudo gpioinfo
sudo lspci -nn -PP
```

NOTE: If `gpioinfo` does not return anything, you may need to figure out which (if any) gpio drivers to load. Also keep in mind that your distribution may not ship with all `gpio-` drivers, so you may need to compile them yourself.

## TODO

- Support variable amount of disk LEDs
- ~~Create a new led trigger driver so that we can blink disk LEDs individually, the existing `disk-activity` trigger always blinks all LEDs on activity from any disk~~
  - Pray that [[PATCH v13 0/2] Introduce block device LED trigger](https://lore.kernel.org/lkml/20221227225226.546489-1-arequipeno@gmail.com/T/#mc8758efa18e1b7ed51a50c298d881a2e91280b1f)
    by Ian Pilcher lands in the linux kernel
  - Pray that [[PATCH] nvme-pci: trigger disk activity LED](https://lore.kernel.org/lkml/4100a868-c5bd-91dd-0c45-a92fb1344b12@kernel.dk/T/)
    by Enzo Matsumiya (or an alternative implementation that lets NVME disk activity trigger LEDs) lands in the linux kernel

## DKMS

DKMS installation to enable module auto-build with kernel upgrades. 

```
sudo make dkms
```
