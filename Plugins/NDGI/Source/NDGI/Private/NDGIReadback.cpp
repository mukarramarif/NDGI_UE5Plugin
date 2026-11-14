// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIReadback.h"

#include "Async/TaskGraphInterfaces.h"
#include "RenderGraphBuilder.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

namespace NDGI
{
	namespace Internal
	{
		namespace
		{
			struct FReadbackPoll
			{
				FRHIGPUBufferReadback* Readback = nullptr;
				uint32 NumBytes = 0;
				TFunction<void(bool bSuccess, TArray<uint8>&& Data)> OnComplete;
			};
		}

		void DispatchAndReadbackBytes(
			uint32 NumBytes,
			TFunction<void(FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)>&& AddPasses,
			TFunction<void(bool bSuccess, TArray<uint8>&& Data)>&& OnComplete)
		{
			if (NumBytes == 0 || !GDynamicRHI || !GMaxRHIFeatureLevel)
			{
				if (OnComplete)
				{
					OnComplete(false, TArray<uint8>());
				}
				return;
			}

			FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("NDGI.Readback"));

			ENQUEUE_RENDER_COMMAND(NDGIDispatchAndReadback)(
				[Readback, NumBytes, AddPasses = MoveTemp(AddPasses)](FRHICommandListImmediate& RHICmdList)
				{
					FRDGBuilder GraphBuilder(RHICmdList);
					AddPasses(GraphBuilder, Readback, NumBytes);
					GraphBuilder.Execute();
				});

			TSharedRef<FReadbackPoll, ESPMode::ThreadSafe> Poll = MakeShared<FReadbackPoll, ESPMode::ThreadSafe>();
			Poll->Readback = Readback;
			Poll->NumBytes = NumBytes;
			Poll->OnComplete = MoveTemp(OnComplete);

			// Poll the fence on the game thread (never block it on the GPU). Once the data
			// is ready, hop to the render thread to lock/copy the staging buffer — Lock()
			// is only valid there — and then deliver the result back on the game thread.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Poll](float DeltaTime) -> bool
			{
				if (!Poll->Readback || !Poll->Readback->IsReady())
				{
					return true;
				}

				FRHIGPUBufferReadback* ReadbackToConsume = Poll->Readback;
				Poll->Readback = nullptr;
				const uint32 NumBytesToRead = Poll->NumBytes;

				ENQUEUE_RENDER_COMMAND(NDGILockReadback)(
					[Poll, ReadbackToConsume, NumBytesToRead](FRHICommandListImmediate& RHICmdList)
					{
						TArray<uint8> Bytes;
						void* Data = ReadbackToConsume->Lock(NumBytesToRead);
						if (Data)
						{
							Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(NumBytesToRead));
						}
						ReadbackToConsume->Unlock();
						delete ReadbackToConsume;

						AsyncTask(ENamedThreads::GameThread, [Poll, Bytes = MoveTemp(Bytes)]() mutable
						{
							if (Poll->OnComplete)
							{
								Poll->OnComplete(Bytes.Num() == static_cast<int32>(Poll->NumBytes), MoveTemp(Bytes));
							}
						});
					});

				return false;
			}), 0.0f);
		}
	}
}
