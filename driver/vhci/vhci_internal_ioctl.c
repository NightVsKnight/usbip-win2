#include "vhci_internal_ioctl.h"
#include "dbgcommon.h"
#include "trace.h"
#include "vhci_internal_ioctl.tmh"

#include "usbreq.h"
#include "vhci_irp.h"

#include <ntstrsafe.h>

const NTSTATUS STATUS_SUBMIT_URBR_IRP = -1L;

enum { USBD_INTERFACE_STR_BUFSZ = 1024 };

static const char *usbd_interfaces_str(char *buf, size_t len, const USBD_INTERFACE_INFORMATION *r, int cnt)
{
	NTSTATUS st = STATUS_SUCCESS;

	for (int i = 0; i < cnt && st == STATUS_SUCCESS; ++i) {

		st = RtlStringCbPrintfExA(buf, len, &buf, &len, STRSAFE_NULL_ON_FAILURE,
			"\nInterface(Length %d, InterfaceNumber %d, AlternateSetting %d, "
			"Class %#02hhx, SubClass %#02hhx, Protocol %#02hhx, InterfaceHandle %#Ix, NumberOfPipes %lu)", 
			r->Length, 
			r->InterfaceNumber,
			r->AlternateSetting,
			r->Class,
			r->SubClass,
			r->Protocol,
			(uintptr_t)r->InterfaceHandle,
			r->NumberOfPipes);

		for (ULONG j = 0; j < r->NumberOfPipes && st == STATUS_SUCCESS; ++j) {

			const USBD_PIPE_INFORMATION *p = r->Pipes + j;

			st = RtlStringCbPrintfExA(buf, len, &buf, &len, STRSAFE_NULL_ON_FAILURE,
				"\nPipes[%lu](MaximumPacketSize %#x, EndpointAddress %#02hhx(%s#%d), Interval %#hhx, %s, "
				"PipeHandle %#Ix, MaximumTransferSize %#lx, PipeFlags %#lx)",
				j,
				p->MaximumPacketSize,
				p->EndpointAddress,
				USB_ENDPOINT_DIRECTION_IN(p->EndpointAddress) ? "in" : "out",
				p->EndpointAddress & USB_ENDPOINT_ADDRESS_MASK,
				p->Interval,
				usbd_pipe_type_str(p->PipeType),
				(uintptr_t)p->PipeHandle,
				p->MaximumTransferSize,
				p->PipeFlags);
		}

		r = get_next_interface(r);
	}

	return buf && *buf ? buf : "usbd_interface_str error"; 
}

static NTSTATUS submit_urbr_irp(vpdo_dev_t* vpdo, IRP* irp)
{
	struct urb_req* urbr = create_urbr(vpdo, irp, 0);
	if (!urbr) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	NTSTATUS status = submit_urbr(vpdo, urbr);
	if (NT_ERROR(status)) {
		free_urbr(urbr);
	}

	return status;
}

NTSTATUS vhci_ioctl_abort_pipe(vpdo_dev_t *vpdo, USBD_PIPE_HANDLE hPipe)
{
	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix", (uintptr_t)hPipe);

	if (!hPipe) {
		return STATUS_INVALID_PARAMETER;
	}

	KIRQL oldirql;
	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);

	// remove all URBRs of the aborted pipe
	for (LIST_ENTRY *le = vpdo->head_urbr.Flink; le != &vpdo->head_urbr; ) {
		struct urb_req	*urbr_local = CONTAINING_RECORD(le, struct urb_req, list_all);
		le = le->Flink;

		if (!is_port_urbr(urbr_local->irp, hPipe)) {
			continue;
		}

		{
			char buf[URB_REQ_STR_BUFSZ];
			TraceInfo(TRACE_IOCTL, "aborted urbr removed %s", urb_req_str(buf, sizeof(buf), urbr_local));
		}

		if (urbr_local->irp) {
			PIRP irp = urbr_local->irp;

			KIRQL oldirql_cancel;
			IoAcquireCancelSpinLock(&oldirql_cancel);
			BOOLEAN	valid_irp = IoSetCancelRoutine(irp, NULL) != NULL;
			IoReleaseCancelSpinLock(oldirql_cancel);

			if (valid_irp) {
				irp->IoStatus.Information = 0;
				irp_done(irp, STATUS_CANCELLED);
			}
		}
		RemoveEntryListInit(&urbr_local->list_state);
		RemoveEntryListInit(&urbr_local->list_all);
		free_urbr(urbr_local);
	}

	KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

	return STATUS_SUCCESS;
}

