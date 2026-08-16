// BlueprintExportAnimEdit.cpp
// Mutation ops for animation assets, built for the mounted rear-aim pose
// pipeline (bulk FBX pose import → mark additive → rewire aim blendspaces):
//   anim.import           — automated FBX → UAnimSequence (saves on import)
//   anim.set_additive     — additive type / base pose settings on a sequence
//   anim.blendspace_*     — create / set_axis / add_sample / remove_sample
//   anim.save             — generic dirty-package save (font.save semantics)
//
// All mutations except import stage in memory and persist via anim.save —
// anim assets are not Blueprints, so `edit save` does not apply. Import
// saves immediately (UAssetImportTask::bSave): staging ~144 imports in
// memory and losing them on a server restart is the wrong default.
//
// Blend space grid: UBlendSpaceBase::AddSample/DeleteSample only mutate
// SampleData — triangulation (GridSamples) is editor-side, in Persona's
// module-private FDelaunayTriangleGenerator/FBlendSpaceGrid (2D) and
// FLineElementGenerator (1D). Without it the asset never blends at runtime.
// Those helpers are vendored (AnimationBlendSpace*Helpers.*, namespace
// DigBSGrid) and every blendspace mutation here ends with a full resample,
// mirroring the blend space editor's ResampleData().

#include "BlueprintExportCommandlet.h"

#include "AnimationBlendSpaceHelpers.h"
#include "AnimationBlendSpace1DHelpers.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Unity-build note: unique AnimEdit_ prefix on locals.

