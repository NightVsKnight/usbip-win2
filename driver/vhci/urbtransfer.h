/*
 * Copyright (C) 2021, 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <stddef.h>
#include <wdm.h>
#include <usb.h>

struct UrbTransfer
{
	using type = _URB_CONTROL_TRANSFER;

	UCHAR Reserved[offsetof(type, PipeHandle)];
	decltype(type::PipeHandle) PipeHandle;
	decltype(type::TransferFlags) TransferFlags;
	decltype(type::TransferBufferLength) TransferBufferLength;
	decltype(type::TransferBuffer) TransferBuffer;
	decltype(type::TransferBufferMDL) TransferBufferMDL;
};


bool has_transfer_buffer(_In_ const URB &urb);

inline auto& AsUrbTransfer(_In_ URB &urb) 
{ 
	NT_ASSERT(has_transfer_buffer(urb));
	return reinterpret_cast<UrbTransfer&>(urb); 
}

inline auto& AsUrbTransfer(_In_ const URB &urb) 
{ 
	NT_ASSERT(has_transfer_buffer(urb));
	return reinterpret_cast<const UrbTransfer&>(urb); 
}
