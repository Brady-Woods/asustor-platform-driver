# Contributing

Bug reports, hardware data and pull requests are welcome. This is a small project
with one maintainer, so please be patient.

## Upstream

This is a fork of
[mafredri/asustor-platform-driver](https://github.com/mafredri/asustor-platform-driver)
(see "About this fork" in the [README](README.md)). Changes that are useful
upstream (fixes, support for more devices) should go there too, so the fork can
eventually go back to tracking upstream `main`:

- Each change meant for upstream lives on its own branch, based on upstream `main`,
  and is submitted as its own upstream pull request (e.g. `gpio-it87-hw-blink` for
  upstream #46). This fork's `main` merges or carries those changes until upstream
  takes them.
- If you send a pull request here that also makes sense upstream, say so; keeping
  it independent of fork-only changes makes it easier to submit upstream as well.
- Changes to the vendored `it87` that are not specific to this fork belong in
  [frankcrawford/it87](https://github.com/frankcrawford/it87), see
  [below](#vendored-it87).

## Reporting bugs

Please use the [bug report form](https://github.com/Brady-Woods/asustor-platform-driver/issues/new/choose)
and include:

- the device model (e.g. AS6704T) and, for detection problems,
  `sudo dmidecode -s system-manufacturer` and `sudo dmidecode -s system-product-name`,
- the kernel (`uname -r`) and distribution (e.g. TrueNAS SCALE 25.10),
- the driver version (`git describe --tags --always --dirty` in your checkout) and
  your module options (`/etc/modprobe.d/`),
- the driver messages: `sudo dmesg | grep -iE 'asustor|it87|gpio'`,
- on IT8625E devices, the GPIO pin configuration:
  `sudo cat /sys/kernel/debug/asustor_gpio_it87/regs` (needs debugfs).

For support of a new device, see "Support" in the [README](README.md) for what to
collect.

## Coding style

The modules follow the
[Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html):

- `asustor_gpio_it87.c` and the vendored `it87.c`: kernel style, by hand. Match the
  surrounding code and don't reformat lines you don't change, which would make
  upstream merges (and `tools/sync-it87.sh`) harder.
- `asustor_main.c` and `asustor_power.c`: formatted with clang-format, using the
  repository's [`.clang-format`](.clang-format) (the kernel's). Run
  `git clang-format` before committing, which formats only the lines you changed.
- `asustor_gpl2.c` holds code copied from the kernel (GPL-2.0-only), `compat.h` is
  vendored with `it87.c`: leave their style alone.

Run the kernel's `checkpatch.pl` (from a Linux source tree) on your commits, in the
top directory of this repository:

```sh
/path/to/linux/scripts/checkpatch.pl --strict --no-tree --ignore FILE_PATH_CHANGES -g main..HEAD
```

It only checks the lines a commit changes, which is the only way to check the
vendored `it87.c` (never `checkpatch.pl --file it87.c`). CI runs the same on every
pull request; it ignores `Co-authored-by:` trailers, which checkpatch doesn't know.
If a warning is a false positive, or the code has to look like the code around it,
say why in the pull request.

## Commits

One logical change per commit, in the kernel's format
([Submitting patches](https://www.kernel.org/doc/html/latest/process/submitting-patches.html)):

```
it87: add skip_pwm parameter

Why the change is needed and what it does, wrapped at 72 to 75
columns. Say what was tested, on which device and kernel, and what
wasn't (e.g. "Build-tested only" or "not tested on hardware yet").

Signed-off-by: Your Name <you@example.com>
```

- The subject is `component: summary`, in the imperative, at most 75 characters.
  Components: `asustor` (`asustor_main.c`, `asustor_power.c`),
  `asustor_gpio_it87`, `it87`, `tools`, `Makefile`, `README`, `CHANGELOG`, `ci`,
  `github`. Name the device for device-specific changes (e.g. `asustor: AS6704: ...`).
- Mark hardware behaviour that's inferred (from ASUSTOR's firmware, say) rather than
  tested, so it can be checked or dropped later.
- Add a line to the `[Unreleased]` section of [CHANGELOG.md](CHANGELOG.md) for
  changes users notice, and mark changes that break userspace (sysfs files, LED,
  module or parameter names) as **Breaking**.

### Developer Certificate of Origin

Every commit needs a `Signed-off-by:` line with your name and email address, added
by `git commit -s`. With it you certify the
[Developer Certificate of Origin](https://developercertificate.org): that you wrote
the change or otherwise have the right to submit it under the license of the files
you change (their `SPDX-License-Identifier` line). It's the same rule as for the
Linux kernel. Only people sign off: if a
tool (including an AI assistant) helped write a change, you are still the one who
certifies it. To add a missing sign-off: `git commit --amend -s`, or
`git rebase --signoff main` for several commits.

## Building and testing

```sh
make                      # needs the headers of the running kernel
make TARGET=6.12.33-...   # or for another installed kernel
```

CI builds the modules against the headers of an Ubuntu kernel with `-Werror`, so
new warnings fail the build. Loading the modules needs the hardware: see the
[README](README.md) for `insmod`, the `it87` replacement and DKMS. In a pull
request, say on which device and kernel you tested, and what.

## Vendored it87

`it87.c` and `compat.h` are [Frank Crawford's `it87`](https://github.com/frankcrawford/it87),
imported by [`tools/sync-it87.sh`](tools/sync-it87.sh), which records the upstream
version in [`it87.UPSTREAM`](it87.UPSTREAM):

- Local changes are separate commits on top of the import (`it87: ...`); never mix
  them into an import commit.
- To update, fetch a clone of frankcrawford/it87 and run
  `tools/sync-it87.sh --commit <clone> origin/master [<commit>:<label>...]` (extra
  commits are e.g. unmerged upstream pull requests). It merges the upstream changes
  into the current files, keeping the local changes, and stops on conflicts. It
  commits without a sign-off, so add one: `git commit --amend -s`.
- `tools/sync-it87.sh --diff <clone>` shows the local changes against the recorded
  upstream version.

## Releases

For the maintainer. Versions follow [Semantic Versioning](https://semver.org/):
while the major version is 0, changes that break userspace bump the minor version.

1. Rename `[Unreleased]` in CHANGELOG.md to `[X.Y.Z] - YYYY-MM-DD`, add an empty
   `[Unreleased]` section and update the compare links at the bottom.
2. Set `DRIVER_VERSION := vX.Y.Z` in the Makefile, and commit both as
   "Release vX.Y.Z" (signed off).
3. Tag it: `git tag -a vX.Y.Z`, with the version's changelog section as the message.
4. Push `main` and the tag, then create a GitHub release from the tag with the
   changelog section as release notes (e.g.
   `gh release create vX.Y.Z --notes-file notes.md`).