namespace
{
	TSharedPtr<FJsonObject> AnimEdit_MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return Out;
	}

	FString AnimEdit_PersistHint(const FString& Path)
	{
		return FString::Printf(TEXT("digbp anim save --path=%s"), *Path);
	}

	void AnimEdit_MarkStaged(const TSharedPtr<FJsonObject>& Result, const FString& Path)
	{
		Result->SetBoolField(TEXT("dirty"), true);
		Result->SetBoolField(TEXT("staged"), true);
		Result->SetStringField(TEXT("persist_with"), AnimEdit_PersistHint(Path));
	}

	template <typename T>
	T* AnimEdit_LoadAsset(const FString& Path, const TCHAR* Expected, TSharedPtr<FJsonObject>& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = AnimEdit_MakeError(TEXT("Empty path"));
			return nullptr;
		}
		UObject* Asset = LoadObject<UObject>(nullptr, *Path);
		while (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset))
		{
			if (!Redirector->DestinationObject) { break; }
			Asset = Redirector->DestinationObject;
		}
		if (!Asset)
		{
			OutError = AnimEdit_MakeError(FString::Printf(TEXT("Failed to load asset at path: %s"), *Path));
			return nullptr;
		}
		T* Typed = Cast<T>(Asset);
		if (!Typed)
		{
			OutError = AnimEdit_MakeError(FString::Printf(TEXT("Asset is not a %s (class %s): %s"), Expected, *Asset->GetClass()->GetName(), *Path));
			return nullptr;
		}
		return Typed;
	}

	// "/Game/Path/AssetName" → package dir + asset name.
	bool AnimEdit_SplitDest(const FString& Dest, FString& OutDir, FString& OutName, TSharedPtr<FJsonObject>& OutError)
	{
		if (!Dest.StartsWith(TEXT("/")) || !Dest.Split(TEXT("/"), &OutDir, &OutName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || OutName.IsEmpty() || OutDir.IsEmpty())
		{
			OutError = AnimEdit_MakeError(FString::Printf(TEXT("dest must be a full asset path like /Game/Path/AssetName (got: %s)"), *Dest));
			return false;
		}
		return true;
	}

	// --- Enum parsing (case-insensitive; explicit lists, mirroring the
	// strings anim.export emits so the two round-trip) ---

	bool AnimEdit_ParseAdditiveType(const FString& In, EAdditiveAnimationType& Out)
	{
		if (In.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { Out = AAT_None; return true; }
		if (In.Equals(TEXT("LocalSpaceBase"), ESearchCase::IgnoreCase)) { Out = AAT_LocalSpaceBase; return true; }
		if (In.Equals(TEXT("RotationOffsetMeshSpace"), ESearchCase::IgnoreCase)) { Out = AAT_RotationOffsetMeshSpace; return true; }
		return false;
	}

	bool AnimEdit_ParseBasePoseType(const FString& In, EAdditiveBasePoseType& Out)
	{
		if (In.Equals(TEXT("None"), ESearchCase::IgnoreCase)) { Out = ABPT_None; return true; }
		if (In.Equals(TEXT("RefPose"), ESearchCase::IgnoreCase)) { Out = ABPT_RefPose; return true; }
		if (In.Equals(TEXT("AnimScaled"), ESearchCase::IgnoreCase)) { Out = ABPT_AnimScaled; return true; }
		if (In.Equals(TEXT("AnimFrame"), ESearchCase::IgnoreCase)) { Out = ABPT_AnimFrame; return true; }
		return false;
	}

	UClass* AnimEdit_BlendSpaceClass(const FString& In)
	{
		if (In.Equals(TEXT("BlendSpace"), ESearchCase::IgnoreCase)) { return UBlendSpace::StaticClass(); }
		if (In.Equals(TEXT("BlendSpace1D"), ESearchCase::IgnoreCase)) { return UBlendSpace1D::StaticClass(); }
		if (In.Equals(TEXT("AimOffsetBlendSpace"), ESearchCase::IgnoreCase)) { return UAimOffsetBlendSpace::StaticClass(); }
		if (In.Equals(TEXT("AimOffsetBlendSpace1D"), ESearchCase::IgnoreCase)) { return UAimOffsetBlendSpace1D::StaticClass(); }
		return nullptr;
	}

	// --- Grid resample (headless port of the blend space editor's
	// ResampleData; see SAnimationBlendSpace{,1D}.cpp) ---

	void AnimEdit_ResampleGrid(UBlendSpaceBase* BlendSpace)
	{
		// Refresh per-sample bIsValid before triangulating — invalid samples
		// (skeleton/additive mismatch, dup points) are excluded like in-editor.
		BlendSpace->ValidateSampleData();
		BlendSpace->EmptyGridElements();

		const TArray<FBlendSample>& BlendSamples = BlendSpace->GetBlendSamples();
		if (BlendSamples.Num() == 0)
		{
			return;
		}

		if (BlendSpace->IsA<UBlendSpace1D>())
		{
			DigBSGrid::FLineElementGenerator ElementGenerator;
			ElementGenerator.Init(BlendSpace->GetBlendParameter(0));
			for (const FBlendSample& Sample : BlendSamples)
			{
				if (Sample.bIsValid)
				{
					ElementGenerator.SamplePointList.Add(Sample.SampleValue.X);
				}
			}
			if (ElementGenerator.SamplePointList.Num() == 0)
			{
				return;
			}
			ElementGenerator.CalculateEditorElements();

			TArray<int32> PointListToSampleIndices;
			PointListToSampleIndices.Init(INDEX_NONE, ElementGenerator.SamplePointList.Num());
			for (int32 PointIndex = 0; PointIndex < ElementGenerator.SamplePointList.Num(); ++PointIndex)
			{
				const float Point = ElementGenerator.SamplePointList[PointIndex];
				for (int32 SampleIndex = 0; SampleIndex < BlendSamples.Num(); ++SampleIndex)
				{
					if (BlendSamples[SampleIndex].SampleValue.X == Point)
					{
						PointListToSampleIndices[PointIndex] = SampleIndex;
						break;
					}
				}
			}
			BlendSpace->FillupGridElements(PointListToSampleIndices, ElementGenerator.EditorElements);
		}
		else
		{
			DigBSGrid::FDelaunayTriangleGenerator Generator;
			DigBSGrid::FBlendSpaceGrid BlendSpaceGrid;
			const FBlendParameter& BlendParamX = BlendSpace->GetBlendParameter(0);
			const FBlendParameter& BlendParamY = BlendSpace->GetBlendParameter(1);
			BlendSpaceGrid.SetGridInfo(BlendParamX, BlendParamY);
			Generator.SetGridBox(BlendParamX, BlendParamY);

			for (int32 SampleIndex = 0; SampleIndex < BlendSamples.Num(); ++SampleIndex)
			{
				if (BlendSamples[SampleIndex].bIsValid)
				{
					Generator.AddSamplePoint(BlendSamples[SampleIndex].SampleValue, SampleIndex);
				}
			}
			Generator.Triangulate();

			const TArray<DigBSGrid::FPoint>& Points = Generator.GetSamplePointList();
			const TArray<DigBSGrid::FTriangle*>& Triangles = Generator.GetTriangleList();
			BlendSpaceGrid.GenerateGridElements(Points, Triangles);
			if (Triangles.Num() > 0)
			{
				BlendSpace->FillupGridElements(Generator.GetIndiceMapping(), BlendSpaceGrid.GetElements());
			}
		}
	}

	TSharedPtr<FJsonObject> AnimEdit_MakeBlendSpaceSummary(UBlendSpaceBase* BlendSpace)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("path"), BlendSpace->GetPathName());
		Out->SetStringField(TEXT("asset_class"), BlendSpace->GetClass()->GetName());
		Out->SetNumberField(TEXT("sample_count"), BlendSpace->GetNumberOfBlendSamples());
		return Out;
	}
}

