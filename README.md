# asustor-platform-driver

Linux kernel platform driver for ASUSTOR NAS hardware (leds, buttons).

On many systems, ASUSTOR uses a mix of IT87 and CPU GPIOs to control leds and buttons. Adding support for more systems should be fairly trivial, but may require some reverse engineering to figure out which GPIOs are responsible for what.

**WARNING:** Changing GPIO input/outputs (as done by this module) without knowledge of their effects can be dangerous and lead to instability, corrupted data or a broken system. **Use at your own risk.**

## About this fork

This is a fork of [mafredri/asustor-platform-driver](https://github.com/mafredri/asustor-platform-driver).
It runs an ASUSTOR AS6704T (LOCKERSTOR 4 Gen2) under TrueNAS SCALE, which
doesn't ship this driver, and brings the driver closer to what ASUSTOR's own
firmware (ADM) does on that hardware. The status daemon
[truenas-asustor-chassisd](https://github.com/Brady-Woods/truenas-asustor-chassisd)
drives the front panel through it and needs this fork's `main`, v0.3.0 or later
(`git clone https://github.com/Brady-Woods/asustor-platform-driver`).
What changed in each release is in the [changelog](CHANGELOG.md); bug reports and
pull requests are welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).

v0.4.0 is upstream `main` plus (v0.4.0 added the `it87` sensor parameters):

- Three changes submitted upstream but not merged yet, combined here because
  they conflict with each other (each also stays on its own branch for review):
  - [#46](https://github.com/mafredri/asustor-platform-driver/pull/46) (`gpio-it87-hw-blink`):
    hardware LED blinking through the IT8625E's GP LED blink units, plus `asustor_gpio_it87`
    fixes (Simple I/O switches, re-selecting the GPIO LDN on every access, locking); removes
    the `gpled*` sysfs files.
  - [#47](https://github.com/mafredri/asustor-platform-driver/pull/47) (`front-usb-led`):
    `asustor-front-usb` LED trigger, so the front USB LED follows the front port.
  - [#48](https://github.com/mafredri/asustor-platform-driver/pull/48) (`per-bay-disk-led`):
    per-bay disk activity LED triggers (`asustor-sataN`) and the `disk_led_ready` parameter,
    here also writable at runtime.
- `asustor_gpio_it87` no longer changes a pin's direction in `request()`, and implements
  `get_direction`.
- [Power rails](#power-rails): the LCD and front panel power GPIOs are held by the `asustor`
  device instead of being LEDs, with `lcd_power` to switch the LCD.
- The ["ASUSTOR Buzzer"](#buzzer) input device: the tone is made with an hrtimer, since the PIT's
  clock is gated on the AS6704T, and the buzzer gate GPIO opens while it plays.
- [`ac_power_resume` and `eup`](#power-settings-ac-power-loss-and-eup) power settings.
- [Pin configuration checks](#gpio-pin-configuration-it8625e) for the IT8625E (`fix_pin_config`)
  and a debugfs register dump.
- [Frank Crawford's `it87`](#it87-fan-control-and-pwm-polarity) (with its PR #110, `force_pwm`)
  vendored as `it87`, replacing `asustor-it87`, plus a `led_pwm` parameter for the
  [front panel LED brightness](#front-panel-led-brightness) and `temp_type`, `temp_source`,
  `reset_limits` and `skip_pwm` parameters for the [sensors](#it87-sensors-on-the-as6704t).
- The AS6704T's [reset button](#reset-button) as `KEY_VENDOR` (a press hasn't been tested yet).

Once all of this is upstream, this fork can go back to tracking upstream `main`.

## Dependencies

**Note:** The following dependencies from the mainline linux kernel are required, if they're not included by your distribution you may need to compile them yourself (note that some modules are only required on specific ASUSTOR models):

- `gpio-ich` (AS6)
- `hwmon-vid` (for the included `it87` module)

### Optional

- `it87` (AS6, AS61, AS62, AS66XX, AS67XX, AS54XX, FS67XX)
  - This project includes [Frank Crawford's out-of-tree `it87` driver](https://github.com/frankcrawford/it87)
    (see [below](#it87-fan-control-and-pwm-polarity)), which supports more variants of the IT86XX
    and IT87XX chips (like the IT8625E) than the kernel's `it87` driver. It's built as `it87.ko`,
    under the same name as the kernel's driver, which it replaces.
  - Also includes a patched version of `gpio-it87` called `asustor-gpio-it87`, which supports
    the IT8625E chip that is used in several newer ASUSTOR devices, including
    [hardware blinking](#hardware-led-blinking) of up to two LEDs.
  - May require adding `acpi_enforce_resources=lax` to kernel boot arguments for full functionality
  - Temperature monitoring (`lm-sensors`), see [below](#it87-sensors-on-the-as6704t) for the
    AS6704T
  - Fan speed regulation via `pwm1`
    - See [`example/fancontrol`](./example/fancontrol) for an example `/etc/fancontrol` config for a AS62 system
    - `pwm1` etc should be in `/sys/devices/platform/it87.*/hwmon/hwmon*/`
  - Front panel LED brightness adjustment, see [below](#front-panel-led-brightness)

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
  - Reset Button on the back (AS6704T, as `KEY_VENDOR`: not `KEY_RESTART`, which would make
    systemd-logind reboot)
- Power rails: LCD power (`/sys/devices/platform/asustor/lcd_power`), front panel power,
  see [below](#power-rails)
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

### Power rails

Two IT87 GPIOs that older versions of this driver showed as the LEDs `power:lcd` and
`power:front_panel` are power rails, not LEDs. They're no longer in `/sys/class/leds/`: the
`asustor` module switches them on when it's loaded and holds them.

- **LCD power** (AS6704T, AS6706T, AS66xx, AS61xx, AS6xx; also AS5404T, which shares the AS6704T's
  tables):
  `/sys/devices/platform/asustor/lcd_power` reads `1` (on) or `0` (off), and root can write
  either, e.g. `echo 1 | sudo tee /sys/devices/platform/asustor/lcd_power`.
  **Writing `0` cuts the power of the whole LCD module**, including its microcontroller: the
  display goes dark, the front panel buttons (which that microcontroller reads) stop working, and
  it boots again when the power comes back, so whatever was on the display is gone. To switch
  off just the display, send the LCD its display-off command over the serial port instead
  (that's what truenas-asustor-chassisd does). The driver never switches the LCD off by itself:
  the rail keeps its state when the driver is unloaded and at shutdown.
- **Front panel power** (AS66xx, AS61xx, AS6xx): switched on and held, nothing to set (not
  tested). On AS67xx, AS54xx and FS67xx that GPIO (GP45) is left as the BIOS set it: ASUSTOR's
  firmware doesn't drive it there, and switching it had no visible effect on an AS6704T.

`blue:lan` stays an LED, although on some devices (like AS6704T) it powers the front LAN LEDs
rather than lighting an LED itself, so it can switch them off (e.g. at night). It keeps its state
when the driver is unloaded.

### Detecting the driver

`/sys/devices/platform/asustor/` exists whenever the `asustor` module is loaded on a supported
device, so userspace can check for it to detect the driver (`/sys/class/leds/power:lcd`, which
was used for that before, no longer exists). What's in it depends on the device:
- `lcd_power`: devices with an LCD, see [Power rails](#power-rails),
- `ac_power_resume` and `eup`: devices with an IT8625E, see [below](#power-settings-ac-power-loss-and-eup),
- `buzzer_gate`: devices with a known buzzer gate, see [Buzzer](#buzzer).

Older versions of the driver only created it on some devices.

### Buzzer

The buzzer is the PC speaker output (port 0x61), but it only sounds while an IT87 GPIO (GP75, the
"buzzer gate") is high: ASUSTOR's firmware sets it before every beep and clears it afterwards.
The `asustor` module claims that GPIO (low while idle) and registers its own input device for the
buzzer, **"ASUSTOR Buzzer"**, which plays tones on the PC speaker and opens the gate while a tone
plays. `pcspkr` isn't needed.

The PIT (timer chip) doesn't generate the tone on these devices: its channel 2 output never
changes when it's programmed for a tone (checked on an AS6704T: its clock is gated), which is why
`pcspkr` is silent. So "ASUSTOR Buzzer" makes the square wave in software, like the kernel's
`snd-pcsp` and ASUSTOR's own buzzer driver: it keeps the PIT channel's output steady and toggles
the speaker data bit of port 0x61 from an hrtimer every half period of the tone.

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
- If `pcspkr` is loaded, its "PC Speaker" input device still exists, but stays silent (the PIT
  doesn't run, and the gate only opens for "ASUSTOR Buzzer"). Both change port 0x61 under the
  kernel's `i8253_lock`, so they never mix up their writes.
- While a tone plays, the hrtimer fires twice per period (4000 times a second at 2 kHz). That's
  fine for beeps; very high frequencies (up to 32766 Hz) mean correspondingly more interrupts.
- The gate GPIO is set from a work item (the IT87 GPIO driver can sleep), so it opens a fraction
  of a millisecond to a few milliseconds after the tone starts; very short tones may be cut short
  or not sound at all.
- The tone stops and the gate closes when `asustor` is unloaded and before a reboot or power off;
  on suspend the kernel stops the tone.
- Tested on an AS6704T: audible, 2 kHz from the hrtimer. GP75 and its polarity are from ASUSTOR's
  firmware, which drives it on all its Jasper Lake devices and on AS66xx; the other models are not
  tested.

### Reset button

The pinhole button on the back of the AS6704T is not a hardware reset: it's an IT87 GPIO input
(GP81) that software polls. ASUSTOR's firmware uses it to restore settings after it's held for
about 5 seconds. The driver only reports it; it does nothing on its own.

It's a key of the `asustor-keys` input device (`gpio-keys-polled`, polled every 50 ms), next to the
USB Copy Button, and reports `KEY_VENDOR` (code 360) on press (value 1) and release (value 0):

```sh
sudo evtest   # pick the "asustor-keys" device
```

It deliberately isn't `KEY_RESTART`: systemd-logind watches `asustor-keys` (it also carries
`KEY_POWER`) and reboots on `KEY_RESTART` by default (`HandleRebootKey=reboot`), so a brush of the
pinhole would reboot the machine. Anything that wants a "hold for N seconds" action, like ASUSTOR's
firmware, has to time the press itself.

*Note:* Only mapped on AS6704T. The GPIO idles high there (checked), so it's mapped active low;
an actual press hasn't been tested yet.

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

### `it87`: fan control and PWM polarity

The hardware monitoring driver (temperatures, fans, `pwm*`) is
[Frank Crawford's out-of-tree `it87`](https://github.com/frankcrawford/it87), included as
`it87.c` and `compat.h`. [`it87.UPSTREAM`](it87.UPSTREAM) records which upstream commits they're
from (currently upstream `master` plus [PR #110](https://github.com/frankcrawford/it87/pull/110),
which adds the `force_pwm` parameter). Changes made here are separate commits on top of the
imported version; `tools/sync-it87.sh <it87 checkout> <ref>` imports a newer upstream version
and keeps them. Older versions of this project had their own copy of `it87` instead, called
`asustor-it87` (`asustor_it87.ko`).

The module is called `it87`, like the kernel's own driver, which it replaces, so only one of the
two can be loaded at a time:
- `make install` installs it to `/lib/modules/$(uname -r)/updates/`, and DKMS installs it in a
  way that also overrides the kernel's driver, so `modprobe it87` (and the `asustor` module's
  soft dependency on `it87`) loads this one.
- When loading the modules from the build directory with `insmod` instead, make sure the kernel's
  `it87` isn't loaded (`lsmod | grep it87`, unload it with `sudo rmmod it87`), otherwise
  `insmod it87.ko` fails with "File exists". Don't let anything load the kernel's driver at boot
  either (e.g. an `it87` line in `/etc/modules` from `sensors-detect`).
- `cat /sys/module/it87/version` shows which one is loaded: this one has a version ending in
  `+asustor`.

The hwmon device is in `/sys/devices/platform/it87.*/hwmon/hwmon*/` (named after the chip, e.g.
`it8625`).

**PWM polarity:** ASUSTOR's firmware sets up the fan outputs with active low polarity, and on
some models (e.g. AS6704T) with all fans "off" in the fan control register. `it87` takes that for
broken BIOS settings and doesn't create the `pwm*` files, with this message in `dmesg`:
```
it87 it87.2608: Detected broken BIOS defaults, disabling PWM interface (see fix_pwm_polarity and force_pwm parameters)
```
In that case, load it with `force_pwm=1`, e.g. with `options it87 force_pwm=1` in
`/etc/modprobe.d/it87.conf`. That keeps the active low polarity (as ASUSTOR's firmware does, it
never changes it) and enables the `pwm*` files, after which `fancontrol` or similar tools work as
usual. Don't use `fix_pwm_polarity=1`, which switches to active high polarity and so inverts fan
control on these devices. And only use `force_pwm=1` if you get that message: it always sets
active low polarity, so on a device whose firmware chose active high it would invert fan control.
(`asustor-it87` didn't check the PWM settings at all, so it needed no parameter for this.)

Tested on an AS6704T: with `force_pwm=1`, `pwm1` = 255, 153 and 100 give 2606, 1785 and 1271 RPM.

### Front panel LED brightness

On the AS6704T, the brightness of the front panel LEDs (power, status, LAN and USB, but not the
drive bay LEDs) is set by the IT8625E's PWM3 output, which `it87` shows as `pwm3`, like a fan.
It's inverted (ASUSTOR's firmware sets the duty cycle to 255 minus the brightness, so `pwm3` = 255
means the LEDs are off), and fan control tools take it for a fan. With the `it87` parameters
`led_pwm=3 led_pwm_invert=1`, it's an LED called `front_panel::brightness` instead, and there's no
`pwm3`:
```
# /etc/modprobe.d/it87.conf
options it87 force_pwm=1 led_pwm=3 led_pwm_invert=1
```
(see [below](#it87-sensors-on-the-as6704t) for all the options the AS6704T needs)
```
cat /sys/class/leds/front_panel::brightness/brightness
echo 76 | sudo tee /sys/class/leds/front_panel::brightness/brightness
```
- The brightness goes from 0 (front panel LEDs off) to 255 (full brightness). ASUSTOR's firmware
  defaults to 30 % (76); the BIOS sets about 80 % (204, `pwm3` = 51). Loading `it87` keeps the
  current brightness, and unloading it leaves the LEDs as they are.
- `it87` keeps the output in manual mode, so the chip's automatic fan control never changes it.
  The LED doesn't need `force_pwm=1`, that's only for the fans.
- `led_pwm=N` works for any PWM output of any chip `it87` supports (`led_pwm_invert` defaults to
  off); `led_pwm_name=` sets another name for the LED.
- `pwm1_freq` also sets the frequency of PWM3 (they share a clock setting). `fix_pwm_polarity=1`
  would invert the brightness.
- ASUSTOR's firmware dims the front panel LEDs like this on most of its x86 models with an IT87
  chip, but the AS6704T is the only one where it's been checked, and the LED device hasn't been
  tested on hardware yet.

### `it87` sensors on the AS6704T

The BIOS of the AS6704T leaves the IT8625E's sensors half set up: no temperature channel is
configured (`temp1`-`temp3` read -128 °C), and the limits are arbitrary values, so most alarms
are set although the inputs are fine. With these `it87` options (the first three are from
[above](#it87-fan-control-and-pwm-polarity) and [Front panel LED brightness](#front-panel-led-brightness)),
every channel should read what it is and no alarm should be set without a reason:
```
# /etc/modprobe.d/it87.conf
options it87 force_pwm=1 led_pwm=3 led_pwm_invert=1 temp_type=0,3,0 temp_source=0,2,0 reset_limits=1 skip_pwm=2,4,5,6
```

| Option | What it does |
|---|---|
| `force_pwm=1` | Enables the `pwm*` files, keeping the active low polarity of the BIOS |
| `led_pwm=3 led_pwm_invert=1` | PWM3 is the front panel LED brightness (`front_panel::brightness`), not `pwm3` |
| `temp_type=0,3,0` | temp2 is a thermal diode (sensor type 3, as in `tempN_type`); 0 leaves temp1 and temp3 as they are |
| `temp_source=0,2,0` | temp2 reads the TMPIN2 pin; 0 leaves temp1 and temp3 as they are |
| `reset_limits=1` | Sets all limits to "no limit" when `it87` is loaded, which clears the false alarms |
| `skip_pwm=2,4,5,6` | Doesn't expose (or ever write) PWM2 and PWM4-6, whose use is unknown. Optional, see below |

The channels with these options:

- **`temp2` is the system (board) temperature**, the one ASUSTOR's firmware (ADM) reads. It's
  about 37 °C on an idle system, and `temp2_type` reads 3 (thermal diode; read-only). `temp1` and
  `temp3` aren't connected and keep reading -128 °C. ADM only uses the system temperature as an
  emergency input: above **89 °C**, or when it can't be read, ADM runs the fan at its maximum
  (45 % on this model) and, for a sensor that can't be read, reports a "System Sensor Fault".
  Otherwise its default (automatic) fan control ignores it: the fan follows the CPU, disk and
  SSD temperatures.
  The temperature setup is done again after resume.
- `fan1` is the fan (the only one of the AS6704T). `fan2` and `fan3` aren't connected and read
  0 RPM, without alarms now (their minimums are 0 RPM).
- `in0`-`in9` are all real inputs; ASUSTOR's firmware doesn't label them, `in7` (3VSB), `in8`
  (Vbat) and `in9` (+3.3V) have the chip's own labels.
- `pwm1` controls the fan. PWM3 is the front panel LED. **PWM2 and PWM4-6 are of unknown use**:
  the pin configuration says they're PWM outputs, but ASUSTOR's firmware never drives them (the
  BIOS leaves PWM2 at 51, PWM4-6 at 128). With `skip_pwm=2,4,5,6`, `it87` doesn't create
  `pwm2`, `pwm4`-`pwm6` and never writes them, so neither can `pwmconfig` (which drives every
  output to full and to zero speed to find the fans) or a misconfigured fan control. Without it,
  they stay visible; then just don't write them. Hiding them is recommended, since there's
  nothing to gain from writing them and nobody knows what they're wired to.
- `intrusion0_alarm` reads 1, but the AS6704T has no known chassis intrusion switch (ADM never
  looks at it), so it means nothing here. `it87` doesn't clear it when loading: on boards with a
  switch, it records an opening while the driver wasn't loaded. To clear it:
  `echo 0 | sudo tee /sys/devices/platform/it87.*/hwmon/hwmon*/intrusion0_alarm` (if it comes
  back right away, the input is simply asserted; ignore it).

With `reset_limits=1`, the chip raises no alarm by itself, including for a stalled fan (`fan1_min`
is 0 RPM too): set limits with `lm-sensors` if you want them (e.g. a `fan1_min` below the speed
at the lowest PWM your fan control uses), they're kept until `it87` is loaded again. For example
`/etc/sensors.d/as6704t.conf`, applied with `sudo sensors -s`:
```
chip "it8625-*"
    label temp2 "System"
    set temp2_max 89     # ADM's emergency threshold
    # optional: hide the unconnected inputs from `sensors`
    ignore temp1
    ignore temp3
    ignore fan2
    ignore fan3
    ignore intrusion0
```

The options are generic, see `modinfo -p it87`: `temp_type` (temp1-temp3: 3 = thermal diode,
4 = thermistor, 6 = PECI where the chip has it) works on any chip `it87` supports;
`temp_source` (1-3 = TMPIN1-3) on the IT8625E, IT8655E and IT8665E; 0 always leaves a channel as
the BIOS set it. They apply to every chip `it87` finds.

*Notes:* ASUSTOR's firmware writes 0x10 to register 0x1D of bank 2 (`0x21D` in `it87`, the
temperature sources of temp1 and temp2) and sets bit 1 of register 0x51 (temp2 is a thermal
diode) before every reading; doing the same on an AS6704T made temp2 read 37 °C. `temp_type` and
`temp_source` do exactly that. The parameters themselves haven't been tested on hardware yet
(including whether the fan alarms of `fan2` and `fan3` really clear with a minimum of 0 RPM).

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

This adds three DKMS packages: `asustor`, `asustor-gpio-it87` and `asustor-it87` (which builds
`it87.ko`). If Frank Crawford's `it87` is already installed through its own DKMS package (`it87`),
remove that first, both install a module called `it87`.
