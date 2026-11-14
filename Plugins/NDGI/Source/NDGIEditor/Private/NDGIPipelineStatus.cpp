// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIPipelineStatus.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/CriticalSection.h"
#include "Logging/LogMacros.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "Misc/StringBuilder.h"
#include "NDGI.h"

namespace NDGI
{
	namespace Editor
	{
		namespace Pipeline
		{
			namespace
			{
				constexpr int32 MaxLogLines = 600;

				FCriticalSection GStateLock;
				FState GState;
				TArray<FString> GLogLines;
				uint64 GLogVersion = 0;
				bool GCancelRequested = false;

				class FNDGIPipelineLogCapture final : public FOutputDevice
				{
				public:
					virtual bool CanBeUsedOnAnyThread() const override
					{
						return true;
					}

					virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
					{
						if (Category != FName(TEXT("LogNDGI")) && Category != FName(TEXT("LogNDGIRuntime")))
						{
							return;
						}

						FString Line(V);
						Line.ReplaceInline(TEXT("\r"), TEXT(""));
						// Strip the timestamp/verbosity prefix but keep the message readable.
						const FString Prefix = Category == FName(TEXT("LogNDGI"))
							? TEXT("LogNDGI: ")
							: TEXT("LogNDGIRuntime: ");
						const int32 CategoryPos = Line.Find(Prefix);
						if (CategoryPos != INDEX_NONE)
						{
							Line = Line.Mid(CategoryPos + Prefix.Len());
						}
						AddLogLine(Line);
					}
				};

				TUniquePtr<FNDGIPipelineLogCapture> GLogCapture;

				enum class EAutoStep : uint8
				{
					Bake,
					Merge,
					Train,
					Validate
				};

				struct FAutoRunState
				{
					FAutoRunOptions Options;
					EAutoStep Step = EAutoStep::Bake;
					bool bWaitingForStart = false;
					double IssuedSeconds = 0.0;
					FTSTicker::FDelegateHandle TickerHandle;
				};

				TSharedPtr<FAutoRunState> GAutoRun;

				FState GetStateLocked()
				{
					return GState;
				}

				bool ExecuteAutoStep(const TSharedRef<FAutoRunState>& Run)
				{
					UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
					if (!World)
					{
						UE_LOG(LogNDGI, Error, TEXT("Full pipeline: no editor world."));
						return false;
					}

					FString Command;
					switch (Run->Step)
					{
					case EAutoStep::Bake:
						Command = FString::Printf(TEXT("ndgi.BakeTOD %d"), Run->Options.BakeSteps);
						break;
					case EAutoStep::Merge:
						Command = FString::Printf(TEXT("ndgi.MergeAllBakedSets %d"), Run->Options.BakeSteps);
						break;
					case EAutoStep::Train:
						Command = FString::Printf(TEXT("ndgi.TrainAllAtlases %d %d %d"),
							Run->Options.TileSize, Run->Options.TrainSteps, Run->Options.Overlap);
						break;
					case EAutoStep::Validate:
						Command = TEXT("ndgi.ValidateLevelAsset 1.0");
						break;
					}

					AddLogLine(FString::Printf(TEXT("> %s"), *Command));
					if (!GEditor->Exec(World, *Command))
					{
						UE_LOG(LogNDGI, Error, TEXT("Full pipeline: command failed to execute: %s"), *Command);
						return false;
					}

					const FState After = GetState();
					Run->bWaitingForStart = !After.bRunning
						&& After.Stage != EStage::Completed
						&& After.Stage != EStage::Failed;
					Run->IssuedSeconds = FPlatformTime::Seconds();
					return true;
				}

				void FinishAutoRun(bool bSuccess, const FString& Message)
				{
					if (!GAutoRun.IsValid())
					{
						return;
					}
					UE_LOG(LogNDGI, Display, TEXT("%s"), *Message);
					if (GAutoRun->Options.bQuitWhenDone && GEngine)
					{
						GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
					}
					GAutoRun.Reset();
				}