//------------------------------------------------------------------------------
// anim.import — automated FBX → UAnimSequence on a skeleton (saves on import)
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimImportFbxToJson(const FString& FbxPath, const FString& SkeletonPath, const FString& DestPath)
{
	if (FbxPath.IsEmpty() || !FPaths::FileExists(FbxPath))
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("FBX file not found: %s"), *FbxPath));
	}

	TSharedPtr<FJsonObject> Err;
	USkeleton* Skeleton = AnimEdit_LoadAsset<USkeleton>(SkeletonPath, TEXT("USkeleton"), Err);
	if (!Skeleton) { return Err; }

	FString PackageDir, AssetName;
	if (!AnimEdit_SplitDest(DestPath, PackageDir, AssetName, Err)) { return Err; }

	UFbxFactory* Factory = NewObject<UFbxFactory>();
	Factory->SetDetectImportTypeOnImport(false);

	// Passed via UAssetImportTask::Options — FbxFactory adopts it wholesale
	// as the ImportUI for automated imports (FbxFactory.cpp override path).
	UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
	ImportUI->MeshTypeToImport = FBXIT_Animation;
	ImportUI->OriginalImportType = FBXIT_Animation;
	ImportUI->bAutomatedImportShouldDetectType = false;
	ImportUI->Skeleton = Skeleton;
	ImportUI->bImportAnimations = true;
	ImportUI->bImportAsSkeletal = false;
	ImportUI->bImportMesh = false;
	ImportUI->bImportRigidMesh = false;
	ImportUI->bImportMaterials = false;
	ImportUI->bImportTextures = false;
	ImportUI->bCreatePhysicsAsset = false;
	ImportUI->AnimSequenceImportData->AnimationLength = FBXALIT_ExportedTime;
	ImportUI->AnimSequenceImportData->bImportBoneTracks = true;

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = FbxPath;
	Task->DestinationPath = PackageDir;
	Task->DestinationName = AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = true;
	Task->Factory = Factory;
	Task->Options = ImportUI;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(Task);
	AssetToolsModule.Get().ImportAssetTasks(Tasks);

	UAnimSequence* Imported = nullptr;
	for (UObject* Obj : Task->Result)
	{
		Imported = Cast<UAnimSequence>(Obj);
		if (Imported) { break; }
	}
	if (!Imported)
	{
		return AnimEdit_MakeError(FString::Printf(
			TEXT("FBX import produced no AnimSequence (file: %s). Check the server log for FbxFactory errors — common causes: no animation take in the file, or bone names not matching skeleton %s"),
			*FbxPath, *SkeletonPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Imported->GetPathName());
	Result->SetStringField(TEXT("name"), Imported->GetName());
	Result->SetStringField(TEXT("asset_class"), Imported->GetClass()->GetName());
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Result->SetNumberField(TEXT("num_frames"), Imported->GetRawNumberOfFrames());
	Result->SetNumberField(TEXT("length"), Imported->SequenceLength);
	Result->SetBoolField(TEXT("saved"), true);
	return Result;
}

//------------------------------------------------------------------------------
// anim.set_additive — additive settings on a UAnimSequence
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimSetAdditiveToJson(const FString& Path, const FString& TypeStr, const FString& BasePosePath, const FString& BasePoseTypeStr, int32 RefFrame)
{
	TSharedPtr<FJsonObject> Err;
	UAnimSequence* Sequence = AnimEdit_LoadAsset<UAnimSequence>(Path, TEXT("UAnimSequence"), Err);
	if (!Sequence) { return Err; }

	EAdditiveAnimationType AdditiveType;
	if (!AnimEdit_ParseAdditiveType(TypeStr, AdditiveType))
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Invalid additive type '%s' (None | LocalSpaceBase | RotationOffsetMeshSpace)"), *TypeStr));
	}

	EAdditiveBasePoseType BasePoseType = ABPT_None;
	const bool bHasBasePoseType = !BasePoseTypeStr.IsEmpty();
	if (bHasBasePoseType && !AnimEdit_ParseBasePoseType(BasePoseTypeStr, BasePoseType))
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Invalid base pose type '%s' (None | RefPose | AnimScaled | AnimFrame)"), *BasePoseTypeStr));
	}

	UAnimSequence* BasePose = nullptr;
	if (!BasePosePath.IsEmpty())
	{
		BasePose = AnimEdit_LoadAsset<UAnimSequence>(BasePosePath, TEXT("UAnimSequence"), Err);
		if (!BasePose) { return Err; }
	}

	// Editor-parity commit (see CLAUDE.md CDO-write guidance): bracket with
	// Modify/PreEditChange + PostEditChangeProperty on AdditiveAnimType, the
	// property whose change triggers additive rebake/recompression.
	FProperty* AdditiveProp = FindFProperty<FProperty>(UAnimSequence::StaticClass(), TEXT("AdditiveAnimType"));
	Sequence->SetFlags(RF_Transactional);
	Sequence->Modify();
	Sequence->PreEditChange(AdditiveProp);

	Sequence->AdditiveAnimType = AdditiveType;
	if (AdditiveType == AAT_None)
	{
		Sequence->RefPoseType = ABPT_None;
		Sequence->RefPoseSeq = nullptr;
		Sequence->RefFrameIndex = 0;
	}
	else
	{
		if (bHasBasePoseType) { Sequence->RefPoseType = BasePoseType; }
		if (BasePose) { Sequence->RefPoseSeq = BasePose; }
		if (RefFrame >= 0) { Sequence->RefFrameIndex = RefFrame; }
	}

	FPropertyChangedEvent ChangeEvent(AdditiveProp, EPropertyChangeType::ValueSet);
	Sequence->PostEditChangeProperty(ChangeEvent);
	Sequence->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Sequence->GetPathName());
	TSharedPtr<FJsonObject> AdditiveObj = MakeShareable(new FJsonObject);
	AdditiveObj->SetStringField(TEXT("anim_type"), TypeStr);
	AdditiveObj->SetStringField(TEXT("base_pose_type"), bHasBasePoseType ? BasePoseTypeStr : FString());
	AdditiveObj->SetStringField(TEXT("base_pose_animation"), BasePose ? BasePose->GetPathName() : FString());
	AdditiveObj->SetNumberField(TEXT("ref_frame_index"), Sequence->RefFrameIndex);
	Result->SetObjectField(TEXT("additive"), AdditiveObj);
	AnimEdit_MarkStaged(Result, Path);
	return Result;
}

