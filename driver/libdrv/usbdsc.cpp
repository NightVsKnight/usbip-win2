#include "usbdsc.h"

extern "C" {
#include <usbdlib.h>
}

USB_COMMON_DESCRIPTOR *dsc_find_next(USB_CONFIGURATION_DESCRIPTOR *dsc_conf, USB_COMMON_DESCRIPTOR *from, int type)
{
	NT_ASSERT(dsc_conf);

	auto start = dsc_next(from ? from : (USB_COMMON_DESCRIPTOR*)dsc_conf);
	NT_ASSERT(start > (USB_COMMON_DESCRIPTOR*)dsc_conf);

	return USBD_ParseDescriptors(dsc_conf, dsc_conf->wTotalLength, start, type);
}

USB_INTERFACE_DESCRIPTOR *dsc_find_intf(USB_CONFIGURATION_DESCRIPTOR *dsc_conf, UCHAR intf_num, UCHAR alt_setting)
{
	NT_ASSERT(dsc_conf);
	return USBD_ParseConfigurationDescriptorEx(dsc_conf, dsc_conf, intf_num, alt_setting, -1, -1, -1);
}

/*
 * @return number of alternate settings for given interface
 */
int get_intf_num_altsetting(USB_CONFIGURATION_DESCRIPTOR *dsc_conf, UCHAR intf_num)
{
	int cnt = 0;
	void *from = dsc_conf;

	while (auto iface = USBD_ParseConfigurationDescriptorEx(dsc_conf, from, intf_num, -1, -1, -1, -1)) {
		from = dsc_next(reinterpret_cast<USB_COMMON_DESCRIPTOR*>(iface));
		++cnt;
	}

	return cnt;
}

NTSTATUS for_each_endpoint(USB_CONFIGURATION_DESCRIPTOR *cfg, USB_INTERFACE_DESCRIPTOR *iface, for_each_ep_fn &func, void *data)
{
	auto cur = reinterpret_cast<USB_COMMON_DESCRIPTOR*>(iface);

	for (int i = 0; i < iface->bNumEndpoints; ++i) {

		cur = dsc_find_next(cfg, cur, USB_ENDPOINT_DESCRIPTOR_TYPE);
		if (!cur) {
			NT_ASSERT(!"Endpoint not found");
			return STATUS_NO_MORE_MATCHES;
		}

		if (auto err = func(i, *reinterpret_cast<USB_ENDPOINT_DESCRIPTOR*>(cur), data)) {
			return err;
		}
	}

	return STATUS_SUCCESS;
}

bool is_valid(const USB_DEVICE_DESCRIPTOR &d)
{
        return  d.bLength == sizeof(d) && 
		d.bDescriptorType == USB_DEVICE_DESCRIPTOR_TYPE;
}

bool is_valid(const USB_CONFIGURATION_DESCRIPTOR &d)
{
        return  d.bLength == sizeof(d) &&
                d.bDescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE &&
                d.wTotalLength > d.bLength;
}

bool is_valid(const USB_STRING_DESCRIPTOR &d)
{
	return  d.bLength >= sizeof(USB_COMMON_DESCRIPTOR) && // string length can be zero
		d.bDescriptorType == USB_STRING_DESCRIPTOR_TYPE;
}

bool is_valid(const USB_OS_STRING_DESCRIPTOR &d)
{
	return  d.bLength == sizeof(d) && 
		d.bDescriptorType == USB_STRING_DESCRIPTOR_TYPE && 
		RtlEqualMemory(d.Signature, L"MSFT100", sizeof(d.Signature));
}
