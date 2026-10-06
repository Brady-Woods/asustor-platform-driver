# Changelog

All notable changes to this fork are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Releases are tagged `vX.Y.Z`. 0.3.0 and 0.4.0 were released as `v0.3` and `v0.4`
(the `DRIVER_VERSION` of the time) and tagged `v0.3.0` and `v0.4.0` afterwards.

Versions up to 0.2 are those of the upstream project,
[mafredri/asustor-platform-driver](https://github.com/mafredri/asustor-platform-driver),
which has no release tags or changelog. 0.3.0, the first release of this fork, is
based on upstream `main` as of December 2025 (cb392fc).

## [Unreleased]

### Added

- This changelog.
- CONTRIBUTING.md: how to report bugs, coding style, commit format with
  `Signed-off-by` (DCO), the vendored `it87` and the release process.
- Issue forms for bug reports and feature requests, and a pull request template.

### Changed

- Versions have three parts now (Semantic Versioning): `DRIVER_VERSION`, and with
  it the version of the DKMS packages, is `v0.4.0` instead of `v0.4`. `make dkms`
  adds `v0.4.0` next to an existing DKMS install of `v0.4`, so remove that first:
  `sudo make dkms_clean DRIVER_VERSION=v0.4`.

## [0.4.0] - 2026-10-06

### Added

- `it87`: `temp_type` and `temp_source` parameters to set up temperature channels
  that the BIOS leaves unconfigured. On the AS6704T, `temp_type=0,3,0
  temp_source=0,2,0` makes `temp2` read the system temperature, like ASUSTOR's
  firmware does.
- `it87`: `reset_limits=1` sets all limits to "no limit" when the driver is loaded,
  which clears the false alarms caused by the arbitrary limits the AS6704T's BIOS
  leaves.
- `it87`: `skip_pwm=N[,N...]` hides the given PWM outputs and never writes them
  (AS6704T: `skip_pwm=2,4,5,6`, outputs of unknown use).
- README: the recommended `it87` options for the AS6704T, and what each of its
  sensor channels is.

All four parameters are off by default and apply to every chip `it87` finds.
Tested on an AS6704T: `temp2` reads the board temperature with the driver alone,
no alarms are set except the chassis intrusion latch, and `pwm1` is the only PWM
output.

## [0.3.0] - 2026-10-06

First release of this fork: upstream `main` plus the changes below, tested
together on an AS6704T running TrueNAS SCALE (kernel 6.12) unless noted. Upstream
pull requests [#46](https://github.com/mafredri/asustor-platform-driver/pull/46),
[#47](https://github.com/mafredri/asustor-platform-driver/pull/47) and
[#48](https://github.com/mafredri/asustor-platform-driver/pull/48) are included,
but not merged upstream yet.

### Added

- Hardware LED blinking on the IT8625E: the `timer` LED trigger uses one of the
  chip's two blink units if `delay_on`/`delay_off` match a mode it supports, and
  blinks in software otherwise (upstream #46).
- `asustor-front-usb` LED trigger: the USB LED is lit while a device is plugged
  into the front USB port (AS6704T, upstream #47).
- Per-bay disk LED triggers `asustor-sataN` (AS6704T): like with ASUSTOR's
  firmware, a bay's LED is on while a disk is in it and blinks off when the disk
  is accessed; `disk_led_ready=0` inverts that, also at runtime (upstream #48).
- `/sys/devices/platform/asustor/lcd_power` switches the power of the LCD module
  (devices with an LCD).
- "ASUSTOR Buzzer" input device (`EV_SND`): plays tones on the PC speaker and opens
  the buzzer gate GPIO (GP75) while a tone plays. The tone is made with an hrtimer,
  since the PIT's clock is gated on the AS6704T. `buzzer` module parameter and
  `/sys/devices/platform/asustor/buzzer_gate` status file. Tested on an AS6704T;
  mapped on the other Jasper Lake models and AS66xx from ASUSTOR's firmware only.
- `ac_power_resume` (`off`, `last`, `on`) and `eup` power settings in
  `/sys/devices/platform/asustor/` on IT8625E devices, read-only unless
  `allow_power_config=1`.
- `asustor_gpio_it87`: warns when an IT8625E pin is inverted or not in GPIO
  function when it's requested; with `fix_pin_config=1` it sets the pin up like
  ASUSTOR's firmware instead. Register dump in
  `/sys/kernel/debug/asustor_gpio_it87/regs`.
- `asustor_gpio_it87`: `get_direction`, so `gpioinfo` and `/sys/kernel/debug/gpio`
  show the actual direction of each line.
- Frank Crawford's out-of-tree [`it87`](https://github.com/frankcrawford/it87)
  (bc06d34 plus its pull request #110, `force_pwm`), built as `it87.ko`, which
  replaces the kernel's `it87`. `tools/sync-it87.sh` updates it.
- `it87`: `led_pwm`, `led_pwm_invert` and `led_pwm_name` parameters to drive a PWM
  output as an LED, for the AS6704T's front panel LED brightness
  (`led_pwm=3 led_pwm_invert=1` gives `front_panel::brightness`). Not tested on
  hardware yet.
- The AS6704T's rear reset button (GP81), as `KEY_VENDOR` on `asustor-keys`. A
  press hasn't been tested yet.
- `/sys/devices/platform/asustor/` exists on every supported device, so userspace
  can detect the driver.

### Changed

- **Breaking:** `asustor_it87` is replaced by `it87` (see Added). The module,
  driver and platform device are called `it87` (`/sys/devices/platform/it87.*`),
  and the `asustor-it87` DKMS package builds `it87.ko`: update fan control configs
  (see `example/fancontrol`). On boards whose BIOS sets up active low PWM with the
  fans "off" (e.g. the AS6704T), the `pwm*` files only exist with `force_pwm=1`.
- `asustor_gpio_it87`: requesting a line no longer changes its direction.
- `asustor_gpio_it87` on the IT8625E: requesting a line switches it to GPIO
  function (Simple I/O), which also stops the status LED blinking that the
  firmware leaves on.
- On AS67xx, AS54xx and FS67xx, the front panel power GPIO (GP45) is left as the
  BIOS set it; AS66xx, AS61xx and AS6xx still switch it on.
- `blue:lan` keeps its state when `asustor` is unloaded and at shutdown.

### Removed

- **Breaking:** the `power:lcd` and `power:front_panel` LEDs. These GPIOs are power
  rails, which the `asustor` device now holds (see `lcd_power`). To detect the
  driver, check for `/sys/devices/platform/asustor/` instead of
  `/sys/class/leds/power:lcd`.
- **Breaking:** the `asustor_it87` module.
- **Breaking:** the `gpled1_blink`, `gpled1_blink_freq` and `gpled2_*` sysfs files
  of `asustor_it87`; use the `timer` LED trigger instead.
- `patches/001-ignore-pwm-polarity-it87.patch`, replaced by `force_pwm`.

### Fixed

- Loading and unloading the drivers no longer cuts the power rails of the LCD, the
  front panel and the front LAN LEDs (the LCD's microcontroller rebooted):
  `asustor_gpio_it87` switched requested lines to input, and unloading `asustor`
  switched the "LEDs" off.
- `asustor_gpio_it87` could write the wrong registers after another driver had
  selected a different Super I/O logical device, and didn't hold its lock for the
  read-modify-write of a GPIO's value.

[Unreleased]: https://github.com/Brady-Woods/asustor-platform-driver/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/Brady-Woods/asustor-platform-driver/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Brady-Woods/asustor-platform-driver/compare/cb392fc2fe60...v0.3.0
