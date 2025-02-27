#include "dev.h"
#include "trace.h"
#include "dev.tmh"
#include "vhci.h"
#include "irp.h"

#include <libdrv\pageable.h>

#include <limits.h>
#include <wsk.h>
#include <wdmsec.h>
#include <initguid.h> // required for GUID definitions

DEFINE_GUID(GUID_SD_USBIP_VHCI,
	0x856F2BDE, 0x9770, 0x429C, 0x9B, 0xDD, 0x88, 0x2F, 0xCC, 0x32, 0xEA, 0xFE);

_IRQL_requires_(PASSIVE_LEVEL)
void *GetDeviceProperty(DEVICE_OBJECT *obj, DEVICE_REGISTRY_PROPERTY prop, NTSTATUS &error, ULONG &ResultLength)
{
	ResultLength = 256;
	auto alloc = [] (auto len) { return ExAllocatePool2(POOL_FLAG_PAGED|POOL_FLAG_UNINITIALIZED, len, USBIP_VHCI_POOL_TAG); };

	for (auto buf = alloc(ResultLength); buf; ) {
		
		error = IoGetDeviceProperty(obj, prop, ResultLength, buf, &ResultLength);
		
		switch (error) {
		case STATUS_SUCCESS:
			return buf;
		case STATUS_BUFFER_TOO_SMALL:
			ExFreePoolWithTag(buf, USBIP_VHCI_POOL_TAG);
			buf = alloc(ResultLength);
			break;
		default:
			TraceDbg("%!DEVICE_REGISTRY_PROPERTY! %!STATUS!", prop, error);
			ExFreePoolWithTag(buf, USBIP_VHCI_POOL_TAG);
			return nullptr;
		}
	}

	error = USBD_STATUS_INSUFFICIENT_RESOURCES;
	return nullptr;
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE DEVICE_OBJECT *vdev_create(_In_ DRIVER_OBJECT *drvobj, _In_ hci_version version, _In_ vdev_type_t type)
{
	PAGED_CODE();

        const ULONG ext_sizes[] = 
        {
                sizeof(root_dev_t),
                sizeof(cpdo_dev_t),
                sizeof(vhci_dev_t),
                sizeof(hpdo_dev_t),
                sizeof(vhub_dev_t),
                sizeof(vpdo_dev_t)
        };
        static_assert(ARRAYSIZE(ext_sizes) == VDEV_SIZE);
	auto extsize = ext_sizes[type];

	DEVICE_OBJECT *devobj{};
	NTSTATUS status{};

	switch (type) {
	case VDEV_CPDO:
	case VDEV_HPDO:
	case VDEV_VPDO:
		status = IoCreateDeviceSecure(drvobj, extsize, nullptr,
				FILE_DEVICE_BUS_EXTENDER, FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
				false, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX, // allow normal users to access the devices
				&GUID_SD_USBIP_VHCI, &devobj);
		break;
	default:
		status = IoCreateDevice(drvobj, extsize, nullptr,
					FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN, true, &devobj);
	}

	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, "Failed to create vdev(%!vdev_type_t!): %!STATUS!", type, status);
		return nullptr;
	}

	auto vdev = to_vdev(devobj);
        vdev->Self = devobj;
	
	vdev->version = version;
	vdev->type = type;

        NT_ASSERT(vdev->PnPState == pnp_state::NotStarted);
        NT_ASSERT(vdev->PreviousPnPState == pnp_state::NotStarted);

        vdev->SystemPowerState = PowerSystemWorking;
        NT_ASSERT(vdev->DevicePowerState == PowerDeviceUnspecified);

	devobj->Flags |= DO_POWER_PAGABLE | DO_BUFFERED_IO;

	TraceDbg("%04x %!hci_version!, %!vdev_type_t!", ptr4log(devobj), version, type);
	return devobj;
}

vhub_dev_t *vhub_from_vhci(vhci_dev_t *vhci)
{	
	NT_ASSERT(vhci);
	auto child_pdo = vhci->child_pdo;
	return child_pdo ? static_cast<vhub_dev_t*>(child_pdo->fdo) : nullptr;
}

vhci_dev_t *to_vhci_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_VHCI ? static_cast<vhci_dev_t*>(vdev) : nullptr;
}

vpdo_dev_t *to_vpdo_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_VPDO ? static_cast<vpdo_dev_t*>(vdev) : nullptr;
}

/*
 * First bit is reserved for direction of transfer (USBIP_DIR_OUT|USBIP_DIR_IN).
 * @see is_valid_seqnum
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
seqnum_t next_seqnum(vpdo_dev_t &vpdo, bool dir_in)
{
	static_assert(!USBIP_DIR_OUT);
	static_assert(USBIP_DIR_IN);

	static_assert(sizeof(vpdo.seqnum) == sizeof(LONG));

	while (true) {
		if (seqnum_t num = InterlockedIncrement(reinterpret_cast<LONG*>(&vpdo.seqnum)) << 1) {
			return num |= seqnum_t(dir_in);
		}
	}
}

/*
 * Zero string index means absense of a descriptor.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
PCWSTR get_string_descr_str(const vpdo_dev_t &vpdo, UCHAR index)
{
	PCWSTR str{};

	if (index && index < ARRAYSIZE(vpdo.strings)) {
		if (volatile auto &d = vpdo.strings[index]) {
			str = d->bString;
		}
	}

	return str;
}
