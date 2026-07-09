trccd - a lightweight daemon for controlling Thermalright LCD displays
======================================================================

`trccd` is a lightweight solution for controlling Thermalright LCD displays
found on AIO coolers such as the Frozen Warframe series. It uses `libusb` and
`ffmpeg` to process and stream images or videos (or just a solid colour)
directly to the screen.

Key Features
------------

- **Easy to use:** Just point `trccd` at your media and it uses `ffmpeg` under
the hood to automatically scale, crop and convert it to fit the display.
- **Automatic caching:** Processed media is cached to disk on the first run.
Each subsequent run will begin playing the media instantly by loading from the
cache.
- **systemd integration:** Designed to run in the background as a systemd
service.
- **Security:** The service unit file makes extensive use of systemd sandboxing
features (see
[systemd.exec(5)#SANDBOXING](https://man.archlinux.org/man/systemd.exec.5.en#SANDBOXING))
to run the daemon as a dynamic user with stripped capabilities. A custom udev
rule grants access to the USB device via the `tr_display` group.
- **Lightweight:** Written in pure C with performance and efficiency in mind.
The Linux page cache is used to keep as much of the media in memory as possible
without exhausting system RAM and the program will loop the media with as few
USB transfers as it can manage. For static images and solid colours this uses
very little CPU time.
- **Pipe in your own data:** Arbitrary data can be read from `stdin` so you can
experiment with your own `ffmpeg` commands or use a different program to
process your media.

Supported Devices
-----------------

| Vendor                    | Product    | VID:PID   |
| ------------------------- | ---------- | --------- |
| Winbond Electronics Corp. | USBDISPLAY | 0416:5302 |

Only the device above is currently supported. If you own a Thermalright product
that isn't supported and you're willing to help test, please
[create an issue](https://github.com/Jontos/trccd/issues/new/choose) and we can
try to add support.

> [!TIP]
> Run `lsusb` to find the name of your device.

Installation
------------

### From Source

#### Dependencies

- `gcc` >= 16 or `clang` >= 21
- `make`
- `libusb`
- `pkg-config`
- `ffmpeg` (not required if you only plan to pipe in pre-processed data)

Clone the repository and build with `make`:

```sh
git clone https://github.com/jontos/trccd.git
cd trccd
make
```

Install to your system with:

```sh
sudo make install
```

> [!NOTE]
> Currently only tested on Arch Linux. If you run into problems on a different
> distro, please [create an
> issue](https://github.com/Jontos/trccd/issues/new/choose).

Usage
-----

The included systemd service reads variables from `/etc/trccd/trccd.conf`. Set
the mode and path to your media there:

```sh
TRCCD_MODE=video
TRCCD_FILE=/etc/trccd/my_video.mkv
```

> [!IMPORTANT]
> The path used for `TRCCD_FILE` must be readable by the dynamic user created
> by systemd; this user **cannot** read files inside your home directory. Copy
> your media somewhere globally readable like `/etc/trccd/` and specify that
> instead.

Start the service with:

```sh
sudo systemctl start trccd
```

Enable it to start at boot:

```sh
sudo systemctl enable trccd
```

### Command Line Usage

`trccd` can also be run manually:

```sh
trccd <mode> [arg]
```

Where `<mode>` is one of `video`, `image` or `colour`, and `[arg]` is either a
path to some media (for the first two modes) or an RGB565 colour in hexadecimal
(e.g. `0xF800` for red) for `colour`.

You can also omit `[arg]` if you want to pipe in your own data:

```sh
ffmpeg -nostdin \
    -loglevel fatal \
    -i /path/to/my_media.mkv \
    -an -sn \
    -vf "scale=320:240:force_original_aspect_ratio=increase,crop=320:240,transpose=1,fps=22" \
    -f rawvideo \
    -pix_fmt rgb565le - | trccd video
```

> [!NOTE]
> The command above is what `trccd` runs internally for `video` mode.

Data can also be fed in from a file using shell redirection:

```sh
trccd video < rgb565_bytes.bin
```

> [!NOTE]
> Data piped/redirected into `trccd` this way is not saved to disk.

Caching
-------

The first time you play a media file, `trccd` runs `ffmpeg` to convert it and
writes the processed frames to disk. Subsequent runs with the same media will
map those cached frames into memory, avoiding redundant calls to `ffmpeg` and
reducing startup time.

#### Cache location

- **Under systemd**: `/var/cache/private/trccd`.
- **When run manually**: `$XDG_CACHE_HOME/trccd` or `$HOME/.cache/trccd` if
`$XDG_CACHE_HOME` is empty or unset.

It's safe to delete these directories to wipe the cache.

Caveats
-------

- Available disk space is not checked before caching the media to disk.
- Media is saved in an uncompressed format. One frame is 153.6 kilobytes, so
one second of video at 22 FPS will be ~3.4MB; keep this in mind if you plan to
watch any movies on your CPU cooler.

Licence
-------

This project is licensed under the GNU General Public Licence v3 (GPLv3) - see
the [LICENCE](LICENCE) file for details.
