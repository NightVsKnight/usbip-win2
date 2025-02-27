/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "network.h"
#include "trace.h"
#include "network.tmh"

#include <usbip\proto.h>
#include <usbip\proto_op.h>

#include <libdrv\dbgcommon.h>
#include <libdrv\usbd_helper.h>

#include "dev.h"
#include "urbtransfer.h"
#include "irp.h"

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS usbip::send(_Inout_ SOCKET *sock, _In_ memory pool, _In_ void *data, _In_ ULONG len)
{
        PAGED_CODE();

        Mdl mdl(data, len);
        if (auto err = pool == memory::nonpaged ? mdl.prepare_nonpaged() : mdl.prepare_paged(IoReadAccess)) {
                return err;
        }

        WSK_BUF buf{ mdl.get(), 0, len };
        return send(sock, &buf, WSK_FLAG_NODELAY);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS usbip::recv(_Inout_ SOCKET *sock, _In_ memory pool, _Out_ void *data, _In_ ULONG len)
{
        PAGED_CODE();

        Mdl mdl(data, len);
        if (auto err = pool == memory::nonpaged ? mdl.prepare_nonpaged() : mdl.prepare_paged(IoWriteAccess)) {
                return err;
        }

        WSK_BUF buf{ mdl.get(), 0, len };
        return receive(sock, &buf);
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE err_t usbip::recv_op_common(_Inout_ SOCKET *sock, _In_ UINT16 expected_code, _Out_ op_status_t &status)
{
        PAGED_CODE();
        op_common r;

        if (auto err = recv(sock, memory::stack, &r, sizeof(r))) {
                Trace(TRACE_LEVEL_ERROR, "Receive %!STATUS!", err);
                return ERR_NETWORK;
        }

	PACK_OP_COMMON(0, &r);

	if (r.version != USBIP_VERSION) {
		Trace(TRACE_LEVEL_ERROR, "Version(%#x) != expected(%#x)", r.version, USBIP_VERSION);
		return ERR_VERSION;
	}

        if (r.code != expected_code) {
                Trace(TRACE_LEVEL_ERROR, "Code(%#x) != expected(%#x)", r.code, expected_code);
                return ERR_PROTOCOL;
        }

        status = static_cast<op_status_t>(r.status);
        return ERR_NONE;
}

_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS usbip::send_cmd(_Inout_ SOCKET *sock, _Inout_ usbip_header &hdr, _Inout_opt_ URB *transfer_buffer)
{
        PAGED_CODE();

        usbip::Mdl mdl_hdr(&hdr, sizeof(hdr));

        if (auto err = mdl_hdr.prepare_paged(IoReadAccess)) {
                Trace(TRACE_LEVEL_ERROR, "prepare_paged %!STATUS!", err);
                return err;
        }

        usbip::Mdl mdl_buf;

        if (transfer_buffer && is_transfer_direction_out(hdr)) { // TransferFlags can have wrong direction
                if (auto err = make_transfer_buffer_mdl(mdl_buf, URB_BUF_LEN, false, IoReadAccess, *transfer_buffer)) {
                        Trace(TRACE_LEVEL_ERROR, "make_transfer_buffer_mdl %!STATUS!", err);
                        return err;
                }
                mdl_hdr.next(mdl_buf);
        }

        WSK_BUF buf{ mdl_hdr.get(), 0, get_total_size(hdr) };
        NT_ASSERT(verify(buf, false));

        {
                char str[DBG_USBIP_HDR_BUFSZ];
                TraceEvents(TRACE_LEVEL_VERBOSE, FLAG_USBIP, "OUT %Iu%s", buf.Length, dbg_usbip_hdr(str, sizeof(str), &hdr, true));
        }

        byteswap_header(hdr, swap_dir::host2net);

        if (auto err = send(sock, &buf, WSK_FLAG_NODELAY)) {
                Trace(TRACE_LEVEL_ERROR, "Send %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

/*
 * URB must have TransferBuffer* members.
 * TransferBuffer && TransferBufferMDL can be both not NULL for bulk/int at least.
 * 
 * TransferBufferMDL can be a chain and have size greater than mdl_size. 
 * If attach tail to this MDL (as for isoch transfer), new MDL must be used 
 * to describe a buffer with required mdl_size.
 * 
 * If use MmBuildMdlForNonPagedPool for TransferBuffer, DRIVER_VERIFIER_DETECTED_VIOLATION (c4) will happen sooner or later,
 * Arg1: 0000000000000140, Non-locked MDL constructed from either pageable or tradable memory.
 * 
 * @param mdl_size pass URB_BUF_LEN to use TransferBufferLength, real value must not be greater than TransferBufferLength
 * @param mdl_chain tail will be attached to this mdl
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS usbip::make_transfer_buffer_mdl(
        _Out_ Mdl &mdl, _In_ ULONG mdl_size, _In_ bool mdl_chain, _In_ LOCK_OPERATION Operation, _In_ const URB& urb)
{
        NT_ASSERT(!mdl);
        auto &r = AsUrbTransfer(urb);

        if (mdl_size == URB_BUF_LEN) {
                mdl_size = r.TransferBufferLength;
        } else if (mdl_size > r.TransferBufferLength) {
                return STATUS_INVALID_PARAMETER;
        }
        
        if (!mdl_size) {
                return STATUS_SUCCESS;
        }

        auto make = [&mdl, mdl_size, Operation] (auto buf, auto probe_and_lock)
        {
                mdl = Mdl(buf, mdl_size);
                auto err = probe_and_lock ? mdl.prepare_paged(Operation) : mdl.prepare_nonpaged();
                if (err) {
                        mdl.reset();
                }
                return err;
        };

        if (r.TransferBufferMDL) {
                // preferable case because it is locked-down
        } else if (auto buf = r.TransferBuffer) { // could be allocated from paged pool
                return make(buf, true); // false -> DRIVER_VERIFIER_DETECTED_VIOLATION
        } else {
                Trace(TRACE_LEVEL_ERROR, "TransferBuffer and TransferBufferMDL are NULL");
                return STATUS_INVALID_PARAMETER;
        }

        auto head = r.TransferBufferMDL;
        auto len = size(head); // can be a chain

        if (len < r.TransferBufferLength) { // must describe full buffer
                return STATUS_BUFFER_TOO_SMALL;
        } else if (len == mdl_size || (len > mdl_size && !mdl_chain)) { // WSK_BUF.Length will cut extra length
                NT_VERIFY(mdl = Mdl(head));
                return STATUS_SUCCESS;
        } else if (!head->Next) { // build partial MDL
                mdl = Mdl(head, 0, mdl_size);
                return mdl ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        } else if (auto buf = MmGetSystemAddressForMdlSafe(head, LowPagePriority | MdlMappingNoExecute)) {
                // IoBuildPartialMdl doesn't treat SourceMdl as a chain and can't be used
                return make(buf, false); // if use MmGetMdlVirtualAddress(head) -> IRQL_NOT_LESS_OR_EQUAL
        } else {
                return STATUS_INSUFFICIENT_RESOURCES;
        }
}