				bool TickAutoRun(float)
				{
					if (!GAutoRun.IsValid())
					{
						return false;
					}

					const FState State = GetState();
					if (GAutoRun->bWaitingForStart)
					{
						if (State.bRunning)
						{
							GAutoRun->bWaitingForStart = false;
						}
						else if (State.Stage == EStage::Failed)
						{
							FinishAutoRun(false, TEXT("Full pipeline stopped because a stage failed."));
							return false;
						}
						else if (FPlatformTime::Seconds() - GAutoRun->IssuedSeconds > 5.0)
						{
							Fail(TEXT("The pipeline stage did not start."));
							FinishAutoRun(false, TEXT("Full pipeline stopped because a stage did not start."));
							return false;
						}
						return true;
					}

					if (State.bRunning)
					{
						return true;
					}
					if (State.Stage == EStage::Failed)
					{
						FinishAutoRun(false, TEXT("Full pipeline stopped because a stage failed."));
						return false;
					}
					if (State.Stage != EStage::Completed)
					{
						return true;
					}

					switch (GAutoRun->Step)
					{
					case EAutoStep::Bake:
						GAutoRun->Step = EAutoStep::Merge;
						break;
					case EAutoStep::Merge:
						GAutoRun->Step = EAutoStep::Train;
						break;
					case EAutoStep::Train:
						if (GAutoRun->Options.bValidateAfterTrain)
						{
							GAutoRun->Step = EAutoStep::Validate;
						}
						else
						{
							FinishAutoRun(true, TEXT("Full pipeline finished (validation skipped)."));
							return false;
						}
						break;
					case EAutoStep::Validate:
						FinishAutoRun(true, TEXT("Full pipeline finished."));
						return false;
					}

					if (!ExecuteAutoStep(GAutoRun.ToSharedRef()))
					{
						Fail(TEXT("Could not start the next pipeline stage."));
						FinishAutoRun(false, TEXT("Full pipeline stopped because the next stage could not start."));
						return false;
					}
					return true;
				}
			}

			void Begin(EStage Stage, const FString& Operation, int32 Total, const FString& Detail)
			{
				FScopeLock Lock(&GStateLock);
				GState.Stage = Stage;
				GState.Operation = Operation;
				GState.Detail = Detail;
				GState.Current = 0;
				GState.Total = FMath::Max(0, Total);
				GState.bRunning = true;
				GState.bLastRunPassed = false;
				GState.StartSeconds = FPlatformTime::Seconds();
				GState.EndSeconds = 0.0;
				GCancelRequested = false;
			}

			void RequestCancel()
			{
				{
					FScopeLock Lock(&GStateLock);
					GCancelRequested = true;
				}
				AddLogLine(TEXT("Cancellation requested; the active job will stop after the current slice/tile."));
			}

			bool IsCancelRequested()
			{
				FScopeLock Lock(&GStateLock);
				return GCancelRequested;
			}

			void ClearCancel()
			{
				FScopeLock Lock(&GStateLock);
				GCancelRequested = false;
			}

			void SetProgress(int32 Current, int32 Total, const FString& Detail)
			{
				FScopeLock Lock(&GStateLock);
				GState.Current = FMath::Max(0, Current);
				GState.Total = FMath::Max(0, Total);
				if (!Detail.IsEmpty())
				{
					GState.Detail = Detail;
				}
			}

			void SetDetail(const FString& Detail)
			{
				FScopeLock Lock(&GStateLock);
				GState.Detail = Detail;
			}

			void Complete(bool bSuccess, const FString& Detail)
			{
				FScopeLock Lock(&GStateLock);
				GState.bRunning = false;
				GState.bLastRunPassed = bSuccess;
				GState.Stage = bSuccess ? EStage::Completed : EStage::Failed;
				GState.EndSeconds = FPlatformTime::Seconds();
				if (!Detail.IsEmpty())
				{
					GState.Detail = Detail;
				}
				else if (GState.Detail.IsEmpty())
				{
					GState.Detail = bSuccess ? TEXT("Completed.") : TEXT("Failed.");
				}
			}

			void Fail(const FString& Detail)
			{
				Complete(false, Detail);
			}

			void Reset()
			{
				FScopeLock Lock(&GStateLock);
				GState = FState();
				GCancelRequested = false;
			}

			FState GetState()
			{
				FScopeLock Lock(&GStateLock);
				return GetStateLocked();
			}

			double GetElapsedSeconds(const FState& State)
			{
				if (State.StartSeconds <= 0.0)
				{
					return 0.0;
				}
				const double End = State.EndSeconds > 0.0 ? State.EndSeconds : FPlatformTime::Seconds();
				return FMath::Max(0.0, End - State.StartSeconds);
			}

			FString StageToString(EStage Stage)
			{
				switch (Stage)
				{
				case EStage::Baking: return TEXT("Baking");
				case EStage::Merging: return TEXT("Merging");
				case EStage::Training: return TEXT("Training");
				case EStage::Validating: return TEXT("Validating");
				case EStage::Completed: return TEXT("Completed");
				case EStage::Failed: return TEXT("Failed");
				default: return TEXT("Idle");
				}
			}

			TArray<FString> GetLogLines()
			{
				FScopeLock Lock(&GStateLock);
				return GLogLines;
			}

			uint64 GetLogVersion()
			{
				FScopeLock Lock(&GStateLock);
				return GLogVersion;
			}

			void AddLogLine(const FString& Line)
			{
				FString Clean = Line;
				Clean.ReplaceInline(TEXT("\r"), TEXT(""));
				Clean.TrimStartAndEndInline();
				if (Clean.IsEmpty())
				{
					return;
				}

				FScopeLock Lock(&GStateLock);
				GLogLines.Add(MoveTemp(Clean));
				++GLogVersion;
				if (GLogLines.Num() > MaxLogLines)
				{
					GLogLines.RemoveAt(0, GLogLines.Num() - MaxLogLines, EAllowShrinking::No);
				}
			}

