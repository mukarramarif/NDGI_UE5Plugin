// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NDGI
{
	namespace Editor
	{
		namespace Pipeline
		{
			enum class EStage : uint8
			{
				Idle,
				Baking,
				Merging,
				Training,
				Validating,
				Completed,
				Failed
			};

			struct FState
			{
				EStage Stage = EStage::Idle;
				FString Operation;
				FString Detail;
				int32 Current = 0;
				int32 Total = 0;
				bool bRunning = false;
				bool bLastRunPassed = false;
				double StartSeconds = 0.0;
				double EndSeconds = 0.0;
			};

			/** Starts a pipeline operation. Total <= 0 shows an indeterminate progress bar. */
			void Begin(EStage Stage, const FString& Operation, int32 Total = 0, const FString& Detail = FString());

			/** Requests cancellation; long-running jobs poll this between slices/tiles. */
			void RequestCancel();
			bool IsCancelRequested();
			void ClearCancel();

			void SetProgress(int32 Current, int32 Total, const FString& Detail = FString());

			void SetDetail(const FString& Detail);

			void Complete(bool bSuccess, const FString& Detail = FString());

			void Fail(const FString& Detail);

			void Reset();

			FState GetState();

			double GetElapsedSeconds(const FState& State);

			FString StageToString(EStage Stage);

			/** Captured LogNDGI/LogNDGIRuntime lines, oldest first. */
			TArray<FString> GetLogLines();

			/** Increments whenever a line is added or the log is cleared. */
			uint64 GetLogVersion();

			void AddLogLine(const FString& Line);

			void ClearLog();

			/** Module lifetime: capture NDGI log output into the ring buffer. */
			void RegisterLogCapture();
			void UnregisterLogCapture();

			struct FAutoRunOptions
			{
				int32 BakeSteps = 8;
				int32 TileSize = 128;
				int32 TrainSteps = 3000;
				int32 Overlap = 8;
				bool bValidateAfterTrain = true;
				bool bQuitWhenDone = false;
			};

			/** Starts bake → merge → train → (optional) validate on the current editor world. */
			bool StartAutoRun(const FAutoRunOptions& Options);

			bool IsAutoRunning();

			/** Stops the auto-run ticker (does not cancel an already running bake/train). */
			void StopAutoRun();
		}
	}
}
