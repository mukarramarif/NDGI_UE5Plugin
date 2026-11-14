// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace NDGI
{
	namespace Editor
	{
		namespace BakeManifest
		{
			struct FAtlasEntry
			{
				int32 TexIndex = 0;
				FString TexturePath;
				int32 Width = 0;
				int32 Height = 0;
				/** Streaming-level packages (cell levels) that own components in this atlas. */
				TArray<FString> Cells;
			};

			struct FManifest
			{
				FString LevelPackage;
				int32 Steps = 0;
				/** UTC ticks when the manifest was written (step 0). */
				int64 SavedAtTicks = 0;
				TArray<FAtlasEntry> Atlases;

				bool IsValid() const
				{
					return Steps > 0 && Atlases.Num() > 0;
				}
			};

			FString GetPath();

			bool Save(const FManifest& Manifest);

			bool Load(FManifest& OutManifest);

			void Clear();
		}
	}
}