			void ClearLog()
			{
				FScopeLock Lock(&GStateLock);
				GLogLines.Reset();
				++GLogVersion;
			}

			bool StartAutoRun(const FAutoRunOptions& Options)
			{
				if (GAutoRun.IsValid() || GetState().bRunning)
				{
					AddLogLine(TEXT("Full pipeline: another operation is already running."));
					return false;
				}
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!World)
				{
					AddLogLine(TEXT("Full pipeline: open an editor level first."));
					return false;
				}

				GAutoRun = MakeShared<FAutoRunState>();
				GAutoRun->Options = Options;
				GAutoRun->Step = EAutoStep::Bake;
				UE_LOG(LogNDGI, Display, TEXT("Starting full NDGI pipeline."));
				if (!ExecuteAutoStep(GAutoRun.ToSharedRef()))
				{
					Fail(TEXT("Could not start the bake."));
					FinishAutoRun(false, TEXT("Full pipeline stopped because the bake could not start."));
					return false;
				}
				GAutoRun->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateStatic(&TickAutoRun), 0.25f);
				return true;
			}

			bool IsAutoRunning()
			{
				return GAutoRun.IsValid();
			}

			void StopAutoRun()
			{
				if (GAutoRun.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(GAutoRun->TickerHandle);
					GAutoRun.Reset();
					UE_LOG(LogNDGI, Display, TEXT("Full pipeline auto-run stopped after the current stage."));
				}
			}

			void RegisterLogCapture()
			{
				if (!GLogCapture.IsValid() && GLog)
				{
					GLogCapture = MakeUnique<FNDGIPipelineLogCapture>();
					GLog->AddOutputDevice(GLogCapture.Get());
				}
			}

			void UnregisterLogCapture()
			{
				if (GLogCapture.IsValid() && GLog)
				{
					GLog->RemoveOutputDevice(GLogCapture.Get());
				}
				GLogCapture.Reset();
			}
		}
	}
}

namespace
{
	void RunAutoPipelineCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		NDGI::Editor::Pipeline::FAutoRunOptions Options;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Options.bQuitWhenDone = true;
			}
			else if (Arg.Equals(TEXT("novalidate"), ESearchCase::IgnoreCase))
			{
				Options.bValidateAfterTrain = false;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				switch (NumericArgIndex)
				{
				case 0: Options.BakeSteps = FMath::Clamp(Value, 2, 64); break;
				case 1:
					Options.TileSize = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Value) / 16.0f) * 16, 16, 512);
					break;
				case 2: Options.TrainSteps = FMath::Clamp(Value, 1, 200000); break;
				case 3:
					Options.Overlap = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Value) / 8.0f) * 8, 0, 64);
					break;
				default: break;
				}
				++NumericArgIndex;
			}
		}

		UE_LOG(LogNDGI, Display,
			TEXT("ndgi.RunPipeline: bake %d, tile %d, %d steps/tile, overlap %d, validation %s."),
			Options.BakeSteps, Options.TileSize, Options.TrainSteps, Options.Overlap,
			Options.bValidateAfterTrain ? TEXT("on") : TEXT("off"));

		if (!NDGI::Editor::Pipeline::StartAutoRun(Options))
		{
			if (Options.bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIRunPipelineCommand(
	TEXT("ndgi.RunPipeline"),
	TEXT("Runs bake -> merge -> train -> validate for the current level. Usage: ndgi.RunPipeline <bakeSteps> <tileSize> <trainSteps> <overlap> [novalidate] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunAutoPipelineCommand));

static FAutoConsoleCommand GNDGICancelPipelineCommand(
	TEXT("ndgi.CancelPipeline"),
	TEXT("Requests cancellation of the active bake/training job; it stops after the current slice/tile."),
	FConsoleCommandDelegate::CreateStatic(&NDGI::Editor::Pipeline::RequestCancel));

namespace
{
	void RunTestCancelAfter(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		double Delay = 2.0;
		for (const FString& Arg : Args)
		{
			if (Arg.IsNumeric())
			{
				Delay = FMath::Clamp(FCString::Atod(*Arg), 0.05, 600.0);
				break;
			}
		}

		struct FDelayedCancel
		{
			double Deadline = 0.0;
		};
		TSharedRef<FDelayedCancel> State = MakeShared<FDelayedCancel>();
		State->Deadline = FPlatformTime::Seconds() + Delay;
		UE_LOG(LogNDGI, Display, TEXT("ndgi.TestCancelAfter: requesting cancellation in %.2f s."), Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
		{
			if (FPlatformTime::Seconds() >= State->Deadline)
			{
				NDGI::Editor::Pipeline::RequestCancel();
				return false;
			}
			return true;
		}), 0.0f);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestCancelAfterCommand(
	TEXT("ndgi.TestCancelAfter"),
	TEXT("Requests NDGI pipeline cancellation after N seconds. Usage: ndgi.TestCancelAfter <seconds>."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestCancelAfter));
