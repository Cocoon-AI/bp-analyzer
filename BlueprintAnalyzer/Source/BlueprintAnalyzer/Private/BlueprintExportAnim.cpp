// BlueprintExportAnim.cpp
// Read-only JSON export for non-Blueprint animation assets: the BlendSpace
// family (UBlendSpace / UBlendSpace1D / UAimOffsetBlendSpace(1D)), plus
// UAnimSequence and UAnimMontage.
//
// Built for BP-analysis workflows that hit anim assets: `digbp export` only
// speaks UBlueprint, and editor-side alternatives (strings over .uasset)
// can't recover sample coordinates or axis ranges. Everything here reads
// public engine API verified against 4.27 headers, with one exception:
// UBlendSpace::AxisToScaleAnimation is a protected UPROPERTY, reached via
// reflection the same way font ops reach UFont::CompositeFont.
//
// Enum values are emitted as strings via explicit switches rather than
// StaticEnum lookups — several of these are legacy namespaced UENUMs
// (ENotifyTriggerMode::Type, ERootMotionRootLock::Type) whose UEnum
// resolution is fragile in 4.27; a switch is deterministic and cheap.

#include "BlueprintExportCommandlet.h"

#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpaceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/Skeleton.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Unity-build note: unique Anim_ prefix on locals (see EditOps_* precedent).

