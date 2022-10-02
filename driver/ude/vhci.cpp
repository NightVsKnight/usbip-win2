/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "vhci.h"
#include "trace.h"
#include "vhci.tmh"

#include <ntstrsafe.h>

#include <usb.h>
#include <usbdlib.h>
#include <usbiodef.h>

#include <wdfusb.h>
#include <Udecx.h>

#include "device.h"
#include "vhci_ioctl.h"
#include "context.h"

namespace
{

using namespace usbip;

_Function_class_(EVT_WDF_DEVICE_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void vhci_cleanup(_In_ WDFOBJECT Object)
{
        PAGED_CODE(); // WDF calls the callback at PASSIVE_LEVEL if object's handle type is WDFDEVICE

        auto vhci = static_cast<WDFDEVICE>(Object);
        TraceDbg("vhci %04x", ptr04x(vhci));

        vhci::destroy_all_devices(vhci);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
auto create_interfaces(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        const GUID* v[] = {
                &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
                &vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER
        };

        for (auto guid: v) {
                if (auto err = WdfDeviceCreateDeviceInterface(vhci, guid, nullptr)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreateDeviceInterface(%!GUID!) %!STATUS!", guid, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY)
_IRQL_requires_same_
NTSTATUS query_usb_capability(
        _In_ WDFDEVICE /*UdecxWdfDevice*/,
        _In_ GUID *CapabilityType,
        _In_ ULONG /*OutputBufferLength*/,
        _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID /*OutputBuffer*/,
        _Out_ ULONG *ResultLength)
{
        const GUID* supported[] = {
                &GUID_USB_CAPABILITY_CHAINED_MDLS, 
                &GUID_USB_CAPABILITY_SELECTIVE_SUSPEND, // class extension reports it as supported without invoking the callback
                &GUID_USB_CAPABILITY_FUNCTION_SUSPEND,
                &GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE, 
                &GUID_USB_CAPABILITY_DEVICE_CONNECTION_SUPER_SPEED_COMPATIBLE 
        };

        auto st = STATUS_NOT_SUPPORTED;

        for (auto i: supported) {
                if (*i == *CapabilityType) {
                        st = STATUS_SUCCESS;
                        break;
                }
        }

        *ResultLength = 0;
        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto initialize(_Inout_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        WDF_PNPPOWER_EVENT_CALLBACKS pnp_power;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power);
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power);

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle_settings;
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle_settings, IdleUsbSelectiveSuspend); // IdleCanWakeFromS0

        WDF_OBJECT_ATTRIBUTES request_attrs;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&request_attrs, request_ctx);
        WdfDeviceInitSetRequestAttributes(DeviceInit, &request_attrs);

        WDF_FILEOBJECT_CONFIG fobj_cfg;
        WDF_FILEOBJECT_CONFIG_INIT(&fobj_cfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
        fobj_cfg.FileObjectClass = WdfFileObjectNotRequired;
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &fobj_cfg, WDF_NO_OBJECT_ATTRIBUTES);

        WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, true);

        if (auto err = WdfDeviceInitAssignSDDLString(DeviceInit, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceInitAssignSDDLString %!STATUS!", err);
                return err;
        }

        if (auto err = UdecxInitializeWdfDeviceInit(DeviceInit)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxInitializeWdfDeviceInit %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto init_context(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        auto &ctx = *get_vhci_ctx(vhci);

        WDF_OBJECT_ATTRIBUTES attrs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = vhci;

        if (auto err = WdfSpinLockCreate(&attrs, &ctx.devices_lock)) {
                Trace(TRACE_LEVEL_ERROR, "WdfSpinLockCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto add_usbdevice_emulation(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        UDECX_WDF_DEVICE_CONFIG cfg;
        UDECX_WDF_DEVICE_CONFIG_INIT(&cfg, query_usb_capability);

        cfg.NumberOfUsb20Ports = vhci::USB2_PORTS;
        cfg.NumberOfUsb30Ports = vhci::USB3_PORTS;

        if (auto err = UdecxWdfDeviceAddUsbDeviceEmulation(vhci, &cfg)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxWdfDeviceAddUsbDeviceEmulation %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto create_vhci(_Out_ WDFDEVICE &vhci, _In_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attrs; // default parent (WDFDRIVER) is OK
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, vhci_ctx);
        attrs.EvtCleanupCallback = vhci_cleanup;

        if (auto err = WdfDeviceCreate(&DeviceInit, &attrs, &vhci)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreate %!STATUS!", err);
                return err;
        }

        using func_t = NTSTATUS(WDFDEVICE);
        func_t *functions[] { init_context, create_interfaces, add_usbdevice_emulation, vhci::create_default_queue };

        for (auto f: functions) {
                if (auto err = f(vhci)) {
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED int usbip::vhci::remember_device(_In_ UDECXUSBDEVICE dev)
{
        PAGED_CODE();

        auto &dev_ctx = *get_device_ctx(dev);
        auto &vhci_ctx = *get_vhci_ctx(dev_ctx.vhci); 

        auto &port = dev_ctx.port;
        NT_ASSERT(!port);

        WdfSpinLockAcquire(vhci_ctx.devices_lock);

        for (int i = 0; i < ARRAYSIZE(vhci_ctx.devices); ++i) {
                auto &handle = vhci_ctx.devices[i];
                if (!handle) {
                        WdfObjectReference(handle = dev);
                        port = i + 1;
                        NT_ASSERT(is_valid_port(port));
                        break;
                }
        }

        WdfSpinLockRelease(vhci_ctx.devices_lock);

        if (port) {
                TraceDbg("dev %04x, port %d", ptr04x(dev), port);
        }

        return port;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::vhci::forget_device(_In_ UDECXUSBDEVICE dev)
{
        auto &dev_ctx = *get_device_ctx(dev);
        auto &vhci_ctx = *get_vhci_ctx(dev_ctx.vhci); 

        auto &port = dev_ctx.port;

        int old_port = 0;
        bool removed = false;

        WdfSpinLockAcquire(vhci_ctx.devices_lock);
        if (port) {
                old_port = port;
                removed = true;

                NT_ASSERT(is_valid_port(port));
                auto &handle = vhci_ctx.devices[port - 1];

                NT_ASSERT(handle == dev);
                handle = WDF_NO_HANDLE;

                port = 0;
                static_assert(!is_valid_port(0));
        }
        WdfSpinLockRelease(vhci_ctx.devices_lock);

        if (removed) {
                TraceDbg("dev %04x, port %ld", ptr04x(dev), old_port);
                WdfObjectDereference(dev);
        }
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
wdf::ObjectRef usbip::vhci::find_device(_In_ WDFDEVICE vhci, _In_ int port)
{
        wdf::ObjectRef dev;
        if (!is_valid_port(port)) {
                return dev;
        }

        auto &ctx = *get_vhci_ctx(vhci);
        WdfSpinLockAcquire(ctx.devices_lock);

        if (auto handle = ctx.devices[port - 1]) {
                NT_ASSERT(get_device_ctx(handle)->port == port);
                dev.reset(handle); // adds reference
        }

        WdfSpinLockRelease(ctx.devices_lock);
        return dev;
}


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::vhci::destroy_all_devices(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        for (int port = 1; port <= ARRAYSIZE(vhci_ctx::devices); ++port) {
                if (auto dev = find_device(vhci, port)) {
                        device::destroy(dev.get<UDECXUSBDEVICE>());
                }
        }
}

_Function_class_(EVT_WDF_DRIVER_DEVICE_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::DriverDeviceAdd(_In_ WDFDRIVER, _Inout_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        if (auto err = initialize(DeviceInit)) {
                return err;
        }

        WDFDEVICE vhci{};
        if (auto err = create_vhci(vhci, DeviceInit)) {
                wdf::ObjectDeleteSafe(vhci);
                return err;
        }

        Trace(TRACE_LEVEL_INFORMATION, "vhci %04x", ptr04x(vhci));
        return STATUS_SUCCESS;
}
