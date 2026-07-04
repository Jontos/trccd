#include <signal.h>
#include <stdio.h>

#include "usb.h"

constexpr uint16_t VID = 0x0416;
constexpr uint16_t PID = 0x5302;
constexpr uint8_t EP_OUT = 0x02;
constexpr uint8_t DISPLAY_INTERFACE = 0;

static libusb_device_handle *
usb_open_device()
{
    int ret;
    struct libusb_device **list;
    const ssize_t num_devices = libusb_get_device_list(nullptr, &list);
    if (num_devices < 0) {
        ret = (int)num_devices;
        (void)fprintf(stderr, "libusb failed to enumerate devices: %s\n",
                      libusb_strerror(ret));
        return nullptr;
    }

    struct libusb_device *found = nullptr;
    for (int i = 0; i < num_devices; ++i) {
        struct libusb_device_descriptor desc;
        struct libusb_device *device = list[i];

        ret = libusb_get_device_descriptor(device, &desc);
        if (ret != LIBUSB_SUCCESS) {
            (void)fprintf(stderr,
                          "libusb failed to get device descriptor: %s\n",
                          libusb_strerror(ret));
            continue;
        }

        if (desc.idVendor == VID && desc.idProduct == PID) {
            found = device;
            break;
        }
    }

    libusb_device_handle *device_handle = nullptr;
    if (found) {
        ret = libusb_open(found, &device_handle);
        if (ret != LIBUSB_SUCCESS) {
            (void)fprintf(stderr, "libusb failed to open device: %s\n",
                          libusb_strerror(ret));
        }
    } else {
        (void)fprintf(stderr, "Error: Could not find device!\n");
    }
    libusb_free_device_list(list, 1);

    return device_handle;
}

libusb_device_handle *usb_init()
{
    int ret = libusb_init_context(nullptr, nullptr, 0);
    if (ret != LIBUSB_SUCCESS) {
        (void)fprintf(stderr, "libusb failed to initialise context: %s\n",
                      libusb_strerror(ret));
        return nullptr;
    }

    libusb_device_handle *device_handle = usb_open_device();
    if (!device_handle) {
        libusb_exit(nullptr);
        return nullptr;
    }

    libusb_set_auto_detach_kernel_driver(device_handle, 1);

    ret = libusb_claim_interface(device_handle, DISPLAY_INTERFACE);
    if (ret != LIBUSB_SUCCESS) {
        (void)fprintf(stderr, "libusb failed to claim interface: %s\n",
                      libusb_strerror(ret));
        libusb_close(device_handle);
        libusb_exit(nullptr);
        return nullptr;
    }

    return device_handle;
}

void usb_release(libusb_device_handle **dev)
{
    if (!*dev) return;

    const int ret = libusb_release_interface(*dev, DISPLAY_INTERFACE);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb failed to release device: %s\n",
                      libusb_strerror(ret));
    }

    libusb_close(*dev);
    libusb_exit(nullptr);
}

int usb_send_data(uint8_t *data, const int length, libusb_device_handle *dev)
{
    const int ret =
        libusb_interrupt_transfer(dev, EP_OUT, data, length, nullptr, 5000);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb failed to send data: %s\n",
                      libusb_strerror(ret));
    }

    return ret;
}

int usb_send_header(libusb_device_handle *dev)
{
    union header {
        struct [[gnu::packed]] {
            uint32_t magic;
            uint32_t command;
            uint16_t width;
            uint16_t height;
            uint32_t unknown;
            uint32_t length;
        };
        uint8_t padding[PACKET_SIZE];
    };

    return usb_send_data(
        (uint8_t*)&(union header){
            .magic = 0xDDDCDBDA,
            .command = 0x00010002,
            .width = LCD_WIDTH,
            .height = LCD_HEIGHT,
            .unknown = 2,
            .length = PACKET_SIZE
        },
    PACKET_SIZE, dev);
}

extern volatile sig_atomic_t keep_running;

int usb_keep_alive(libusb_device_handle *dev)
{
    while (keep_running) {
        if (usb_send_data((uint8_t[PACKET_SIZE]){}, PACKET_SIZE, dev) < 0) {
            return -1;
        }
        // 2.5s is the longest interval between transfers that still keeps the
        // connection alive
        nanosleep(&(struct timespec){
            .tv_nsec = 500000000,
            .tv_sec = 2
        }, nullptr);
    }

    return 0;
}