namespace
{
	TSharedPtr<FJsonObject> Anim_MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return Out;
	}

	FString Anim_ObjectPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	void Anim_SetObjectPathField(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, const UObject* Object)
	{
		if (Object)
		{
			Out->SetStringField(Key, Object->GetPathName());
		}
		else
		{
			Out->SetField(Key, MakeShareable(new FJsonValueNull()));
		}
	}

	// --- Enum-to-string helpers (explicit switches; see file header) ---

	FString Anim_FilterInterpolationTypeToString(EFilterInterpolationType Type)
	{
		switch (Type)
		{
		case BSIT_Average: return TEXT("Average");
		case BSIT_Linear:  return TEXT("Linear");
		case BSIT_Cubic:   return TEXT("Cubic");
		default:           return FString::Printf(TEXT("Unknown(%d)"), (int32)Type);
		}
	}

	FString Anim_NotifyTriggerModeToString(ENotifyTriggerMode::Type Mode)
	{
		switch (Mode)
		{
		case ENotifyTriggerMode::AllAnimations:            return TEXT("AllAnimations");
		case ENotifyTriggerMode::HighestWeightedAnimation: return TEXT("HighestWeightedAnimation");
		case ENotifyTriggerMode::None:                     return TEXT("None");
		default:                                           return FString::Printf(TEXT("Unknown(%d)"), (int32)Mode);
		}
	}

	FString Anim_BlendSpaceAxisToString(EBlendSpaceAxis Axis)
	{
		switch (Axis)
		{
		case BSA_None: return TEXT("None");
		case BSA_X:    return TEXT("X");
		case BSA_Y:    return TEXT("Y");
		default:       return FString::Printf(TEXT("Unknown(%d)"), (int32)Axis);
		}
	}

	FString Anim_AdditiveAnimTypeToString(EAdditiveAnimationType Type)
	{
		switch (Type)
		{
		case AAT_None:                    return TEXT("None");
		case AAT_LocalSpaceBase:          return TEXT("LocalSpaceBase");
		case AAT_RotationOffsetMeshSpace: return TEXT("RotationOffsetMeshSpace");
		default:                          return FString::Printf(TEXT("Unknown(%d)"), (int32)Type);
		}
	}

	FString Anim_AdditiveBasePoseTypeToString(EAdditiveBasePoseType Type)
	{
		switch (Type)
		{
		case ABPT_None:      return TEXT("None");
		case ABPT_RefPose:   return TEXT("RefPose");
		case ABPT_AnimScaled: return TEXT("AnimScaled");
		case ABPT_AnimFrame: return TEXT("AnimFrame");
		default:             return FString::Printf(TEXT("Unknown(%d)"), (int32)Type);
		}
	}

	FString Anim_InterpolationTypeToString(EAnimInterpolationType Type)
	{
		switch (Type)
		{
		case EAnimInterpolationType::Linear: return TEXT("Linear");
		case EAnimInterpolationType::Step:   return TEXT("Step");
		default:                             return FString::Printf(TEXT("Unknown(%d)"), (int32)Type);
		}
	}

	FString Anim_RootMotionRootLockToString(ERootMotionRootLock::Type Lock)
	{
		switch (Lock)
		{
		case ERootMotionRootLock::RefPose:        return TEXT("RefPose");
		case ERootMotionRootLock::AnimFirstFrame: return TEXT("AnimFirstFrame");
		case ERootMotionRootLock::Zero:           return TEXT("Zero");
		default:                                  return FString::Printf(TEXT("Unknown(%d)"), (int32)Lock);
		}
	}

	FString Anim_AlphaBlendOptionToString(EAlphaBlendOption Option)
	{
		switch (Option)
		{
		case EAlphaBlendOption::Linear:         return TEXT("Linear");
		case EAlphaBlendOption::Cubic:          return TEXT("Cubic");
		case EAlphaBlendOption::HermiteCubic:   return TEXT("HermiteCubic");
		case EAlphaBlendOption::Sinusoidal:     return TEXT("Sinusoidal");
		case EAlphaBlendOption::QuadraticInOut: return TEXT("QuadraticInOut");
		case EAlphaBlendOption::CubicInOut:     return TEXT("CubicInOut");
		case EAlphaBlendOption::QuarticInOut:   return TEXT("QuarticInOut");
		case EAlphaBlendOption::QuinticInOut:   return TEXT("QuinticInOut");
		case EAlphaBlendOption::CircularIn:     return TEXT("CircularIn");
		case EAlphaBlendOption::CircularOut:    return TEXT("CircularOut");
		case EAlphaBlendOption::CircularInOut:  return TEXT("CircularInOut");
		case EAlphaBlendOption::ExpIn:          return TEXT("ExpIn");
		case EAlphaBlendOption::ExpOut:         return TEXT("ExpOut");
		case EAlphaBlendOption::ExpInOut:       return TEXT("ExpInOut");
		case EAlphaBlendOption::Custom:         return TEXT("Custom");
		default:                                return FString::Printf(TEXT("Unknown(%d)"), (int32)Option);
		}
	}

	// --- Shared UAnimSequenceBase pieces ---

	TSharedPtr<FJsonObject> Anim_NotifyToJson(const FAnimNotifyEvent& Event)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetStringField(TEXT("name"), Event.NotifyName.ToString());
		Out->SetNumberField(TEXT("trigger_time"), Event.GetTriggerTime());
		Out->SetNumberField(TEXT("duration"), Event.GetDuration());
		// Notify (instant) and NotifyStateClass (duration) are the instanced
		// BP/C++ notify objects; both null means a skeleton (name-only) notify.
		Out->SetStringField(TEXT("notify_class"), Event.Notify ? Event.Notify->GetClass()->GetName() : FString());
		Out->SetStringField(TEXT("notify_state_class"), Event.NotifyStateClass ? Event.NotifyStateClass->GetClass()->GetName() : FString());
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> Anim_NotifiesToJson(const UAnimSequenceBase* Sequence)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FAnimNotifyEvent& Event : Sequence->Notifies)
		{
			Out.Add(MakeShareable(new FJsonValueObject(Anim_NotifyToJson(Event))));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> Anim_CurveNamesToJson(const UAnimSequenceBase* Sequence)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FFloatCurve& Curve : Sequence->RawCurveData.FloatCurves)
		{
			Out.Add(MakeShareable(new FJsonValueString(Curve.Name.DisplayName.ToString())));
		}
		return Out;
	}

	// --- BlendSpace family ---

	TSharedPtr<FJsonObject> Anim_BlendSpaceToJson(const UBlendSpaceBase* BlendSpace)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);

		// 1D blend spaces (and their aim-offset subclass) use axis 0 only.
		const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
		const int32 NumAxes = bIs1D ? 1 : 2;
		Out->SetNumberField(TEXT("num_axes"), NumAxes);

		TArray<TSharedPtr<FJsonValue>> Axes;
		for (int32 AxisIndex = 0; AxisIndex < NumAxes; ++AxisIndex)
		{
			const FBlendParameter& Param = BlendSpace->GetBlendParameter(AxisIndex);
			TSharedPtr<FJsonObject> AxisObj = MakeShareable(new FJsonObject);
			AxisObj->SetNumberField(TEXT("index"), AxisIndex);
			AxisObj->SetStringField(TEXT("display_name"), Param.DisplayName);
			AxisObj->SetNumberField(TEXT("min"), Param.Min);
			AxisObj->SetNumberField(TEXT("max"), Param.Max);
			AxisObj->SetNumberField(TEXT("grid_num"), Param.GridNum);

			TSharedPtr<FJsonObject> InterpObj = MakeShareable(new FJsonObject);
			InterpObj->SetNumberField(TEXT("interpolation_time"), BlendSpace->InterpolationParam[AxisIndex].InterpolationTime);
			InterpObj->SetStringField(TEXT("interpolation_type"), Anim_FilterInterpolationTypeToString(BlendSpace->InterpolationParam[AxisIndex].InterpolationType));
			AxisObj->SetObjectField(TEXT("interpolation"), InterpObj);

			Axes.Add(MakeShareable(new FJsonValueObject(AxisObj)));
		}
		Out->SetArrayField(TEXT("axes"), Axes);

		TArray<TSharedPtr<FJsonValue>> Samples;
		const TArray<FBlendSample>& SampleData = BlendSpace->GetBlendSamples();
		for (int32 SampleIndex = 0; SampleIndex < SampleData.Num(); ++SampleIndex)
		{
			const FBlendSample& Sample = SampleData[SampleIndex];
			TSharedPtr<FJsonObject> SampleObj = MakeShareable(new FJsonObject);
			SampleObj->SetNumberField(TEXT("index"), SampleIndex);
			Anim_SetObjectPathField(SampleObj, TEXT("animation"), Sample.Animation);
			SampleObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
			if (!bIs1D)
			{
				SampleObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
			}
			SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
#if WITH_EDITORONLY_DATA
			SampleObj->SetBoolField(TEXT("snap_to_grid"), Sample.bSnapToGrid != 0);
			SampleObj->SetBoolField(TEXT("is_valid"), Sample.bIsValid != 0);
#endif
			Samples.Add(MakeShareable(new FJsonValueObject(SampleObj)));
		}
		Out->SetArrayField(TEXT("samples"), Samples);

		Out->SetNumberField(TEXT("target_weight_interpolation_speed"), BlendSpace->TargetWeightInterpolationSpeedPerSec);
		Out->SetStringField(TEXT("notify_trigger_mode"), Anim_NotifyTriggerModeToString(BlendSpace->NotifyTriggerMode));
		Out->SetBoolField(TEXT("rotation_blend_in_mesh_space"), BlendSpace->bRotationBlendInMeshSpace);
		Out->SetBoolField(TEXT("is_valid_additive"), BlendSpace->IsValidAdditive());

		// Which axis drives animation play-rate scaling. 2D stores a protected
		// TEnumAsByte<EBlendSpaceAxis> (reflection to cross the access
		// specifier, font-ops style); 1D stores a public bool meaning "axis X".
		if (const UBlendSpace1D* BlendSpace1D = Cast<UBlendSpace1D>(BlendSpace))
		{
			Out->SetStringField(TEXT("axis_to_scale_animation"), BlendSpace1D->bScaleAnimation ? TEXT("X") : TEXT("None"));
		}
		else if (FByteProperty* AxisProp = FindFProperty<FByteProperty>(UBlendSpace::StaticClass(), TEXT("AxisToScaleAnimation")))
		{
			const uint8 AxisValue = AxisProp->GetPropertyValue(AxisProp->ContainerPtrToValuePtr<uint8>(BlendSpace));
			Out->SetStringField(TEXT("axis_to_scale_animation"), Anim_BlendSpaceAxisToString((EBlendSpaceAxis)AxisValue));
		}

