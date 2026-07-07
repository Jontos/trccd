#ifndef USB_H
#define USB_H

#include <libusb-1.0/libusb.h>

constexpr uint16_t LCD_WIDTH = 320;
constexpr uint16_t LCD_HEIGHT = 240;

constexpr size_t PACKET_SIZE = 512;

libusb_device_handle *usb_init();

int usb_send_data(uint8_t *data, int length, libusb_device_handle *dev);
int usb_send_header(libusb_device_handle *dev);

void usb_release(libusb_device_handle **dev);

#endif // USB_H
