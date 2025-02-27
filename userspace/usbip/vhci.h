#pragma once

#include <vector>

#include <usbip\vhci.h>
#include <libusbip\win_handle.h>

namespace usbip
{

Handle vhci_driver_open(hci_version version);

std::vector<ioctl_usbip_vhci_imported_dev> vhci_get_imported_devs(HANDLE hdev);

bool vhci_attach_device(HANDLE hdev, ioctl_usbip_vhci_plugin &r);
int vhci_detach_device(HANDLE hdev, int port);

} // namespace usbip