#if WITH_EDITORONLY_DATA
		Anim_SetObjectPathField(Out, TEXT("preview_base_pose"), BlendSpace->PreviewBasePose);
#endif
		return Out;
	}

	// --- Raw-track dump (anim.export tracks=true) ---
	//
	// Per-bone transform at a single frame, read from RawAnimationData — the
	// uncompressed keys FBX import writes — NOT the compressed runtime stream.
	// That makes it the right oracle for import round-trip fidelity checks:
	// comparing against source poses is exact, unaffected by compression.
	// Transforms are bone-local (relative to parent), quat as [x,y,z,w].
	// Constant tracks store a single key; sampling clamps the key index.

	TSharedPtr<FJsonValue> Anim_VectorToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShareable(new FJsonValueNumber(V.X)));
		Out.Add(MakeShareable(new FJsonValueNumber(V.Y)));
		Out.Add(MakeShareable(new FJsonValueNumber(V.Z)));
		return MakeShareable(new FJsonValueArray(Out));
	}

	TSharedPtr<FJsonValue> Anim_QuatToJson(const FQuat& Q)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShareable(new FJsonValueNumber(Q.X)));
		Out.Add(MakeShareable(new FJsonValueNumber(Q.Y)));
		Out.Add(MakeShareable(new FJsonValueNumber(Q.Z)));
		Out.Add(MakeShareable(new FJsonValueNumber(Q.W)));
		return MakeShareable(new FJsonValueArray(Out));
	}

	TArray<TSharedPtr<FJsonValue>> Anim_TracksToJson(const UAnimSequence* Sequence, int32 Frame)
	{
		const TArray<FRawAnimSequenceTrack>& RawTracks = Sequence->GetRawAnimationData();
		const TArray<FName>& TrackNames = Sequence->GetAnimationTrackNames();
		const TArray<FTrackToSkeletonMap>& TrackMap = Sequence->GetRawTrackToSkeletonMapTable();

		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 TrackIndex = 0; TrackIndex < RawTracks.Num(); ++TrackIndex)
		{
			const FRawAnimSequenceTrack& Track = RawTracks[TrackIndex];
			TSharedPtr<FJsonObject> TrackObj = MakeShareable(new FJsonObject);
			TrackObj->SetStringField(TEXT("bone"), TrackNames.IsValidIndex(TrackIndex) ? TrackNames[TrackIndex].ToString() : FString::Printf(TEXT("track_%d"), TrackIndex));
			if (TrackMap.IsValidIndex(TrackIndex))
			{
				TrackObj->SetNumberField(TEXT("skeleton_bone_index"), TrackMap[TrackIndex].BoneTreeIndex);
			}
			const FVector Pos = Track.PosKeys.Num() > 0 ? Track.PosKeys[FMath::Min(Frame, Track.PosKeys.Num() - 1)] : FVector::ZeroVector;
			const FQuat Rot = Track.RotKeys.Num() > 0 ? Track.RotKeys[FMath::Min(Frame, Track.RotKeys.Num() - 1)] : FQuat::Identity;
			const FVector Scale = Track.ScaleKeys.Num() > 0 ? Track.ScaleKeys[FMath::Min(Frame, Track.ScaleKeys.Num() - 1)] : FVector::OneVector;
			TrackObj->SetField(TEXT("pos"), Anim_VectorToJson(Pos));
			TrackObj->SetField(TEXT("rot"), Anim_QuatToJson(Rot));
			TrackObj->SetField(TEXT("scale"), Anim_VectorToJson(Scale));
			Out.Add(MakeShareable(new FJsonValueObject(TrackObj)));
		}
		return Out;
	}

	// --- UAnimSequence ---

	TSharedPtr<FJsonObject> Anim_SequenceToJson(const UAnimSequence* Sequence)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetNumberField(TEXT("length"), Sequence->SequenceLength);
		Out->SetNumberField(TEXT("num_frames"), Sequence->GetRawNumberOfFrames());
		Out->SetNumberField(TEXT("frame_rate"), Sequence->GetFrameRate());
		Out->SetNumberField(TEXT("rate_scale"), Sequence->RateScale);
		Out->SetStringField(TEXT("interpolation"), Anim_InterpolationTypeToString(Sequence->Interpolation));
		Out->SetBoolField(TEXT("enable_root_motion"), Sequence->bEnableRootMotion);
		Out->SetStringField(TEXT("root_motion_root_lock"), Anim_RootMotionRootLockToString(Sequence->RootMotionRootLock));

		TSharedPtr<FJsonObject> AdditiveObj = MakeShareable(new FJsonObject);
		AdditiveObj->SetStringField(TEXT("anim_type"), Anim_AdditiveAnimTypeToString(Sequence->AdditiveAnimType));
		AdditiveObj->SetStringField(TEXT("base_pose_type"), Anim_AdditiveBasePoseTypeToString(Sequence->RefPoseType));
		Anim_SetObjectPathField(AdditiveObj, TEXT("base_pose_animation"), Sequence->RefPoseSeq);
		AdditiveObj->SetNumberField(TEXT("ref_frame_index"), Sequence->RefFrameIndex);
		Out->SetObjectField(TEXT("additive"), AdditiveObj);

		TArray<TSharedPtr<FJsonValue>> Markers;
		for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
		{
			TSharedPtr<FJsonObject> MarkerObj = MakeShareable(new FJsonObject);
			MarkerObj->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
			MarkerObj->SetNumberField(TEXT("time"), Marker.Time);
			Markers.Add(MakeShareable(new FJsonValueObject(MarkerObj)));
		}
		Out->SetArrayField(TEXT("sync_markers"), Markers);

		Out->SetArrayField(TEXT("notifies"), Anim_NotifiesToJson(Sequence));
		Out->SetArrayField(TEXT("curves"), Anim_CurveNamesToJson(Sequence));
		return Out;
	}

	// --- UAnimMontage ---

	TSharedPtr<FJsonObject> Anim_AlphaBlendToJson(const FAlphaBlend& Blend)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetNumberField(TEXT("time"), Blend.GetBlendTime());
		Out->SetStringField(TEXT("option"), Anim_AlphaBlendOptionToString(Blend.GetBlendOption()));
		return Out;
	}

	TSharedPtr<FJsonObject> Anim_MontageToJson(const UAnimMontage* Montage)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetNumberField(TEXT("length"), Montage->SequenceLength);
		Out->SetObjectField(TEXT("blend_in"), Anim_AlphaBlendToJson(Montage->BlendIn));
		Out->SetObjectField(TEXT("blend_out"), Anim_AlphaBlendToJson(Montage->BlendOut));
		Out->SetNumberField(TEXT("blend_out_trigger_time"), Montage->BlendOutTriggerTime);
		Out->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
		Out->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);

		TArray<TSharedPtr<FJsonValue>> Slots;
		for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
		{
			TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject);
			SlotObj->SetStringField(TEXT("name"), Slot.SlotName.ToString());
			TArray<TSharedPtr<FJsonValue>> Segments;
			for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
			{
				TSharedPtr<FJsonObject> SegmentObj = MakeShareable(new FJsonObject);
				Anim_SetObjectPathField(SegmentObj, TEXT("animation"), Segment.AnimReference);
				SegmentObj->SetNumberField(TEXT("start_pos"), Segment.StartPos);
				SegmentObj->SetNumberField(TEXT("anim_start_time"), Segment.AnimStartTime);
				SegmentObj->SetNumberField(TEXT("anim_end_time"), Segment.AnimEndTime);
				SegmentObj->SetNumberField(TEXT("play_rate"), Segment.AnimPlayRate);
				SegmentObj->SetNumberField(TEXT("looping_count"), Segment.LoopingCount);
				Segments.Add(MakeShareable(new FJsonValueObject(SegmentObj)));
			}
			SlotObj->SetArrayField(TEXT("segments"), Segments);
			Slots.Add(MakeShareable(new FJsonValueObject(SlotObj)));
		}
		Out->SetArrayField(TEXT("slots"), Slots);

		TArray<TSharedPtr<FJsonValue>> Sections;
		for (const FCompositeSection& Section : Montage->CompositeSections)
		{
			TSharedPtr<FJsonObject> SectionObj = MakeShareable(new FJsonObject);
			SectionObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
			SectionObj->SetNumberField(TEXT("time"), Section.GetTime());
			SectionObj->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
			Sections.Add(MakeShareable(new FJsonValueObject(SectionObj)));
		}
		Out->SetArrayField(TEXT("sections"), Sections);

		Out->SetArrayField(TEXT("notifies"), Anim_NotifiesToJson(Montage));
		Out->SetArrayField(TEXT("curves"), Anim_CurveNamesToJson(Montage));
		return Out;
	}
}