//------------------------------------------------------------------------------
// anim.blendspace_create — new empty blend space asset on a skeleton
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimBlendSpaceCreateToJson(const FString& DestPath, const FString& ClassName, const FString& SkeletonPath)
{
	UClass* BlendSpaceClass = AnimEdit_BlendSpaceClass(ClassName);
	if (!BlendSpaceClass)
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Invalid class '%s' (BlendSpace | BlendSpace1D | AimOffsetBlendSpace | AimOffsetBlendSpace1D)"), *ClassName));
	}

	TSharedPtr<FJsonObject> Err;
	USkeleton* Skeleton = AnimEdit_LoadAsset<USkeleton>(SkeletonPath, TEXT("USkeleton"), Err);
	if (!Skeleton) { return Err; }

	FString PackageDir, AssetName;
	if (!AnimEdit_SplitDest(DestPath, PackageDir, AssetName, Err)) { return Err; }
	const FString PackageName = PackageDir / AssetName;

	if (LoadObject<UObject>(nullptr, *DestPath))
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Asset already exists: %s"), *DestPath));
	}

	UPackage* Package = CreatePackage(*PackageName);
	UBlendSpaceBase* BlendSpace = NewObject<UBlendSpaceBase>(Package, BlendSpaceClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	BlendSpace->SetSkeleton(Skeleton);
	FAssetRegistryModule::AssetCreated(BlendSpace);
	BlendSpace->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = AnimEdit_MakeBlendSpaceSummary(BlendSpace);
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	AnimEdit_MarkStaged(Result, DestPath);
	return Result;
}

