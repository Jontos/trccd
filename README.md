trccd - a lightweight daemon for controlling Thermalright LCD displays
======================================================================

`trccd` is a lightweight solution to allow for control of Thermalright LCD
displays found on AIO coolers such as the Frozen Warframe series. It uses
`libusb` and `ffmpeg` to process and stream images or videos (or just a solid
colour) directly to the screen.


Key Features
------------

- **Easy to use**: Just specify the path to your custom media and the program
  will use `ffmpeg` under the hood to automatically scale, crop and convert
  the media to fit the display.
- **systemd integration**: Designed to be run in the background as a systemd
  service.
- **Security**: The service unit file makes extensive use of systemd sandboxing
  features (see [systemd.exec(5)#SANDBOXING](https://man.archlinux.org/man/systemd.exec.5.en#SANDBOXING))
  to run the daemon as a dynamic user with stripped capabilities. A custom udev
  rule is used to allow access to the USB device via the `tr_display` group.
- **Lightweight**: Written in pure C with performance in mind. At startup the
  program will load your chosen media into memory and loop it with as few USB
  transfers as it can manage. For static images/colour this uses very little CPU
  time.
- **Pipe in your own data**: Can read arbitrary data from `stdin` in case you'd
  like to, for example, experiment with your own `ffmpeg` commands or use a
  different program for processing your media.


Supported Devices
-----------------

| Vendor                    | Product    | VID:PID   |
| ------------------------- | ---------- | --------- |
| Winbond Electronics Corp. | USBDISPLAY | 0416:5302 |

There is currently only support for the above device. If you own a Thermalright
product that is not supported and you are willing to help with testing, please
[create an issue](https://github.com/Jontos/trccd/issues/new/choose) and we can
try to add support.

> [!TIP]
> Run `lsusb` to find the name of your device.


Installation
------------

### From Source

#### Dependencies:

- `gcc` (or another C compiler that supports `-std=gnu23`)
- `make`
- `libusb`
- `pkgconf`
- `ffmpeg` (not required if you only plan to pipe in your own pre-processed data)

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
> *Currently only tested on Arch Linux. If you run into problems with the above
> steps on a different distro please [create an issue](https://github.com/Jontos/trccd/issues/new/choose).*


Usage
-----

The included systemd service reads variables from the file installed to
`/etc/trccd/trccd.conf`. Here you can set the mode and path to your
media file like so:

```sh
TRCCD_MODE=video
TRCCD_FILE=/etc/trccd/my_video.mkv
```

> [!IMPORTANT]
> *Note that the path used for `TRCCD_FILE` needs to be accessible by the dynamic
> user created by systemd, i.e. this user won't be able to read files inside
> your home directory. It is recommended to copy your media to a globally
> readable directory such as `/etc/trccd/` and specify that instead.*

Start the service with:

```sh
sudo systemctl start trccd
```

Enable the service if you'd like it to automatically start at boot:

```sh
sudo systemctl enable trccd
```

Alternatively, run this command to start and enable the service simultaneously:

```sh
sudo systemctl enable --now trccd
```

### Command Line Usage

`trccd` can also be run manually from the command line like so:

```sh
trccd <mode> [arg]
```

Where `<mode>` is one of `video`, `image` or `colour`, and `[arg]` is either a
path to some media (for the first two modes), or an RGB565 colour in hex
format (e.g. 0xF800 for red) for the latter mode.

You can also omit `[arg]` if you'd like to pipe in your own data, like this:

```sh
ffmpeg -nostdin \
-loglevel quiet \
-i /path/to/my_media.mkv \
-an -sn \
-vf "scale=320:240:force_original_aspect_ratio=increase,crop=320:240,transpose=1,fps=22" \
-f rawvideo \
-pix_fmt rgb565le - | trccd video
```

The `ffmpeg` command in the example above is the exact command `trccd` uses
under the hood for the `video` mode. Piping your own data can be useful if you'd
like to avoid the overhead of running `ffmpeg` on every invocation of `trccd`.
For example, you could instead redirect the output of the above `ffmpeg` command
into a file `raw_bytes.txt` and then use:

```sh
trccd video < raw_bytes.txt
```

This way `ffmpeg` only needs to be run once to process your media.


Caveats
-------

- Currently `trccd` will blindly process whatever data you give it and store it
  in memory without checking the size of said media. Each frame of data sent to
  the screen is 153.6 kilobytes, so a 5 second video at 22FPS will consume
  ~16.8MB of RAM. A 5 minute video, however, will consume over 1GB!
  Be aware of the length of the video you plan to use; a short loop
  is best if you want to keep memory use to a minimum.


Licence
-------

This project is licensed under the GNU General Public Licence v3 (GPLv3) - see
the [LICENCE](LICENCE) file for details.
