// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIBakeManifest.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace NDGI
{
	namespace Editor
	{
		namespace BakeManifest
		{
			namespace
			{
				const TCHAR* Header = TEXT("NDGI_BAKE_MANIFEST\t1");
			}

			FString GetPath()
			{
				return FPaths::ProjectSavedDir() / TEXT("NDGI/Bake/BakeManifest.txt");
			}

			bool Save(const FManifest& Manifest)
			{
				TArray<FString> Lines;
				Lines.Add(Header);
				Lines.Add(FString::Printf(TEXT("Level\t%s"), *Manifest.LevelPackage));
				Lines.Add(FString::Printf(TEXT("Steps\t%d"), Manifest.Steps));
				Lines.Add(FString::Printf(TEXT("Timestamp\t%lld"), FDateTime::UtcNow().GetTicks()));
				for (const FAtlasEntry& Atlas : Manifest.Atlases)
				{
					Lines.Add(FString::Printf(TEXT("Atlas\t%d\t%s\t%d\t%d\t%s"),
						Atlas.TexIndex, *Atlas.TexturePath, Atlas.Width, Atlas.Height,
						*FString::Join(Atlas.Cells, TEXT(";"))));
				}
				return FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *GetPath());
			}

			bool Load(FManifest& OutManifest)
			{
				OutManifest = FManifest();
				FString Contents;
				if (!FFileHelper::LoadFileToString(Contents, *GetPath()))
				{
					return false;
				}

				TArray<FString> Lines;
				Contents.ParseIntoArrayLines(Lines, false);
				if (Lines.Num() == 0)
				{
					return false;
				}

				TArray<FString> HeaderFields;
				Lines[0].ParseIntoArray(HeaderFields, TEXT("\t"), false);
				if (HeaderFields.Num() != 2 || HeaderFields[0] != TEXT("NDGI_BAKE_MANIFEST") || HeaderFields[1] != TEXT("1"))
				{
					return false;
				}

				for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
				{
					TArray<FString> Fields;
					Lines[LineIndex].ParseIntoArray(Fields, TEXT("\t"), false);
					if (Fields.Num() >= 2 && Fields[0] == TEXT("Level"))
					{
						OutManifest.LevelPackage = Fields[1];
					}
					else if (Fields.Num() >= 2 && Fields[0] == TEXT("Steps"))
					{
						OutManifest.Steps = FCString::Atoi(*Fields[1]);
					}
					else if (Fields.Num() >= 2 && Fields[0] == TEXT("Timestamp"))
					{
						OutManifest.SavedAtTicks = FCString::Atoi64(*Fields[1]);
					}
					else if (Fields.Num() >= 5 && Fields[0] == TEXT("Atlas"))
					{
						FAtlasEntry& Atlas = OutManifest.Atlases.AddDefaulted_GetRef();
						Atlas.TexIndex = FCString::Atoi(*Fields[1]);
						Atlas.TexturePath = Fields[2];
						Atlas.Width = FCString::Atoi(*Fields[3]);
						Atlas.Height = FCString::Atoi(*Fields[4]);
						if (Fields.Num() >= 6 && !Fields[5].IsEmpty())
						{
							Fields[5].ParseIntoArray(Atlas.Cells, TEXT(";"), false);
						}
					}
				}
				return OutManifest.IsValid();
			}

			void Clear()
			{
				IFileManager::Get().Delete(*GetPath(), false, true, true);
			}
		}
	}
}