//------------------------------------------------------------------------------
// anim.blendspace_set_axis — axis name/range/grid (protected BlendParameters
// via reflection, font-ops style). Editor-parity PostEditChange remaps or
// snaps existing samples the same way the blend space editor does.
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimBlendSpaceSetAxisToJson(const FString& Path, int32 AxisIndex, const FString& AxisName, bool bHasMin, double Min, bool bHasMax, double Max, bool bHasGridNum, int32 GridNum)
{
	TSharedPtr<FJsonObject> Err;
	UBlendSpaceBase* BlendSpace = AnimEdit_LoadAsset<UBlendSpaceBase>(Path, TEXT("UBlendSpaceBase"), Err);
	if (!BlendSpace) { return Err; }

	const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
	const int32 MaxAxis = bIs1D ? 0 : 1;
	if (AxisIndex < 0 || AxisIndex > MaxAxis)
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Axis %d out of range for %s (valid: 0..%d)"), AxisIndex, *BlendSpace->GetClass()->GetName(), MaxAxis));
	}
	if (AxisName.IsEmpty() && !bHasMin && !bHasMax && !bHasGridNum)
	{
		return AnimEdit_MakeError(TEXT("Nothing to set (pass name, min, max, and/or grid-num)"));
	}
	if (bHasGridNum && GridNum < 1)
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("grid-num must be >= 1 (got %d)"), GridNum));
	}

	FStructProperty* Prop = FindFProperty<FStructProperty>(UBlendSpaceBase::StaticClass(), TEXT("BlendParameters"));
	if (!Prop)
	{
		return AnimEdit_MakeError(TEXT("Reflection lookup of UBlendSpaceBase::BlendParameters failed (engine layout change?)"));
	}

	// PreEditChange with the Min/Max member property caches the old axis
	// ranges (PreviousAxisMinMaxValues) so PostEditChange can remap samples —
	// same path the details panel takes. Use the struct's Min FProperty so
	// the engine's name checks match.
	FProperty* MinProp = FindFProperty<FProperty>(FBlendParameter::StaticStruct(), TEXT("Min"));
	FProperty* GridNumProp = FindFProperty<FProperty>(FBlendParameter::StaticStruct(), TEXT("GridNum"));

	BlendSpace->SetFlags(RF_Transactional);
	BlendSpace->Modify();
	const bool bRangeChanged = bHasMin || bHasMax;
	BlendSpace->PreEditChange(bRangeChanged ? MinProp : GridNumProp);

	FBlendParameter* Param = Prop->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, AxisIndex);
	if (!AxisName.IsEmpty()) { Param->DisplayName = AxisName; }
	if (bHasMin) { Param->Min = (float)Min; }
	if (bHasMax) { Param->Max = (float)Max; }
	if (bHasGridNum) { Param->GridNum = GridNum; }
	if (Param->Min >= Param->Max)
	{
		// Roll nothing back — the caller sees the error before PostEditChange
		// commits any remap; the in-memory object is reloaded next call anyway.
		return AnimEdit_MakeError(FString::Printf(TEXT("Axis min must be < max (got %f..%f)"), Param->Min, Param->Max));
	}

	// MemberProperty = BlendParameters, Property = Min/GridNum: this is the
	// shape UBlendSpaceBase::PostEditChangeProperty keys its sample
	// remap/snap behavior on.
	FPropertyChangedEvent ChangeEvent(bRangeChanged ? MinProp : GridNumProp, EPropertyChangeType::ValueSet);
	ChangeEvent.SetActiveMemberProperty(Prop);
	BlendSpace->PostEditChangeProperty(ChangeEvent);

	AnimEdit_ResampleGrid(BlendSpace);
	BlendSpace->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = AnimEdit_MakeBlendSpaceSummary(BlendSpace);
	TSharedPtr<FJsonObject> AxisObj = MakeShareable(new FJsonObject);
	AxisObj->SetNumberField(TEXT("index"), AxisIndex);
	AxisObj->SetStringField(TEXT("display_name"), Param->DisplayName);
	AxisObj->SetNumberField(TEXT("min"), Param->Min);
	AxisObj->SetNumberField(TEXT("max"), Param->Max);
	AxisObj->SetNumberField(TEXT("grid_num"), Param->GridNum);
	Result->SetObjectField(TEXT("axis"), AxisObj);
	AnimEdit_MarkStaged(Result, Path);
	return Result;
}

