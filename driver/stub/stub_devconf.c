#include "stub_driver.h"
#include "stub_trace.h"
#include "stub_devconf.tmh"

#include "stub_dev.h"
#include "dbgcommon.h"
#include "stub_dbg.h"
#include "stub_usbd.h"
#include "usbdsc.h"

#include "strutil.h"

const char *dbg_info_intf(char *buf, unsigned int len, const USBD_INTERFACE_INFORMATION *info_intf)
{
	if (!info_intf) {
		return "<null>";
	}

	NTSTATUS st = RtlStringCbPrintfA(buf, len, "num:%hhu,alt:%hhu", 
		info_intf->InterfaceNumber, info_intf->AlternateSetting);

	return st == STATUS_SUCCESS ? buf : "dbg_info_intf error";
}

const char *dbg_info_pipe(char *buf, unsigned int len, const USBD_PIPE_INFORMATION *info_pipe)
{
	if (!info_pipe) {
		return "<null>";
	}

	NTSTATUS st = RtlStringCbPrintfA(buf, len, "epaddr:%#hhx", info_pipe->EndpointAddress);
	return st == STATUS_SUCCESS ? buf : "dbg_info_pipe error";
}

static PUSBD_INTERFACE_INFORMATION
dup_info_intf(PUSBD_INTERFACE_INFORMATION info_intf)
{
	PUSBD_INTERFACE_INFORMATION	info_intf_copied;
	int	size_info = INFO_INTF_SIZE(info_intf);

	info_intf_copied = ExAllocatePoolWithTag(NonPagedPool, size_info, USBIP_STUB_POOL_TAG);
	if (info_intf_copied == NULL) {
		TraceError(TRACE_GENERAL, "out of memory");
		return NULL;
	}
	RtlCopyMemory(info_intf_copied, info_intf, size_info);
	return info_intf_copied;
}

static BOOLEAN
build_infos_intf(devconf_t *devconf, PUSBD_INTERFACE_LIST_ENTRY pintf_list)
{
	unsigned	i;

	for (i = 0; i < devconf->bNumInterfaces; i++) {
		PUSBD_INTERFACE_INFORMATION	info_intf_copied = dup_info_intf(pintf_list[i].Interface);
		if (info_intf_copied == NULL) {
			TraceError(TRACE_GENERAL, "out of memory");
			return FALSE;
		}
		devconf->infos_intf[i] = info_intf_copied;
	}
	return TRUE;
}

devconf_t *
create_devconf(PUSB_CONFIGURATION_DESCRIPTOR dsc_conf, USBD_CONFIGURATION_HANDLE hconf, PUSBD_INTERFACE_LIST_ENTRY pintf_list)
{
	devconf_t	*devconf;
	int	size_devconf;

	size_devconf = sizeof(devconf_t) - sizeof(PUSBD_INTERFACE_INFORMATION) + dsc_conf->bNumInterfaces * sizeof(PUSBD_INTERFACE_INFORMATION);
	devconf = (devconf_t *)ExAllocatePoolWithTag(NonPagedPool, size_devconf, USBIP_STUB_POOL_TAG);
	if (devconf == NULL) {
		TraceError(TRACE_GENERAL, "out of memory");
		return NULL;
	}

	devconf->dsc_conf = ExAllocatePoolWithTag(NonPagedPool, dsc_conf->wTotalLength, USBIP_STUB_POOL_TAG);
	if (devconf->dsc_conf == NULL) {
		TraceError(TRACE_GENERAL, "out of memory");
		ExFreePoolWithTag(devconf, USBIP_STUB_POOL_TAG);
		return NULL;
	}
	RtlCopyMemory(devconf->dsc_conf, dsc_conf, dsc_conf->wTotalLength);

	devconf->bConfigurationValue = dsc_conf->bConfigurationValue;
	devconf->bNumInterfaces = dsc_conf->bNumInterfaces;
	devconf->hConf = hconf;
	RtlZeroMemory(devconf->infos_intf, sizeof(PUSBD_INTERFACE_INFORMATION) * devconf->bNumInterfaces);

	if (!build_infos_intf(devconf, pintf_list)) {
		free_devconf(devconf);
		return NULL;
	}

	return devconf;
}

void
free_devconf(devconf_t *devconf)
{
	unsigned	i;

	if (devconf == NULL)
		return;
	for (i = 0; i < devconf->bNumInterfaces; i++) {
		if (devconf->infos_intf[i] != NULL)
			ExFreePoolWithTag(devconf->infos_intf[i], USBIP_STUB_POOL_TAG);
	}

	ExFreePoolWithTag(devconf->dsc_conf, USBIP_STUB_POOL_TAG);
	ExFreePoolWithTag(devconf, USBIP_STUB_POOL_TAG);
}

void
update_devconf(devconf_t *devconf, PUSBD_INTERFACE_INFORMATION info_intf)
{
	PUSBD_INTERFACE_INFORMATION	info_intf_exist;

	info_intf_exist = devconf->infos_intf[info_intf->InterfaceNumber];
	if (info_intf_exist != NULL) 
		ExFreePoolWithTag(info_intf_exist, USBIP_STUB_POOL_TAG);
	devconf->infos_intf[info_intf->InterfaceNumber] = dup_info_intf(info_intf);
}

PUSBD_PIPE_INFORMATION
get_intf_info_pipe(PUSBD_INTERFACE_INFORMATION info_intf, UCHAR epaddr)
{
	unsigned	i;

	for (i = 0; i < info_intf->NumberOfPipes; i++) {
		PUSBD_PIPE_INFORMATION	info_pipe;

		info_pipe = info_intf->Pipes + i;
		if (info_pipe->EndpointAddress == epaddr)
			return info_pipe;
	}

	return NULL;
}

ULONG get_info_intf_size(devconf_t *devconf, UCHAR intf_num, UCHAR alt_setting)
{
	USB_INTERFACE_DESCRIPTOR *dsc_intf = dsc_find_intf(devconf->dsc_conf, intf_num, alt_setting);
	if (!dsc_intf) {
		return 0;
	}

	int n = dsc_intf->bNumEndpoints; // can be zero
	if (n > 1) {
		--n;
	}

	return sizeof(USBD_INTERFACE_INFORMATION) + n*sizeof(USBD_PIPE_INFORMATION);
}

USBD_PIPE_INFORMATION *get_info_pipe(devconf_t *devconf, UCHAR epaddr)
{
	if (!devconf) {
		return NULL;
	}

	for (int i = 0; i < devconf->bNumInterfaces; ++i) {

		USBD_INTERFACE_INFORMATION *info_intf = devconf->infos_intf[i];
		if (!info_intf) {
			continue;
		}

		USBD_PIPE_INFORMATION *info_pipe = get_intf_info_pipe(info_intf, epaddr);
		if (info_pipe) {
			return info_pipe;
		}
	}

	return NULL;
}