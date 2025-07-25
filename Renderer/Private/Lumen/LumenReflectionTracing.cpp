// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "LumenReflections.h"
#include "RayTracedTranslucency.h"
#include "HairStrands/HairStrandsData.h"
#include "LumenHardwareRayTracingCommon.h"
#include "LumenTracingUtils.h"
#include "ShaderPrintParameters.h"

int32 GLumenReflectionScreenTraces = 1;
FAutoConsoleVariableRef CVarLumenReflectionScreenTraces(
	TEXT("r.Lumen.Reflections.ScreenTraces"),
	GLumenReflectionScreenTraces,
	TEXT("Whether to trace against the screen for reflections before falling back to other methods."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

// Rendering project setting
int32 GLumenScreenTracingSource = 0;
FAutoConsoleVariableRef CVarLumenScreenTracingSource(
	TEXT("r.Lumen.ScreenTracingSource"),
	GLumenScreenTracingSource,
	TEXT("Specifies the source texture for Lumen's screen trace hits\n")
	TEXT("0: Scene Color (no translucency and noise from small emissive elements)\n")
	TEXT("1: Anti-aliased Scene Color (translucency intersected with the opaque depths, less noise from small emissive elements)"),
	ECVF_RenderThreadSafe
);

int32 GLumenReflectionHierarchicalScreenTracesMaxIterations = 50;
FAutoConsoleVariableRef CVarLumenReflectionHierarchicalScreenTracesMaxIterations(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.MaxIterations"),
	GLumenReflectionHierarchicalScreenTracesMaxIterations,
	TEXT("Max iterations for HZB tracing."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionScreenTracesMinimumOccupancy = 0;
FAutoConsoleVariableRef CVarLumenReflectionHierarchicalScreenTracesMinimumOccupancy(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.MinimumOccupancy"),
	GLumenReflectionScreenTracesMinimumOccupancy,
	TEXT("Minimum number of threads still tracing before aborting the trace.  Can be used for scalability to abandon traces that have a disproportionate cost."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold = .005f;
FAutoConsoleVariableRef GVarLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.RelativeDepthThickness"),
	GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold,
	TEXT("Determines depth thickness of objects hit by HZB tracing, as a relative depth threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionHierarchicalScreenTraceHistoryDepthTestRelativeThickness = .005f;
FAutoConsoleVariableRef GVarLumenReflectionHierarchicalScreenTraceHistoryDepthTestRelativeThickness(
	TEXT("r.Lumen.Reflections.HierarchicalScreenTraces.HistoryDepthTestRelativeThickness"),
	GLumenReflectionHierarchicalScreenTraceHistoryDepthTestRelativeThickness,
	TEXT("Distance between HZB trace hit and previous frame scene depth from which to allow hits, as a relative depth threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionHairStrands_VoxelTrace = 1;
FAutoConsoleVariableRef GVarLumenReflectionHairStrands_VoxelTrace(
	TEXT("r.Lumen.Reflections.HairStrands.VoxelTrace"),
	GLumenReflectionHairStrands_VoxelTrace,
	TEXT("Whether to trace against hair voxel structure for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionHairStrands_ScreenTrace = 1;
FAutoConsoleVariableRef GVarLumenReflectionHairStrands_ScreenTrace(
	TEXT("r.Lumen.Reflections.HairStrands.ScreenTrace"),
	GLumenReflectionHairStrands_ScreenTrace,
	TEXT("Whether to trace against hair depth for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTraceCompactionGroupSizeInTiles = 16;
FAutoConsoleVariableRef GVarLumenReflectionTraceCompactionGroupSizeInTiles(
	TEXT("r.Lumen.Reflections.TraceCompaction.GroupSizeInTraceTiles"),
	GLumenReflectionTraceCompactionGroupSizeInTiles,
	TEXT("Size of the trace compaction threadgroup.  Larger group = better coherency in the compacted traces.  Currently only supported by WaveOps path."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionTraceCompactionWaveOps = 1;
FAutoConsoleVariableRef CVarLumenReflectionTraceCompactionWaveOps(
	TEXT("r.Lumen.Reflections.TraceCompaction.WaveOps"),
	GLumenReflectionTraceCompactionWaveOps,
	TEXT("Whether to use Wave Ops path for trace compaction."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionsSampleSceneColorAtHit = 1;
FAutoConsoleVariableRef CVarLumenReflectionsSampleSceneColorAtHit(
	TEXT("r.Lumen.Reflections.SampleSceneColorAtHit"),
	GLumenReflectionsSampleSceneColorAtHit,
	TEXT("Whether to sample SceneColor on reflection ray hits (both SWRT and HWRT). Useful for hiding areas where Screen Traces gave up when they went behind a foreground object. 0 - Disable. 1 - Enable only when screen space traces are enabled. 2 - Always enable."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> GVarLumenReflectionSampleSceneColorRelativeDepthThreshold(
	TEXT("r.Lumen.Reflections.SampleSceneColorRelativeDepthThickness"),
	0.01f,
	TEXT("Depth threshold that controls how close ray hits have to be to the depth buffer, before sampling SceneColor is allowed."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> CVarLumenReflectionsSampleSceneColorNormalTreshold(
	TEXT("r.Lumen.Reflections.SampleSceneColorNormalTreshold"),
	85.0f,
	TEXT("Normal threshold in degrees that controls how close ray hit normal and screen normal have to be, before sampling SceneColor is allowed. 0 - only exactly matching normals allowed. 180 - all normals allowed."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> GVarLumenReflectionsFarFieldSampleSceneColorRelativeDepthThreshold(
	TEXT("r.Lumen.Reflections.FarField.SampleSceneColorRelativeDepthThickness"),
	0.1f,
	TEXT("Depth threshold for far field traces that controls how close ray hits have to be to the depth buffer, before sampling SceneColor is allowed."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> CVarLumenReflectionsFarFieldSampleSceneColorNormalTreshold(
	TEXT("r.Lumen.Reflections.FarField.SampleSceneColorNormalTreshold"),
	85.0f,
	TEXT("Normal threshold in degrees for far field traces that controls how close ray hit normal and screen normal have to be, before sampling SceneColor is allowed. 0 - only exactly matching normals allowed. 180 - all normals allowed."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenReflectionsDistantScreenTraces = 1;
FAutoConsoleVariableRef CVarLumenReflectionsDistantScreenTraces(
	TEXT("r.Lumen.Reflections.DistantScreenTraces"),
	GLumenReflectionsDistantScreenTraces,
	TEXT("Whether to do a linear screen trace starting where Lumen Scene ends to handle distant reflections."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionDistantScreenTraceSlopeCompareTolerance = 2.0f;
FAutoConsoleVariableRef GVarLumenReflectionDistantScreenTraceDepthThreshold(
	TEXT("r.Lumen.Reflections.DistantScreenTraces.DepthThreshold"),
	GLumenReflectionDistantScreenTraceSlopeCompareTolerance,
	TEXT("Depth threshold for the linear screen traces done where other traces have missed."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenReflectionDistantScreenTraceMaxTraceDistance = 200000.0f;
FAutoConsoleVariableRef GVarLumenReflectionDistantScreenTraceMaxTraceDistance(
	TEXT("r.Lumen.Reflections.DistantScreenTraces.MaxTraceDistance"),
	GLumenReflectionDistantScreenTraceMaxTraceDistance,
	TEXT("Trace distance of distant screen traces."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

TAutoConsoleVariable<float> CVarLumenReflectionDistantScreenTraceStepOffsetBias(
	TEXT("r.Lumen.Reflections.DistantScreenTraces.StepOffsetBias"),
	0.f,
	TEXT("A bias added to the ray step offset to shift the center of jittering."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsMaxBounces(
	TEXT("r.Lumen.Reflections.MaxBounces"),
	0,
	TEXT("Sets the maximum number of recursive reflection bounces. Values above 0 override Post Process Volume settings. 1 means a single reflection ray (no secondary reflections in mirrors). Currently only supported by Hardware Ray Tracing with Hit Lighting."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsVisualizeTraces(
	TEXT("r.Lumen.Reflections.VisualizeTraces"),
	0,
	TEXT("Whether to visualize reflection traces from cursor position, useful for debugging"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsHardwareRayTracingTranslucentMaxRefractionBounces(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.Translucent.MaxRefractionBounces"),
	0,
	TEXT("The maximum count of refraction event to trace."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenReflectionsHardwareRayTracingFirstPersonMinimumHitDistance(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.FirstPersonMinimumHitDistance"),
	4.0f,
	TEXT("The minimum hit distance when handing off a ray to HWRT after missing in screen space but potentially intersecting first person world space representation primitives.\n")
	TEXT("These primitives are not visible on screen, so we must ensure HWRT has a chance to intersect them, but we also want to ensure that screen tracing covers a minimum distance to avoid self intersection artifacts between Nanite geometry and the fallback meshes in the BVH."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRayTracedTranslucencyAllowed(
	TEXT("r.RayTracedTranslucency.Allowed"),
	-1,
	TEXT("Runtime switch for ray traced translucency. Requires Hardware Ray Tracing with Hit Lighting.\n")
	TEXT("Enabling this will disable r.RayTracing.Translucency and translucency front layer reflection.\n")
	TEXT("Set to -1 to use post process volume Translucency Type"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<bool> CVarRayTracedTranslucencyForceOpaque(
	TEXT("r.RayTracedTranslucency.ForceOpaque"),
	false,
	TEXT("Force rays traced for ray traced translucency to be tagged as opaque so that the Any-Hit shader does not need to be executed.\n")
	TEXT("Enabling this may produced artifacts with masked materials seen in translucent reflections and refractions (default: off)"),
	ECVF_Scalability | ECVF_RenderThreadSafe);


static TAutoConsoleVariable<int32> CVarRayTracedTranslucencyUseRayTracedRefraction(
	TEXT("r.RayTracedTranslucency.UseRayTracedRefraction"),
	-1,
	TEXT("Whether to use ray traced refraction which currently doesn't work well with rough refraction or simulate it using a screen space effect.\n")
	TEXT("Set to -1 to use post process volume setting"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracedTranslucencySampleTranslucentReflectionInReflections(
	TEXT("r.RayTracedTranslucency.SampleTranslucentReflectionInReflections"),
	1,
	TEXT("Whether to stochastically pick between reflection and refraction when hitting translucent surfaces in reflection rays.\n")
	TEXT("If disabled, translucent objects in reflections will lose reflections but reduces noise especially during movement."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarRayTracedTranslucencyPathThroughputThreshold(
	TEXT("r.RayTracedTranslucency.PathThroughputThreshold"),
	0.001f,
	TEXT("Path throughput threshold below which a path will be terminated."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracedTranslucencyDownsampleFactor(
	TEXT("r.RayTracedTranslucency.DownsampleFactor"),
	1,
	TEXT("Experimental. Do not use."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracedTranslucencyMaxPrimaryHitEvents(
	TEXT("r.RayTracedTranslucency.MaxPrimaryHitEvents"),
	-1,
	TEXT("Maximum number of hit events allowed on primary ray paths. Set to -1 to use post process volume settings"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracedTranslucencyMaxSecondaryHitEvents(
	TEXT("r.RayTracedTranslucency.MaxSecondaryHitEvents"),
	-1,
	TEXT("Maximum number of hit events allowed on secondary ray paths. Set to -1 to use post process volume settings."),
	ECVF_Scalability | ECVF_RenderThreadSafe);

bool LumenReflections::UseScreenTraces(const FViewInfo& View)
{
	return GLumenReflectionScreenTraces != 0 && View.Family->EngineShowFlags.LumenScreenTraces && View.FinalPostProcessSettings.LumenReflectionsScreenTraces;
}

bool LumenReflections::UseDistantScreenTraces(const FViewInfo& View)
{
	return GLumenReflectionsDistantScreenTraces != 0 && UseScreenTraces(View);
}

float LumenReflections::GetDistantScreenTraceStepOffsetBias()
{
	return CVarLumenReflectionDistantScreenTraceStepOffsetBias.GetValueOnRenderThread();
}

float LumenReflections::GetSampleSceneColorDepthTreshold()
{
	return GVarLumenReflectionSampleSceneColorRelativeDepthThreshold.GetValueOnRenderThread();
}

float LumenReflections::GetSampleSceneColorNormalTreshold()
{
	const float Radians = FMath::DegreesToRadians(FMath::Clamp(CVarLumenReflectionsSampleSceneColorNormalTreshold.GetValueOnRenderThread(), 0.0f, 180.0f));
	return FMath::Cos(Radians);
}

float LumenReflections::GetFarFieldSampleSceneColorDepthTreshold()
{
	return GVarLumenReflectionsFarFieldSampleSceneColorRelativeDepthThreshold.GetValueOnRenderThread();
}

float LumenReflections::GetFarFieldSampleSceneColorNormalTreshold()
{
	const float Radians = FMath::DegreesToRadians(FMath::Clamp(CVarLumenReflectionsFarFieldSampleSceneColorNormalTreshold.GetValueOnRenderThread(), 0.0f, 180.0f));
	return FMath::Cos(Radians);
}

uint32 LumenReflections::GetMaxReflectionBounces(const FViewInfo& View)
{
	int32 MaxBounces = CVarLumenReflectionsMaxBounces.GetValueOnRenderThread();
	if (MaxBounces <= 0)
	{
		MaxBounces = View.FinalPostProcessSettings.LumenMaxReflectionBounces;
	}
	return FMath::Clamp(MaxBounces, 1, 64);
}

uint32 LumenReflections::GetMaxRefractionBounces(const FViewInfo& View)
{
	int32 LumenMaxRefractionBounces = CVarLumenReflectionsHardwareRayTracingTranslucentMaxRefractionBounces.GetValueOnRenderThread();
	if (LumenMaxRefractionBounces <= 0)
	{
		LumenMaxRefractionBounces = View.FinalPostProcessSettings.LumenMaxRefractionBounces;
	}
	return FMath::Clamp(1 + LumenMaxRefractionBounces, 1, 64);	// we add one to account for the first loop in the shader that is mandatory to at least get reflection.
}

bool RayTracedTranslucency::IsEnabled(const FViewInfo& View)
{
	const int32 RayTracedTranslucencyAllowed = CVarRayTracedTranslucencyAllowed.GetValueOnRenderThread();
	const bool bTranslucencyEnabled = RayTracedTranslucencyAllowed < 0
		? View.FinalPostProcessSettings.TranslucencyType == ETranslucencyType::RayTraced
		: RayTracedTranslucencyAllowed != 0;
	
	return bTranslucencyEnabled
		&& View.Family->EngineShowFlags.Translucency
		&& View.AntiAliasingMethod != AAM_MSAA
		&& LumenHardwareRayTracing::IsRayGenSupported()
		&& Lumen::UseHardwareRayTracing(*View.Family);
}

bool RayTracedTranslucency::UseForceOpaque()
{
	return CVarRayTracedTranslucencyForceOpaque.GetValueOnRenderThread();
}

bool RayTracedTranslucency::UseRayTracedRefraction(const TArray<FViewInfo>& Views)
{
	const int32 RayTracedRefractionCVarValue = CVarRayTracedTranslucencyUseRayTracedRefraction.GetValueOnRenderThread();
	if (RayTracedRefractionCVarValue < 0)
	{
		// If any view has ray traced refraction turned on, render all views with ray traced refraction
		for (int32 Index = 0; Index < Views.Num(); ++Index)
		{
			if (Views[Index].FinalPostProcessSettings.RayTracingTranslucencyUseRayTracedRefraction)
			{
				return true;
			}
		}

		return false;
	}
	
	return CVarRayTracedTranslucencyUseRayTracedRefraction.GetValueOnRenderThread() != 0;
}

bool RayTracedTranslucency::AllowTranslucentReflectionInReflections()
{
	return CVarRayTracedTranslucencySampleTranslucentReflectionInReflections.GetValueOnRenderThread() != 0;
}

float RayTracedTranslucency::GetPathThroughputThreshold()
{
	return CVarRayTracedTranslucencyPathThroughputThreshold.GetValueOnRenderThread();
}

uint32 RayTracedTranslucency::GetDownsampleFactor(const TArray<FViewInfo>& Views)
{
	if (RayTracedTranslucency::UseRayTracedRefraction(Views))
	{
		return 1;
	}

	return FMath::Clamp(CVarRayTracedTranslucencyDownsampleFactor.GetValueOnRenderThread(), 1, 2);
}

uint32 RayTracedTranslucency::GetMaxPrimaryHitEvents(const FViewInfo& View)
{
	int32 MaxBounces = CVarRayTracedTranslucencyMaxPrimaryHitEvents.GetValueOnRenderThread();
	if (MaxBounces < 0)
	{
		MaxBounces = View.FinalPostProcessSettings.RayTracingTranslucencyMaxPrimaryHitEvents;
	}
	return FMath::Clamp(MaxBounces, 1, 64);
}

uint32 RayTracedTranslucency::GetMaxSecondaryHitEvents(const FViewInfo& View)
{
	int32 MaxBounces = CVarRayTracedTranslucencyMaxSecondaryHitEvents.GetValueOnRenderThread();
	if (MaxBounces < 0)
	{
		MaxBounces = View.FinalPostProcessSettings.RayTracingTranslucencyMaxSecondaryHitEvents;
	}
	return FMath::Clamp(MaxBounces, 0, 64);
}

class FReflectionClearTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionClearTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionClearTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
	END_SHADER_PARAMETER_STRUCT()

	class FCleatTraceMaterialId : SHADER_PERMUTATION_BOOL("CLEAT_TRACE_MATERIAL_ID");
	class FClearBackgroundVisibility : SHADER_PERMUTATION_BOOL("CLEAR_BACKGROUND_VISIBILITY");
	using FPermutationDomain = TShaderPermutationDomain<FCleatTraceMaterialId, FClearBackgroundVisibility>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FCleatTraceMaterialId>() && PermutationVector.Get<FClearBackgroundVisibility>())
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionClearTracesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionClearTracesCS", SF_Compute);

class FReflectionTraceScreenTexturesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceScreenTexturesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceScreenTexturesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHZBScreenTraceParameters, HZBScreenTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER(float, MaxHierarchicalScreenTraceIterations)
		SHADER_PARAMETER(float, RelativeDepthThickness)
		SHADER_PARAMETER(float, HistoryDepthTestRelativeThickness)
		SHADER_PARAMETER(uint32, MinimumTracingThreadOccupancy)
		SHADER_PARAMETER(FVector4f, FirstPersonWorldSpaceRepresentationBounds)
		SHADER_PARAMETER(float, FirstPersonMinimumHitDistanceOnScreenTraceMiss)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
	END_SHADER_PARAMETER_STRUCT()

	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_SCREEN");
	class FTerminateOnLowOccupancy : SHADER_PERMUTATION_BOOL("TERMINATE_ON_LOW_OCCUPANCY");
	using FPermutationDomain = TShaderPermutationDomain<FHairStrands, FTerminateOnLowOccupancy>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FTerminateOnLowOccupancy>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FTerminateOnLowOccupancy>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceScreenTexturesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceScreenTexturesCS", SF_Compute);

class FSetupCompactionIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupCompactionIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupCompactionIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactionIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionTracingTileIndirectArgs)
		SHADER_PARAMETER(uint32, CompactionThreadGroupSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupCompactionIndirectArgsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "SetupCompactionIndirectArgsCS", SF_Compute);


class FReflectionCompactTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionCompactTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionCompactTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
		SHADER_PARAMETER(uint32, CullByDistanceFromCamera)
		SHADER_PARAMETER(float, CompactionTracingEndDistanceFromCamera)
		SHADER_PARAMETER(float, CompactionMaxTraceDistance)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionTracingTileIndirectArgs)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static uint32 GetThreadGroupSize(uint32 GroupSizeInTracingTiles)
	{
		if (GroupSizeInTracingTiles == 1)
		{
			return 64;
		}
		else if (GroupSizeInTracingTiles == 2)
		{
			return 128;
		}
		else if (GroupSizeInTracingTiles <= 4)
		{
			return 256;
		}
		else if (GroupSizeInTracingTiles <= 8)
		{
			return 512;
		}

		return 1024;
	}

	class FTraceCompactionMode : SHADER_PERMUTATION_ENUM_CLASS("TRACE_COMPACTION_MODE", LumenReflections::ETraceCompactionMode);
	class FWaveOps : SHADER_PERMUTATION_BOOL("WAVE_OPS");
	class FThreadGroupSize : SHADER_PERMUTATION_SPARSE_INT("THREADGROUP_SIZE", 64, 128, 256, 512, 1024);
	using FPermutationDomain = TShaderPermutationDomain<FTraceCompactionMode, FWaveOps, FThreadGroupSize>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionCompactTracesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionCompactTracesCS", SF_Compute);

class FReflectionSortTracesByMaterialCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionSortTracesByMaterialCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionSortTracesByMaterialCS, FGlobalShader);

	class FWaveOps : SHADER_PERMUTATION_BOOL("DIM_WAVE_OPS");
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCompactedTraceTexelData)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 16; }
};

IMPLEMENT_GLOBAL_SHADER(FReflectionSortTracesByMaterialCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionSortTracesByMaterialCS", SF_Compute);


class FSetupReflectionCompactedTracesIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupReflectionCompactedTracesIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupReflectionCompactedTracesIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactTracingIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWReflectionCompactRayTraceDispatchIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupReflectionCompactedTracesIndirectArgsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "SetupCompactedTracesIndirectArgsCS", SF_Compute);

class FReflectionTraceMeshSDFsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceMeshSDFsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceMeshSDFsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenMeshSDFGridParameters, MeshSDFGridParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedReflectionTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()
		
	class FThreadGroupSize32 : SHADER_PERMUTATION_BOOL("THREADGROUP_SIZE_32");
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	class FTraceMeshSDFs : SHADER_PERMUTATION_BOOL("SCENE_TRACE_MESH_SDFS");
	class FTraceHeightfields : SHADER_PERMUTATION_BOOL("SCENE_TRACE_HEIGHTFIELDS");
	class FOffsetDataStructure : SHADER_PERMUTATION_INT("OFFSET_DATA_STRUCT", 3);
	using FPermutationDomain = TShaderPermutationDomain<FThreadGroupSize32, FHairStrands, FTraceMeshSDFs, FTraceHeightfields, FOffsetDataStructure>;

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		// FOffsetDataStructure is only used for mesh SDFs
		if (!PermutationVector.Get<FTraceMeshSDFs>())
		{
			PermutationVector.Set<FOffsetDataStructure>(0);
		}

		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_FEEDBACK"), 1);
		OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_HIGH_RES_PAGES"), 1);

		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceMeshSDFsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceMeshSDFsCS", SF_Compute);


class FReflectionTraceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FReflectionTraceVoxelsCS)
	SHADER_USE_PARAMETER_STRUCT(FReflectionTraceVoxelsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedReflectionTraceParameters, CompactedTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHZBScreenTraceParameters, HZBScreenTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER(float, RelativeDepthThickness)
		SHADER_PARAMETER(float, SampleSceneColorNormalTreshold)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DistantScreenTraceFurthestHZBTexture)
		SHADER_PARAMETER(float, DistantScreenTraceSlopeCompareTolerance)
		SHADER_PARAMETER(float, DistantScreenTraceMaxTraceDistance)
		SHADER_PARAMETER(float, DistantScreenTraceStepOffsetBias)
	END_SHADER_PARAMETER_STRUCT()

	class FThreadGroupSize32 : SHADER_PERMUTATION_BOOL("THREADGROUP_SIZE_32");
	class FTraceGlobalSDF : SHADER_PERMUTATION_BOOL("TRACE_GLOBAL_SDF");
	class FSimpleCoverageBasedExpand : SHADER_PERMUTATION_BOOL("GLOBALSDF_SIMPLE_COVERAGE_BASED_EXPAND");
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	class FSampleSceneColor : SHADER_PERMUTATION_BOOL("SAMPLE_SCENE_COLOR");
	class FDistantScreenTraces : SHADER_PERMUTATION_BOOL("DISTANT_SCREEN_TRACES");

	using FPermutationDomain = TShaderPermutationDomain<FThreadGroupSize32, FTraceGlobalSDF, FSimpleCoverageBasedExpand, FHairStrands, FRadianceCache, FSampleSceneColor, FDistantScreenTraces>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (!PermutationVector.Get<FTraceGlobalSDF>() && PermutationVector.Get<FSimpleCoverageBasedExpand>())
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		// If derived from engine show flags then precache request is optional if not set because debug modes may allow those permutations to be used
		FEngineShowFlags EngineShowFlags(ESFIM_Game);
		const bool bScreenTraces = GLumenReflectionScreenTraces != 0 && EngineShowFlags.LumenScreenTraces;
		const bool bSampleSceneColorAtHit = (GLumenReflectionsSampleSceneColorAtHit != 0 && bScreenTraces) || GLumenReflectionsSampleSceneColorAtHit == 2;
		const bool bDistantScreenTraces = GLumenReflectionsDistantScreenTraces && bScreenTraces;

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FThreadGroupSize32>() != Lumen::UseThreadGroupSize32())
		{
			return EShaderPermutationPrecacheRequest::NotUsed;
		}

		// Different than default game engine show flags then it's a development only feature
		if (PermutationVector.Get<FTraceGlobalSDF>() != Lumen::UseGlobalSDFTracing(EngineShowFlags))
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}

		// Different than default game engine show flags then it's a development only feature
		if (PermutationVector.Get<FSimpleCoverageBasedExpand>() != (Lumen::UseGlobalSDFTracing(EngineShowFlags) && Lumen::UseGlobalSDFSimpleCoverageBasedExpand()))
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}

		if (PermutationVector.Get<FSampleSceneColor>() != bSampleSceneColorAtHit)
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}

		if (PermutationVector.Get<FDistantScreenTraces>() != bDistantScreenTraces)
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}

		return EShaderPermutationPrecacheRequest::Precached;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_FEEDBACK"), 1);
		OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_HIGH_RES_PAGES"), 1);

		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FReflectionTraceVoxelsCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "ReflectionTraceVoxelsCS", SF_Compute);

class FVisualizeReflectionTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisualizeReflectionTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FVisualizeReflectionTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintUniformBuffer)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSubstrateGlobalUniformParameters, Substrate)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static int32 GetGroupSize()
	{
		return 8;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FVisualizeReflectionTracesCS, "/Engine/Private/Lumen/LumenReflectionTracing.usf", "VisualizeReflectionTracesCS", SF_Compute);

enum class ECompactedReflectionTracingIndirectArgs
{
	NumTracesDiv64 = 0 * sizeof(FRHIDispatchIndirectParameters),
	NumTracesDiv32 = 1 * sizeof(FRHIDispatchIndirectParameters),
	NumTracesDiv256 = 2 * sizeof(FRHIDispatchIndirectParameters),
	MAX = 3,
};

FCompactedReflectionTraceParameters LumenReflections::CompactTraces(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FLumenCardTracingParameters& TracingParameters,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	bool bCullByDistanceFromCamera,
	float CompactionTracingEndDistanceFromCamera,
	float CompactionMaxTraceDistance,
	ERDGPassFlags ComputePassFlags,
	ETraceCompactionMode TraceCompactionMode,
	bool bSortByMaterial)
{
	const uint32 ClosureCount = Substrate::GetSubstrateMaxClosureCount(View);
	FRDGBufferRef CompactedTraceTexelAllocator = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Lumen.Reflections.CompactedTraceTexelAllocator"));
	const int32 NumCompactedTraceTexelDataElements = ReflectionTracingParameters.ReflectionTracingBufferSize.X * ReflectionTracingParameters.ReflectionTracingBufferSize.Y * ClosureCount;
	FRDGBufferRef CompactedTraceTexelData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumCompactedTraceTexelDataElements), TEXT("Lumen.Reflections.CompactedTraceTexelData"));

	const bool bWaveOps = GLumenReflectionTraceCompactionWaveOps != 0 
		&& Lumen::UseWaveOps(View.GetShaderPlatform())
		&& GRHIMinimumWaveSize <= 32 
		&& GRHIMaximumWaveSize >= 32;

	// Only the wave ops path maintains trace order, switch to smaller groups without it to preserve coherency in the traces
	const uint32 CompactionThreadGroupSize = FReflectionCompactTracesCS::GetThreadGroupSize(bWaveOps ? GLumenReflectionTraceCompactionGroupSizeInTiles : 1);
	FRDGBufferRef ReflectionCompactionIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.CompactionIndirectArgs"));

	{
		FSetupCompactionIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupCompactionIndirectArgsCS::FParameters>();
		PassParameters->RWCompactedTraceTexelAllocator = GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT);
		PassParameters->RWReflectionCompactionIndirectArgs = GraphBuilder.CreateUAV(ReflectionCompactionIndirectArgs, PF_R32_UINT);
		PassParameters->ReflectionTracingTileIndirectArgs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionTileParameters.TracingIndirectArgs, PF_R32_UINT));
		PassParameters->CompactionThreadGroupSize = CompactionThreadGroupSize;
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		auto ComputeShader = View.ShaderMap->GetShader<FSetupCompactionIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupCompactionIndirectArgs"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	{
		FReflectionCompactTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionCompactTracesCS::FParameters>();
		PassParameters->TracingParameters = TracingParameters;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->RWCompactedTraceTexelAllocator = GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT);
		PassParameters->RWCompactedTraceTexelData = GraphBuilder.CreateUAV(CompactedTraceTexelData, PF_R32_UINT);
		PassParameters->ReflectionTracingTileIndirectArgs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReflectionTileParameters.TracingIndirectArgs, PF_R32_UINT));
		PassParameters->CullByDistanceFromCamera = bCullByDistanceFromCamera ? 1 : 0;
		PassParameters->CompactionTracingEndDistanceFromCamera = CompactionTracingEndDistanceFromCamera;
		PassParameters->CompactionMaxTraceDistance = CompactionMaxTraceDistance;
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);
		PassParameters->IndirectArgs = ReflectionCompactionIndirectArgs;
	
		FReflectionCompactTracesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FReflectionCompactTracesCS::FTraceCompactionMode>(TraceCompactionMode);
		PermutationVector.Set<FReflectionCompactTracesCS::FWaveOps>(bWaveOps);
		PermutationVector.Set<FReflectionCompactTracesCS::FThreadGroupSize>(CompactionThreadGroupSize);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionCompactTracesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			bWaveOps ? RDG_EVENT_NAME("CompactTracesOrderedWaveOps %u", CompactionThreadGroupSize) : RDG_EVENT_NAME("CompactTraces"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			ReflectionCompactionIndirectArgs,
			0);
	}

	FCompactedReflectionTraceParameters CompactedTraceParameters;

	CompactedTraceParameters.IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>((int32)ECompactedReflectionTracingIndirectArgs::MAX), TEXT("Lumen.Reflections.CompactTracingIndirectArgs"));
	CompactedTraceParameters.RayTraceDispatchIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflections.CompactRayTraceDispatchIndirectArgs"));

	{
		FSetupReflectionCompactedTracesIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupReflectionCompactedTracesIndirectArgsCS::FParameters>();
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->RWReflectionCompactTracingIndirectArgs = GraphBuilder.CreateUAV(CompactedTraceParameters.IndirectArgs, PF_R32_UINT);
		PassParameters->RWReflectionCompactRayTraceDispatchIndirectArgs = GraphBuilder.CreateUAV(CompactedTraceParameters.RayTraceDispatchIndirectArgs, PF_R32_UINT);
		PassParameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		auto ComputeShader = View.ShaderMap->GetShader<FSetupReflectionCompactedTracesIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupCompactedTracesIndirectArgs"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	// Sort by material
	if (bSortByMaterial)
	{
		FRDGBufferRef SortedCompactedTraceTexelData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumCompactedTraceTexelDataElements), TEXT("Lumen.Reflections.CompactedTraceTexelData"));

		FReflectionSortTracesByMaterialCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionSortTracesByMaterialCS::FParameters>();
		PassParameters->IndirectArgs = CompactedTraceParameters.IndirectArgs;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));
		PassParameters->CompactedTraceTexelData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelData, PF_R32_UINT));
		PassParameters->RWCompactedTraceTexelData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SortedCompactedTraceTexelData, PF_R32_UINT));
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		FReflectionSortTracesByMaterialCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FReflectionSortTracesByMaterialCS::FWaveOps>(bWaveOps);

		TShaderRef<FReflectionSortTracesByMaterialCS> ComputeShader = View.ShaderMap->GetShader<FReflectionSortTracesByMaterialCS>(PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SortTracesByMaterialCS"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			PassParameters->IndirectArgs,
			(uint32)ECompactedReflectionTracingIndirectArgs::NumTracesDiv256);

		CompactedTraceTexelData = SortedCompactedTraceTexelData;
	}

	CompactedTraceParameters.CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));
	CompactedTraceParameters.CompactedTraceTexelData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelData, PF_R32_UINT));

	return CompactedTraceParameters;
}

void SetupIndirectTracingParametersForReflections(const FViewInfo& View, FLumenIndirectTracingParameters& OutParameters)
{
	//@todo - cleanup
	OutParameters.StepFactor = 1.0f;
	extern float GDiffuseCardTraceEndDistanceFromCamera;
	OutParameters.CardTraceEndDistanceFromCamera = GDiffuseCardTraceEndDistanceFromCamera;
	OutParameters.MinSampleRadius = 0.0f;
	OutParameters.MinTraceDistance = 0.0f;
	OutParameters.MaxTraceDistance = Lumen::GetMaxTraceDistance(View);
	extern FLumenGatherCvarState GLumenGatherCvars;

	bool OrthoOverrideMeshDF = false;
	if(!View.IsPerspectiveProjection())
	{
		const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Lumen.Ortho.OverrideMeshDFTraceDistances"));
		if (CVar)
		{
			OrthoOverrideMeshDF = CVar->GetValueOnRenderThread() > 0;
		}
	}

	if (OrthoOverrideMeshDF)
	{
		float TraceSDFDistance = FMath::Clamp(View.ViewMatrices.GetOrthoDimensions().GetMax(), OutParameters.MinTraceDistance, OutParameters.MaxTraceDistance);
		OutParameters.MaxMeshSDFTraceDistance = TraceSDFDistance;
		OutParameters.CardTraceEndDistanceFromCamera = FMath::Max(GDiffuseCardTraceEndDistanceFromCamera, TraceSDFDistance);
	}
	else
	{
		OutParameters.MaxMeshSDFTraceDistance = FMath::Clamp(GLumenGatherCvars.MeshSDFTraceDistance, OutParameters.MinTraceDistance, OutParameters.MaxTraceDistance);
		OutParameters.CardTraceEndDistanceFromCamera = GDiffuseCardTraceEndDistanceFromCamera;
	}

	OutParameters.SurfaceBias = FMath::Clamp(GLumenGatherCvars.SurfaceBias, .01f, 100.0f);
	OutParameters.CardInterpolateInfluenceRadius = 10.0f;
	OutParameters.DiffuseConeHalfAngle = 0.0f;
	OutParameters.TanDiffuseConeHalfAngle = 0.0f;
	OutParameters.SpecularFromDiffuseRoughnessStart = 0.0f;
	OutParameters.SpecularFromDiffuseRoughnessEnd = 0.0f;
	OutParameters.HeightfieldMaxTracingSteps = Lumen::GetHeightfieldMaxTracingSteps();
}

FLumenHZBScreenTraceParameters SetupHZBScreenTraceParameters(
	FRDGBuilder& GraphBuilder, 
	const FViewInfo& View,
	const FSceneTextures& SceneTextures)
{
	FRDGTextureRef CurrentSceneColor = SceneTextures.Color.Resolve;

	FRDGTextureRef InputColor = CurrentSceneColor;
	FIntPoint ViewportOffset = View.ViewRect.Min;
	FIntPoint ViewportExtent = View.ViewRect.Size();
	FIntPoint PrevColorBufferSize = SceneTextures.Config.Extent;
	int32 InputColorSliceIndex = INDEX_NONE;

	if (View.PrevViewInfo.CustomSSRInput.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.CustomSSRInput.RT[0]);
		ViewportOffset = View.PrevViewInfo.CustomSSRInput.ViewportRect.Min;
		ViewportExtent = View.PrevViewInfo.CustomSSRInput.ViewportRect.Size();
		PrevColorBufferSize = InputColor->Desc.Extent;
	}
	else if (View.PrevViewInfo.TemporalAAHistory.IsValid() && GLumenScreenTracingSource == 1)
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.TemporalAAHistory.RT[0]);
		ViewportOffset = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Min;
		ViewportExtent = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Size();
		PrevColorBufferSize = InputColor->Desc.Extent;

		if (InputColor->Desc.ArraySize > 1)
		{
			InputColorSliceIndex = View.PrevViewInfo.TemporalAAHistory.OutputSliceIndex;
		}
	}
	else if (View.PrevViewInfo.ScreenSpaceRayTracingInput.IsValid())
	{
		InputColor = GraphBuilder.RegisterExternalTexture(View.PrevViewInfo.ScreenSpaceRayTracingInput);
		ViewportOffset = View.PrevViewInfo.ViewRect.Min;
		ViewportExtent = View.PrevViewInfo.ViewRect.Size();
		PrevColorBufferSize = InputColor->Desc.Extent;
	}

	FLumenHZBScreenTraceParameters Parameters;
	Parameters.HZBParameters = GetHZBParameters(GraphBuilder, View, EHZBType::ClosestHZB);

	{
		const float InvPrevColorBufferSizeX = 1.0f / PrevColorBufferSize.X;
		const float InvPrevColorBufferSizeY = 1.0f / PrevColorBufferSize.Y;

		Parameters.PrevScreenPositionScaleBias = FVector4f(
			ViewportExtent.X * 0.5f * InvPrevColorBufferSizeX,
			-ViewportExtent.Y * 0.5f * InvPrevColorBufferSizeY,
			(ViewportExtent.X * 0.5f + ViewportOffset.X) * InvPrevColorBufferSizeX,
			(ViewportExtent.Y * 0.5f + ViewportOffset.Y) * InvPrevColorBufferSizeY);

		FIntPoint ViewportOffsetForDepth = View.PrevViewInfo.ViewRect.Min;
		FIntPoint ViewportExtentForDepth = View.PrevViewInfo.ViewRect.Size();

		FIntPoint HistorySceneTextureExtent = SceneTextures.Config.Extent;
		if (View.PrevViewInfo.DepthBuffer)
		{
			HistorySceneTextureExtent = View.PrevViewInfo.DepthBuffer->GetDesc().Extent;
		}

		float InvBufferSizeX = 1.0f / HistorySceneTextureExtent.X;
		float InvBufferSizeY = 1.0f / HistorySceneTextureExtent.Y;

		Parameters.PrevScreenPositionScaleBiasForDepth = FVector4f(
			ViewportExtentForDepth.X * 0.5f * InvBufferSizeX,
			-ViewportExtentForDepth.Y * 0.5f * InvBufferSizeY,
			(ViewportExtentForDepth.X * 0.5f + ViewportOffsetForDepth.X) * InvBufferSizeX,
			(ViewportExtentForDepth.Y * 0.5f + ViewportOffsetForDepth.Y) * InvBufferSizeY);
	}

	FScreenPassTextureViewportParameters PrevSceneColorParameters = GetScreenPassTextureViewportParameters(FScreenPassTextureViewport(InputColor, FIntRect(ViewportOffset, ViewportOffset + ViewportExtent)));
	Parameters.PrevSceneColorBilinearUVMin = PrevSceneColorParameters.UVViewportBilinearMin;
	Parameters.PrevSceneColorBilinearUVMax = PrevSceneColorParameters.UVViewportBilinearMax;

	Parameters.PrevSceneColorPreExposureCorrection = InputColor != CurrentSceneColor ? View.PreExposure / View.PrevViewInfo.SceneColorPreExposure : 1.0f;

	Parameters.PrevSceneColorTexture = InputColorSliceIndex >= 0 ?
		GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForSlice(InputColor, InputColorSliceIndex)) :
		GraphBuilder.CreateSRV(InputColor);
	Parameters.HistorySceneDepth = View.ViewState && View.ViewState->StochasticLighting.SceneDepthHistory ? GraphBuilder.RegisterExternalTexture(View.ViewState->StochasticLighting.SceneDepthHistory) : SceneTextures.Depth.Target;

	return Parameters;
}

void TraceReflections(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenSceneFrameTemporaries& FrameTemporaries,
	bool bTraceMeshObjects,
	const FSceneTextures& SceneTextures,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	const FLumenMeshSDFGridParameters& InMeshSDFGridParameters,
	bool bUseRadianceCache,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	const FBoxSphereBounds& FirstPersonWorldSpaceRepresentationViewBounds,
	ERDGPassFlags ComputePassFlags)
{
	{
		FReflectionClearTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionClearTracesCS::FParameters>();
		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		FReflectionClearTracesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FReflectionClearTracesCS::FCleatTraceMaterialId>(ReflectionTracingParameters.TraceMaterialId != nullptr);
		PermutationVector.Set<FReflectionClearTracesCS::FClearBackgroundVisibility>(false);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionClearTracesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearTraces"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}

	FLumenCardTracingParameters TracingParameters;
	GetLumenCardTracingParameters(GraphBuilder, View, *Scene->GetLumenSceneData(View), FrameTemporaries, LumenReflections::UseSurfaceCacheFeedback(), TracingParameters);

	FLumenIndirectTracingParameters IndirectTracingParameters;
	SetupIndirectTracingParametersForReflections(View, IndirectTracingParameters);

	const FSceneTextureParameters& SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures);

	const bool bScreenTraces = LumenReflections::UseScreenTraces(View);
	const bool bSampleSceneColorAtHit = (GLumenReflectionsSampleSceneColorAtHit != 0 && bScreenTraces) || GLumenReflectionsSampleSceneColorAtHit == 2;
	const bool bDistantScreenTraces = LumenReflections::UseDistantScreenTraces(View);

	if (bScreenTraces)
	{
		FReflectionTraceScreenTexturesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceScreenTexturesCS::FParameters>();

		PassParameters->HZBScreenTraceParameters = SetupHZBScreenTraceParameters(GraphBuilder, View, SceneTextures);
		PassParameters->TracingParameters = TracingParameters;
		
		PassParameters->SceneTextures = SceneTextureParameters;

		if (PassParameters->HZBScreenTraceParameters.PrevSceneColorTexture->GetParent() == SceneTextures.Color.Resolve || !PassParameters->SceneTextures.GBufferVelocityTexture)
		{
			PassParameters->SceneTextures.GBufferVelocityTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		}

		PassParameters->MaxHierarchicalScreenTraceIterations = GLumenReflectionHierarchicalScreenTracesMaxIterations;
		PassParameters->RelativeDepthThickness = GLumenReflectionHierarchicalScreenTraceRelativeDepthThreshold * View.ViewMatrices.GetPerProjectionDepthThicknessScale();
		PassParameters->HistoryDepthTestRelativeThickness = GLumenReflectionHierarchicalScreenTraceHistoryDepthTestRelativeThickness * View.ViewMatrices.GetPerProjectionDepthThicknessScale();
		PassParameters->MinimumTracingThreadOccupancy = GLumenReflectionScreenTracesMinimumOccupancy;
		PassParameters->FirstPersonWorldSpaceRepresentationBounds = FVector4f::Zero();
		PassParameters->FirstPersonMinimumHitDistanceOnScreenTraceMiss = FMath::Max(CVarLumenReflectionsHardwareRayTracingFirstPersonMinimumHitDistance.GetValueOnRenderThread(), UE_SMALL_NUMBER);
		// If there are relevant primitives in the scene, the bounds will not be zero sized, in which case they are valid.
		if (FirstPersonWorldSpaceRepresentationViewBounds.SphereRadius > 0.0)
		{
			PassParameters->FirstPersonWorldSpaceRepresentationBounds = FVector4f(FVector3f(FirstPersonWorldSpaceRepresentationViewBounds.Origin + View.ViewMatrices.GetPreViewTranslation()), FirstPersonWorldSpaceRepresentationViewBounds.SphereRadius);
		}

		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;

		const bool bHasHairStrands = HairStrands::HasViewHairStrandsData(View) && GLumenReflectionHairStrands_ScreenTrace > 0;
		if (bHasHairStrands)
		{
			PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
		}
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		const bool bTerminateOnLowOccupancy = GLumenReflectionScreenTracesMinimumOccupancy > 0
			&& GRHISupportsWaveOperations 
			&& GRHIMinimumWaveSize <= 32 
			&& GRHIMaximumWaveSize >= 32
			&& RHISupportsWaveOperations(View.GetShaderPlatform());

		FReflectionTraceScreenTexturesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FReflectionTraceScreenTexturesCS::FHairStrands>(bHasHairStrands);
		PermutationVector.Set< FReflectionTraceScreenTexturesCS::FTerminateOnLowOccupancy>(bTerminateOnLowOccupancy);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceScreenTexturesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TraceScreen(%s)", bHasHairStrands ? TEXT("Scene, HairStrands") : TEXT("Scene")),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}
	
	bool bNeedTraceHairVoxel = HairStrands::HasViewHairStrandsVoxelData(View) && GLumenReflectionHairStrands_VoxelTrace > 0;

	if (Lumen::UseHardwareRayTracedReflections(*View.Family))
	{
		RenderLumenHardwareRayTracingReflections(
			GraphBuilder,
			SceneTextures,
			SceneTextureParameters,
			Scene,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			IndirectTracingParameters.MaxTraceDistance,
			bUseRadianceCache,
			RadianceCacheParameters,
			bSampleSceneColorAtHit,
			DiffuseIndirectMethod,
			ComputePassFlags
		);
	}
	else 
	{
		if (bTraceMeshObjects)
		{
			FLumenMeshSDFGridParameters MeshSDFGridParameters = InMeshSDFGridParameters;
			if (!MeshSDFGridParameters.NumGridCulledMeshSDFObjects)
			{
				CullForCardTracing(
					GraphBuilder,
					Scene, 
					View,
					FrameTemporaries,
					IndirectTracingParameters,
					/* out */ MeshSDFGridParameters,
					ComputePassFlags);
			}

			const bool bTraceMeshSDFs = MeshSDFGridParameters.TracingParameters.DistanceFieldObjectBuffers.NumSceneObjects > 0;
			const bool bTraceHeightfields = Lumen::UseHeightfieldTracing(*View.Family, *Scene->GetLumenSceneData(View));

			if (bTraceMeshSDFs || bTraceHeightfields)
			{
				FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
					GraphBuilder,
					View,
					TracingParameters,
					ReflectionTracingParameters,
					ReflectionTileParameters,
					true,
					IndirectTracingParameters.CardTraceEndDistanceFromCamera,
					IndirectTracingParameters.MaxMeshSDFTraceDistance,
					ComputePassFlags);

				{
					FReflectionTraceMeshSDFsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceMeshSDFsCS::FParameters>();
					PassParameters->TracingParameters = TracingParameters;
					PassParameters->MeshSDFGridParameters = MeshSDFGridParameters;
					PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
					PassParameters->IndirectTracingParameters = IndirectTracingParameters;
					PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
					PassParameters->CompactedTraceParameters = CompactedTraceParameters;
					if (bNeedTraceHairVoxel)
					{
						PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
					}
					PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

					FReflectionTraceMeshSDFsCS::FPermutationDomain PermutationVector;
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FThreadGroupSize32 >(Lumen::UseThreadGroupSize32());
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FHairStrands >(bNeedTraceHairVoxel);
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FTraceMeshSDFs >(bTraceMeshSDFs);
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FTraceHeightfields >(bTraceHeightfields);
					extern int32 GDistanceFieldOffsetDataStructure;
					PermutationVector.Set< FReflectionTraceMeshSDFsCS::FOffsetDataStructure >(GDistanceFieldOffsetDataStructure);
					PermutationVector = FReflectionTraceMeshSDFsCS::RemapPermutation(PermutationVector);
					auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceMeshSDFsCS>(PermutationVector);

					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("TraceMeshSDFs(%s)", bNeedTraceHairVoxel ? TEXT("Scene, HairStrands") : TEXT("Scene")),
						ComputePassFlags,
						ComputeShader,
						PassParameters,
						CompactedTraceParameters.IndirectArgs,
						(int32)(Lumen::UseThreadGroupSize32() ? ECompactedReflectionTracingIndirectArgs::NumTracesDiv32 : ECompactedReflectionTracingIndirectArgs::NumTracesDiv64));
					bNeedTraceHairVoxel = false;
				}
			}
		}

		FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
			GraphBuilder,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			false,
			0.0f,
			IndirectTracingParameters.MaxTraceDistance,
			ComputePassFlags);

		{
			FReflectionTraceVoxelsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionTraceVoxelsCS::FParameters>();
			PassParameters->TracingParameters = TracingParameters;
			PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
			PassParameters->IndirectTracingParameters = IndirectTracingParameters;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->CompactedTraceParameters = CompactedTraceParameters;
			PassParameters->RadianceCacheParameters = RadianceCacheParameters;
			PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);
			if (bNeedTraceHairVoxel)
			{
				PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
			}

			PassParameters->HZBScreenTraceParameters = SetupHZBScreenTraceParameters(GraphBuilder, View, SceneTextures);
			PassParameters->SceneTextures = SceneTextureParameters;

			if (PassParameters->HZBScreenTraceParameters.PrevSceneColorTexture->GetParent() == SceneTextures.Color.Resolve || !PassParameters->SceneTextures.GBufferVelocityTexture)
			{
				PassParameters->SceneTextures.GBufferVelocityTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
			}

			PassParameters->RelativeDepthThickness = LumenReflections::GetSampleSceneColorDepthTreshold() * View.ViewMatrices.GetPerProjectionDepthThicknessScale();
			PassParameters->SampleSceneColorNormalTreshold = LumenReflections::GetSampleSceneColorNormalTreshold();

			PassParameters->DistantScreenTraceFurthestHZBTexture = GetHZBTexture(View, EHZBType::FurthestHZB);
			PassParameters->DistantScreenTraceSlopeCompareTolerance = GLumenReflectionDistantScreenTraceSlopeCompareTolerance;
			PassParameters->DistantScreenTraceMaxTraceDistance = GLumenReflectionDistantScreenTraceMaxTraceDistance;
			PassParameters->DistantScreenTraceStepOffsetBias = LumenReflections::GetDistantScreenTraceStepOffsetBias();

			FReflectionTraceVoxelsCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< FReflectionTraceVoxelsCS::FThreadGroupSize32 >(Lumen::UseThreadGroupSize32());
			PermutationVector.Set< FReflectionTraceVoxelsCS::FTraceGlobalSDF >(Lumen::UseGlobalSDFTracing(View.Family->EngineShowFlags));
			PermutationVector.Set< FReflectionTraceVoxelsCS::FSimpleCoverageBasedExpand>(Lumen::UseGlobalSDFTracing(View.Family->EngineShowFlags) && Lumen::UseGlobalSDFSimpleCoverageBasedExpand());
			PermutationVector.Set< FReflectionTraceVoxelsCS::FHairStrands >(bNeedTraceHairVoxel);
			PermutationVector.Set< FReflectionTraceVoxelsCS::FRadianceCache >(bUseRadianceCache);
			PermutationVector.Set< FReflectionTraceVoxelsCS::FSampleSceneColor >(bSampleSceneColorAtHit);
			PermutationVector.Set< FReflectionTraceVoxelsCS::FDistantScreenTraces >(bDistantScreenTraces);
			auto ComputeShader = View.ShaderMap->GetShader<FReflectionTraceVoxelsCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TraceVoxels(%s)", bNeedTraceHairVoxel ? TEXT("Scene, HairStrands") : TEXT("Scene")),
				ComputePassFlags,
				ComputeShader,
				PassParameters,
				CompactedTraceParameters.IndirectArgs,
				(int32)(Lumen::UseThreadGroupSize32() ? ECompactedReflectionTracingIndirectArgs::NumTracesDiv32 : ECompactedReflectionTracingIndirectArgs::NumTracesDiv64));
			bNeedTraceHairVoxel = false;
		}
	}

	if (CVarLumenReflectionsVisualizeTraces.GetValueOnRenderThread())
	{
		ShaderPrint::SetEnabled(true);

		FVisualizeReflectionTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisualizeReflectionTracesCS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);
		ShaderPrint::SetParameters(GraphBuilder, View.ShaderPrintData, PassParameters->ShaderPrintUniformBuffer);
		
		auto ComputeShader = View.ShaderMap->GetShader<FVisualizeReflectionTracesCS>();

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VisualizeReflectionTraces"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}
}

void TraceTranslucency(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenSceneFrameTemporaries& FrameTemporaries,
	const FSceneTextures& SceneTextures,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	ERDGPassFlags ComputePassFlags,
	bool bUseRayTracedRefraction)
{
	{
		FReflectionClearTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReflectionClearTracesCS::FParameters>();
		PassParameters->ReflectionTracingParameters = ReflectionTracingParameters;
		PassParameters->ReflectionTileParameters = ReflectionTileParameters;
		PassParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

		FReflectionClearTracesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FReflectionClearTracesCS::FCleatTraceMaterialId>(false);
		PermutationVector.Set<FReflectionClearTracesCS::FClearBackgroundVisibility>(true);
		auto ComputeShader = View.ShaderMap->GetShader<FReflectionClearTracesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearTraces"),
			ComputePassFlags,
			ComputeShader,
			PassParameters,
			ReflectionTileParameters.TracingIndirectArgs,
			0);
	}

	FLumenCardTracingParameters TracingParameters;
	GetLumenCardTracingParameters(GraphBuilder, View, *Scene->GetLumenSceneData(View), FrameTemporaries, LumenReflections::UseSurfaceCacheFeedback(), TracingParameters);

	FLumenIndirectTracingParameters IndirectTracingParameters;
	SetupIndirectTracingParametersForReflections(View, IndirectTracingParameters);

	const FSceneTextureParameters& SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures);

	RenderHardwareRayTracingTranslucency(
		GraphBuilder,
		SceneTextures,
		SceneTextureParameters,
		Scene,
		View,
		TracingParameters,
		ReflectionTracingParameters,
		ReflectionTileParameters,
		IndirectTracingParameters.MaxTraceDistance,
		DiffuseIndirectMethod,
		ComputePassFlags,
		bUseRayTracedRefraction);

	// TODO: Visualizing traces
}