//------------------------------------------------------------------------------
// anim.blendspace_add_sample — add + revalidate + regrid
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimBlendSpaceAddSampleToJson(const FString& Path, const FString& AnimationPath, double X, double Y)
{
	TSharedPtr<FJsonObject> Err;
	UBlendSpaceBase* BlendSpace = AnimEdit_LoadAsset<UBlendSpaceBase>(Path, TEXT("UBlendSpaceBase"), Err);
	if (!BlendSpace) { return Err; }
	UAnimSequence* Animation = AnimEdit_LoadAsset<UAnimSequence>(AnimationPath, TEXT("UAnimSequence"), Err);
	if (!Animation) { return Err; }

	BlendSpace->SetFlags(RF_Transactional);
	BlendSpace->Modify();

	if (!BlendSpace->AddSample(Animation, FVector((float)X, (float)Y, 0.f)))
	{
		return AnimEdit_MakeError(FString::Printf(
			TEXT("AddSample rejected %s at (%g, %g) — causes: skeleton mismatch, additive-type mismatch with existing samples, value outside axis range, or too close to an existing sample"),
			*AnimationPath, X, Y));
	}

	AnimEdit_ResampleGrid(BlendSpace);
	BlendSpace->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = AnimEdit_MakeBlendSpaceSummary(BlendSpace);
	Result->SetNumberField(TEXT("sample_index"), BlendSpace->GetNumberOfBlendSamples() - 1);
	AnimEdit_MarkStaged(Result, Path);
	return Result;
}

//------------------------------------------------------------------------------
// anim.blendspace_remove_sample — remove by index + regrid.
// NOTE: engine removal is RemoveAtSwap — the last sample takes the removed
// index. Re-export before removing another.
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimBlendSpaceRemoveSampleToJson(const FString& Path, int32 SampleIndex)
{
	TSharedPtr<FJsonObject> Err;
	UBlendSpaceBase* BlendSpace = AnimEdit_LoadAsset<UBlendSpaceBase>(Path, TEXT("UBlendSpaceBase"), Err);
	if (!BlendSpace) { return Err; }

	BlendSpace->SetFlags(RF_Transactional);
	BlendSpace->Modify();

	if (!BlendSpace->DeleteSample(SampleIndex))
	{
		return AnimEdit_MakeError(FString::Printf(TEXT("Sample index %d out of range (blend space has %d)"), SampleIndex, BlendSpace->GetNumberOfBlendSamples()));
	}

	AnimEdit_ResampleGrid(BlendSpace);
	BlendSpace->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = AnimEdit_MakeBlendSpaceSummary(BlendSpace);
	Result->SetBoolField(TEXT("index_order_changed"), true);
	AnimEdit_MarkStaged(Result, Path);
	return Result;
}

//------------------------------------------------------------------------------
// anim.save — persist a dirty animation-asset package (font.save semantics)
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimSaveToJson(const FString& Path)
{
	TSharedPtr<FJsonObject> Err;
	UObject* Asset = AnimEdit_LoadAsset<UObject>(Path, TEXT("UObject"), Err);
	if (!Asset) { return Err; }

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		return AnimEdit_MakeError(TEXT("Asset has no outer package"));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	if (!Package->IsDirty())
	{
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("not_dirty"), true);
		return Result;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!bSaved)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("SavePackages reported failure"));
	}
	return Result;
}