static NTSTATUS urb_control_get_status_request(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_CONTROL_GET_STATUS_REQUEST *r = &urb->UrbControlGetStatusRequest;
	
	TraceVerbose(TRACE_IOCTL, "%s: TransferBufferLength %lu (must be 2), Index %hd", 
		urb_function_str(r->Hdr.Function), r->TransferBufferLength, r->Index);
	
	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_control_vendor_class_request(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *r = &urb->UrbControlVendorClassRequest;
	char buf[USBD_TRANSFER_FLAGS_BUFBZ];

	TraceVerbose(TRACE_IOCTL, "%s: %s, TransferBufferLength %lu, %s(%!#XBYTE!), Value %#hx, Index %#hx",
		urb_function_str(r->Hdr.Function), usbd_transfer_flags(buf, sizeof(buf), r->TransferFlags), 
		r->TransferBufferLength, brequest_str(r->Request), r->Request, r->Value, r->Index);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_control_descriptor_request(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	
	struct _URB_CONTROL_DESCRIPTOR_REQUEST *r = &urb->UrbControlDescriptorRequest;

	TraceVerbose(TRACE_IOCTL, "%s: TransferBufferLength %lu, Index %d, %!usb_descriptor_type!, LanguageId %#04hx",
		urb_function_str(r->Hdr.Function), r->TransferBufferLength, r->Index, r->DescriptorType, r->LanguageId);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_control_feature_request(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	
	struct _URB_CONTROL_FEATURE_REQUEST *r = &urb->UrbControlFeatureRequest;

	TraceVerbose(TRACE_IOCTL, "%s: FeatureSelector %#hx, Index %#hx", 
		urb_function_str(r->Hdr.Function), r->FeatureSelector, r->Index);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_select_configuration(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	
	struct _URB_SELECT_CONFIGURATION *r = &urb->UrbSelectConfiguration;
	USB_CONFIGURATION_DESCRIPTOR *cd = r->ConfigurationDescriptor;

	if (cd) {
		char buf[USBD_INTERFACE_STR_BUFSZ];

		TraceVerbose(TRACE_IOCTL, "ConfigurationHandle %#Ix, "
			"ConfigurationDescriptor(bLength %d, %!usb_descriptor_type!, wTotalLength %hu, bNumInterfaces %d, "
			"bConfigurationValue %d, iConfiguration %d, bmAttributes %!#XBYTE!, MaxPower %d)%s",
			(uintptr_t)r->ConfigurationHandle,
			cd->bLength,
			cd->bDescriptorType,
			cd->wTotalLength,
			cd->bNumInterfaces,
			cd->bConfigurationValue,
			cd->iConfiguration,
			cd->bmAttributes,
			cd->MaxPower,
			usbd_interfaces_str(buf, sizeof(buf), &r->Interface, cd->bNumInterfaces));
	} else {
		TraceVerbose(TRACE_IOCTL, "ConfigurationHandle %#Ix, ConfigurationDescriptor NULL (unconfigured)", 
				(uintptr_t)r->ConfigurationHandle);
	}

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_select_interface(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	
	struct _URB_SELECT_INTERFACE *r = &urb->UrbSelectInterface;
	char buf[USBD_INTERFACE_STR_BUFSZ];

	TraceVerbose(TRACE_IOCTL, "ConfigurationHandle %#Ix%s", (uintptr_t)r->ConfigurationHandle, 
			usbd_interfaces_str(buf, sizeof(buf), &r->Interface, 1));

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS urb_pipe_request(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_PIPE_REQUEST *r = &urb->UrbPipeRequest;

	TraceVerbose(TRACE_IOCTL, "%s: PipeHandle %#Ix(EndpointAddress %#02x, %!USBD_PIPE_TYPE!, Interval %d)",
			urb_function_str(r->Hdr.Function), 
			(uintptr_t)r->PipeHandle, 
			get_endpoint_address(r->PipeHandle), 
			get_endpoint_type(r->PipeHandle),
			get_endpoint_interval(r->PipeHandle));

	NTSTATUS st = STATUS_NOT_SUPPORTED;

	switch (urb->UrbHeader.Function) {
	case URB_FUNCTION_ABORT_PIPE:
		st = vhci_ioctl_abort_pipe(vpdo, r->PipeHandle);
		break;
	case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		st = STATUS_SUBMIT_URBR_IRP;
		break;
	case URB_FUNCTION_SYNC_RESET_PIPE:
	case URB_FUNCTION_SYNC_CLEAR_STALL:
	case URB_FUNCTION_CLOSE_STATIC_STREAMS:
		break;
	}

	return st;
}

static NTSTATUS urb_get_current_frame_number(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	urb->UrbGetCurrentFrameNumber.FrameNumber = 0;
	TraceVerbose(TRACE_IOCTL, "FrameNumber %lu", urb->UrbGetCurrentFrameNumber.FrameNumber);

	return STATUS_SUCCESS;
}

static NTSTATUS urb_control_transfer(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_CONTROL_TRANSFER *r = &urb->UrbControlTransfer;

	char buf_flags[USBD_TRANSFER_FLAGS_BUFBZ];
	char buf_setup[USB_SETUP_PKT_STR_BUFBZ];

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, %s, TransferBufferLength %lu, %s",
		(uintptr_t)r->PipeHandle, 
		usbd_transfer_flags(buf_flags, sizeof(buf_flags), r->TransferFlags),
		r->TransferBufferLength,
		usb_setup_pkt_str(buf_setup, sizeof(buf_setup), r->SetupPacket));

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS bulk_or_interrupt_transfer(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_BULK_OR_INTERRUPT_TRANSFER *r = &urb->UrbBulkOrInterruptTransfer;

	const char *func = urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL ? 
				", using chained mdl" : "";

	char buf[USBD_TRANSFER_FLAGS_BUFBZ];

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, %s, TransferBufferLength %lu%s",
			(uintptr_t)r->PipeHandle,
			usbd_transfer_flags(buf, sizeof(buf), r->TransferFlags),
			r->TransferBufferLength,
			func);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS isoch_transfer(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_ISOCH_TRANSFER *r = &urb->UrbIsochronousTransfer;

	const char *func = urb->UrbHeader.Function == URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL ? 
				", using chained mdl" : "";

	char buf[USBD_TRANSFER_FLAGS_BUFBZ];

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, %s, TransferBufferLength %lu, StartFrame %lu, NumberOfPackets %lu, ErrorCount %lu%s",
			(uintptr_t)r->PipeHandle,	
			usbd_transfer_flags(buf, sizeof(buf), r->TransferFlags),
			r->TransferBufferLength, 
			r->StartFrame, 
			r->NumberOfPackets, 
			r->ErrorCount,
			func);

	return STATUS_SUBMIT_URBR_IRP;
}
static NTSTATUS urb_control_transfer_ex(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_CONTROL_TRANSFER_EX* r = &urb->UrbControlTransferEx;

	char buf_flags[USBD_TRANSFER_FLAGS_BUFBZ];
	char buf_setup[USB_SETUP_PKT_STR_BUFBZ];

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, %s, TransferBufferLength %lu, Timeout %lu, %s",
		(uintptr_t)r->PipeHandle,
		usbd_transfer_flags(buf_flags, sizeof(buf_flags), r->TransferFlags),
		r->TransferBufferLength,
		r->Timeout,
		usb_setup_pkt_str(buf_setup, sizeof(buf_setup), r->SetupPacket));

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS usb_function_deprecated(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);
	UNREFERENCED_PARAMETER(urb);
	return STATUS_NOT_SUPPORTED;
}

static NTSTATUS get_configuration(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *r = &urb->UrbControlGetConfigurationRequest;
	TraceVerbose(TRACE_IOCTL, "TransferBufferLength %lu (must be 1)", r->TransferBufferLength);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS get_interface(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_CONTROL_GET_INTERFACE_REQUEST *r = &urb->UrbControlGetInterfaceRequest;
	TraceVerbose(TRACE_IOCTL, "TransferBufferLength %lu (must be 1), Interface %hu", r->TransferBufferLength, r->Interface);

	return STATUS_SUBMIT_URBR_IRP;
}

static NTSTATUS get_ms_feature_descriptor(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST *r = &urb->UrbOSFeatureDescriptorRequest;

	TraceVerbose(TRACE_IOCTL, "TransferBufferLength %lu, Recipient %d, InterfaceNumber %d, MS_PageIndex %d, MS_FeatureDescriptorIndex %d", 
			r->TransferBufferLength, r->Recipient, r->InterfaceNumber, r->MS_PageIndex, r->MS_FeatureDescriptorIndex);

	return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_isoch_pipe_transfer_path_delays(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS *r = &urb->UrbGetIsochPipeTransferPathDelays;

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, MaximumSendPathDelayInMilliSeconds %lu, MaximumCompletionPathDelayInMilliSeconds %lu",
				(uintptr_t)r->PipeHandle, 
				r->MaximumSendPathDelayInMilliSeconds, 
				r->MaximumCompletionPathDelayInMilliSeconds);

	return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS open_static_streams(vpdo_dev_t *vpdo, URB *urb)
{
	UNREFERENCED_PARAMETER(vpdo);

	struct _URB_OPEN_STATIC_STREAMS *r = &urb->UrbOpenStaticStreams;

	TraceVerbose(TRACE_IOCTL, "PipeHandle %#Ix, NumberOfStreams %lu, StreamInfoVersion %hu, StreamInfoSize %hu",
				(uintptr_t)r->PipeHandle, r->NumberOfStreams, r->StreamInfoVersion, r->StreamInfoSize);

	return STATUS_NOT_IMPLEMENTED;
}

typedef NTSTATUS (*urb_function_t)(vpdo_dev_t*, URB*);

static const urb_function_t urb_functions[] =
{
	urb_select_configuration,
	urb_select_interface,
	urb_pipe_request, // URB_FUNCTION_ABORT_PIPE

	usb_function_deprecated, // URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL
	usb_function_deprecated, // URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL

	usb_function_deprecated, // URB_FUNCTION_GET_FRAME_LENGTH
	usb_function_deprecated, // URB_FUNCTION_SET_FRAME_LENGTH
	urb_get_current_frame_number,

	urb_control_transfer,
	bulk_or_interrupt_transfer,
	isoch_transfer,

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE

	urb_control_feature_request, // URB_FUNCTION_SET_FEATURE_TO_DEVICE
	urb_control_feature_request, // URB_FUNCTION_SET_FEATURE_TO_INTERFACE
	urb_control_feature_request, // URB_FUNCTION_SET_FEATURE_TO_ENDPOINT

	urb_control_feature_request, // URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE
	urb_control_feature_request, // URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE
	urb_control_feature_request, // URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT

	urb_control_get_status_request, // URB_FUNCTION_GET_STATUS_FROM_DEVICE
	urb_control_get_status_request, // URB_FUNCTION_GET_STATUS_FROM_INTERFACE
	urb_control_get_status_request, // URB_FUNCTION_GET_STATUS_FROM_ENDPOINT

	NULL, // URB_FUNCTION_RESERVED_0X0016          

	urb_control_vendor_class_request, // URB_FUNCTION_VENDOR_DEVICE
	urb_control_vendor_class_request, // URB_FUNCTION_VENDOR_INTERFACE
	urb_control_vendor_class_request, // URB_FUNCTION_VENDOR_ENDPOINT

	urb_control_vendor_class_request, // URB_FUNCTION_CLASS_DEVICE 
	urb_control_vendor_class_request, // URB_FUNCTION_CLASS_INTERFACE
	urb_control_vendor_class_request, // URB_FUNCTION_CLASS_ENDPOINT

	NULL, // URB_FUNCTION_RESERVE_0X001D

	urb_pipe_request, // URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL

	urb_control_vendor_class_request, // URB_FUNCTION_CLASS_OTHER
	urb_control_vendor_class_request, // URB_FUNCTION_VENDOR_OTHER

	urb_control_get_status_request, // URB_FUNCTION_GET_STATUS_FROM_OTHER

	urb_control_feature_request, // URB_FUNCTION_SET_FEATURE_TO_OTHER
	urb_control_feature_request, // URB_FUNCTION_CLEAR_FEATURE_TO_OTHER

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT

	get_configuration, // URB_FUNCTION_GET_CONFIGURATION
	get_interface, // URB_FUNCTION_GET_INTERFACE

	urb_control_descriptor_request, // URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE
	urb_control_descriptor_request, // URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE

	get_ms_feature_descriptor,

	NULL, // URB_FUNCTION_RESERVE_0X002B
	NULL, // URB_FUNCTION_RESERVE_0X002C
	NULL, // URB_FUNCTION_RESERVE_0X002D
	NULL, // URB_FUNCTION_RESERVE_0X002E
	NULL, // URB_FUNCTION_RESERVE_0X002F

	urb_pipe_request, // URB_FUNCTION_SYNC_RESET_PIPE
	urb_pipe_request, // URB_FUNCTION_SYNC_CLEAR_STALL
	urb_control_transfer_ex,

	NULL, // URB_FUNCTION_RESERVE_0X0033
	NULL, // URB_FUNCTION_RESERVE_0X0034                  

	open_static_streams,
	urb_pipe_request, // URB_FUNCTION_CLOSE_STATIC_STREAMS
	bulk_or_interrupt_transfer, // URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL
	isoch_transfer, // URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL

	NULL, // 0x0039
	NULL, // 0x003A        
	NULL, // 0x003B
	NULL, // 0x003C        

	get_isoch_pipe_transfer_path_delays // URB_FUNCTION_GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS
};

static NTSTATUS process_irp_urb_req(vpdo_dev_t *vpdo, IRP *irp)
{
	URB *urb = URB_FROM_IRP(irp);
	if (!urb) {
		TraceError(TRACE_IOCTL, "null urb");
		return STATUS_INVALID_PARAMETER;
	}

	USHORT func = urb->UrbHeader.Function;
	urb_function_t pfunc = func < ARRAYSIZE(urb_functions) ? urb_functions[func] : NULL;

	if (pfunc) {
		NTSTATUS st = pfunc(vpdo, urb);
		return st == STATUS_SUBMIT_URBR_IRP ? submit_urbr_irp(vpdo, irp) : st;
	}

	TraceError(TRACE_IOCTL, "%s(%#04x) has no handler (reserved?)", urb_function_str(func), func);
	return STATUS_INVALID_PARAMETER;
}

static NTSTATUS setup_topology_address(vpdo_dev_t *vpdo, USB_TOPOLOGY_ADDRESS *r)
{
	r->RootHubPortNumber = (USHORT)vpdo->port;
	TraceVerbose(TRACE_IOCTL, "RootHubPortNumber %d", r->RootHubPortNumber);
	return STATUS_SUCCESS;
}

static NTSTATUS usb_get_port_status(ULONG *status)
{
	*status = USBD_PORT_ENABLED | USBD_PORT_CONNECTED;
	TraceVerbose(TRACE_IOCTL, "-> PORT_ENABLED|PORT_CONNECTED"); 
	return STATUS_SUCCESS;
}

NTSTATUS vhci_internal_ioctl(__in PDEVICE_OBJECT devobj, __in PIRP Irp)
{
	IO_STACK_LOCATION *irpStack = IoGetCurrentIrpStackLocation(Irp);
	ULONG ioctl_code = irpStack->Parameters.DeviceIoControl.IoControlCode;

	TraceVerbose(TRACE_IOCTL, "Enter irql %!irql!, %s(%#08lX)", 
			KeGetCurrentIrql(), dbg_ioctl_code(ioctl_code), ioctl_code);

	vpdo_dev_t *vpdo = devobj_to_vpdo_or_null(devobj);
	if (!vpdo) {
		TraceWarning(TRACE_IOCTL, "internal ioctl only for vpdo is allowed");
		return irp_done(Irp, STATUS_INVALID_DEVICE_REQUEST);
	}

	if (!vpdo->plugged) {
		NTSTATUS st = STATUS_DEVICE_NOT_CONNECTED;
		TraceVerbose(TRACE_IOCTL, "%!STATUS!", st);
		return irp_done(Irp, st);
	}

	NTSTATUS status = STATUS_NOT_SUPPORTED;

	switch (ioctl_code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		status = process_irp_urb_req(vpdo, Irp);
		break;
	case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
		status = usb_get_port_status(irpStack->Parameters.Others.Argument1);
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
		status = submit_urbr_irp(vpdo, Irp);
		break;
	case IOCTL_INTERNAL_USB_GET_TOPOLOGY_ADDRESS:
		status = setup_topology_address(vpdo, irpStack->Parameters.Others.Argument1);
		break;
	default:
		TraceWarning(TRACE_IOCTL, "Unhandled %s(%#08lX)", dbg_ioctl_code(ioctl_code), ioctl_code);
	}

	if (status != STATUS_PENDING) {
		Irp->IoStatus.Information = 0;
		irp_done(Irp, status);
	}

	TraceVerbose(TRACE_IOCTL, "Leave %!STATUS!", status);
	return status;
}