//------------------------------------------------------------------------------
// anim.export — dispatch on asset class, dump as JSON
//------------------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::AnimExportToJson(const FString& Path, bool bTracks, int32 TracksFrame)
{
	if (Path.IsEmpty())
	{
		return Anim_MakeError(TEXT("Empty path"));
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		return Anim_MakeError(FString::Printf(TEXT("Failed to load asset at path: %s"), *Path));
	}

	UAnimationAsset* AnimAsset = Cast<UAnimationAsset>(Asset);
	if (!AnimAsset)
	{
		return Anim_MakeError(FString::Printf(
			TEXT("Asset is not an animation asset (class %s): %s"),
			*Asset->GetClass()->GetName(), *Path));
	}

	if (bTracks && !AnimAsset->IsA<UAnimSequence>())
	{
		return Anim_MakeError(FString::Printf(
			TEXT("tracks is only supported for AnimSequence (asset is %s): %s"),
			*AnimAsset->GetClass()->GetName(), *Path));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), AnimAsset->GetPathName());
	Result->SetStringField(TEXT("name"), AnimAsset->GetName());
	Result->SetStringField(TEXT("asset_class"), AnimAsset->GetClass()->GetName());
	Result->SetStringField(TEXT("skeleton"), Anim_ObjectPathOrEmpty(AnimAsset->GetSkeleton()));

	if (const UBlendSpaceBase* BlendSpace = Cast<UBlendSpaceBase>(AnimAsset))
	{
		Result->SetObjectField(TEXT("blend_space"), Anim_BlendSpaceToJson(BlendSpace));
	}
	// Montage before sequence-check ordering doesn't matter (montage is not a
	// UAnimSequence), but keep the most specific classes first for clarity.
	else if (const UAnimMontage* Montage = Cast<UAnimMontage>(AnimAsset))
	{
		Result->SetObjectField(TEXT("anim_montage"), Anim_MontageToJson(Montage));
	}
	else if (const UAnimSequence* Sequence = Cast<UAnimSequence>(AnimAsset))
	{
		Result->SetObjectField(TEXT("anim_sequence"), Anim_SequenceToJson(Sequence));
		if (bTracks)
		{
			const int32 NumFrames = Sequence->GetRawNumberOfFrames();
			if (TracksFrame < 0 || TracksFrame >= NumFrames)
			{
				return Anim_MakeError(FString::Printf(TEXT("Frame %d out of range (sequence has %d frames)"), TracksFrame, NumFrames));
			}
			Result->SetNumberField(TEXT("tracks_frame"), TracksFrame);
			Result->SetStringField(TEXT("tracks_space"), TEXT("bone_local"));
			Result->SetArrayField(TEXT("tracks"), Anim_TracksToJson(Sequence, TracksFrame));
		}
	}
	else
	{
		return Anim_MakeError(FString::Printf(
			TEXT("Unsupported animation asset class %s (supported: BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D, AnimSequence, AnimMontage): %s"),
			*AnimAsset->GetClass()->GetName(), *Path));
	}

	return Result;
}
