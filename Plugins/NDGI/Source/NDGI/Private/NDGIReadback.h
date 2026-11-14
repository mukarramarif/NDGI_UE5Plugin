// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FRHIGPUBufferReadback;

namespace NDGI
{
	namespace Internal
	{
		/**
		 * Executes an RDG graph produced by AddPasses (which must enqueue the final
		 * copy into Readback via AddEnqueueCopyPass), then reads the staging buffer
		 * back and delivers the bytes on the game thread.
		 */
		void DispatchAndReadbackBytes(
			uint32 NumBytes,
			TFunction<void(FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)>&& AddPasses,
			TFunction<void(bool bSuccess, TArray<uint8>&& Data)>&& OnComplete);
	}
}
