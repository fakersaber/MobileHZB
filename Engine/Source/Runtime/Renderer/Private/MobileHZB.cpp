#include "MobileHZB.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PixelShaderUtils.h"

#define SL_USE_MOBILEHZB 1

static TAutoConsoleVariable<int32> CVarMobileHzbBuildLevel(
	TEXT("r.MobileHzbBuildLevel"),
	FMobileHzbSystem::kHZBTestMaxMipmap,
	TEXT("Test the Mali Device Build Level"),
	ECVF_RenderThreadSafe
);


DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Generator"), STAT_CLMM_HZBOcclusionGenerator, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Submit"), STAT_CLMM_HZBCopyOcclusionSubmit, STATGROUP_CommandListMarkers);

BEGIN_SHADER_PARAMETER_STRUCT(FMobileHZBParameters, )
SHADER_PARAMETER(FVector4, DispatchThreadIdToBufferUV)
SHADER_PARAMETER(FVector4, HZBInvDeviceZToWorldZTransform)
SHADER_PARAMETER(FVector2D, InputViewportMaxBound)
SHADER_PARAMETER(FVector2D, InvSize)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ParentTextureMip)
SHADER_PARAMETER_SAMPLER(SamplerState, ParentTextureMipSampler)
END_SHADER_PARAMETER_STRUCT()


class FMobileHZBBuildPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildPS);
	SHADER_USE_PARAMETER_STRUCT(FMobileHZBBuildPS, FGlobalShader)

	//第一个Pass,从alpha中取LinearDepth
	class FDimSceneDepth : SHADER_PERMUTATION_BOOL("FDimSceneDepth");
	using FPermutationDomain = TShaderPermutationDomain<FDimSceneDepth>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

};

class FMobileHZBBuildCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildCS);
	SHADER_USE_PARAMETER_STRUCT(FMobileHZBBuildCS, FGlobalShader)

	class FDimMipLevelCount : SHADER_PERMUTATION_RANGE_INT("DIM_MIP_LEVEL_COUNT", 1, FMobileHzbSystem::kMaxMipBatchSize);
	using FPermutationDomain = TShaderPermutationDomain<FDimMipLevelCount>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)

		SHADER_PARAMETER_UAV(RWBuffer<uint>, FurthestHZBOutput)
		//SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FurthestHZBOutput)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildPS, "/Engine/Private/MobileHZB.usf", "HZBBuildPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCS, "/Engine/Private/MobileHZB.usf", "HZBBuildCS", SF_Compute);


TUniquePtr<FMobileHzbResource> FMobileHzbSystem::MobileHzbResourcesPtr = nullptr;

FMobileHzbResource::FMobileHzbResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	const int32 NumMipsX = FMath::Max(FMath::CeilLogTwo(View.ViewRect.Width()) - 1, 1u);
	const int32 NumMipsY = FMath::Max(FMath::CeilLogTwo(View.ViewRect.Height()) - 1, 1u);
	HzbSize = FIntPoint(1 << NumMipsX, 1 << NumMipsY);
	//int32 NumMips = FMath::Min(FMath::Max(NumMipsX, NumMipsY), static_cast<int32>(FMobileHzbSystem::kHZBTestMaxMipmap));
	int32 NumMips = FMath::Max(NumMipsX, NumMipsY);

	check(FMobileHzbSystem::kHZBTestMaxMipmap <= NumMipsX && FMobileHzbSystem::kHZBTestMaxMipmap <= NumMipsY);

	if (FMobileHzbSystem::bUseCompute) {
		uint32 NumElements = 0u;
		for (auto MipIndex = 0; MipIndex < FMobileHzbSystem::kHZBTestMaxMipmap; ++MipIndex) {
			uint8 CurXMip = NumMipsX - MipIndex;
			uint8 CurYMip = NumMipsY - MipIndex;

			uint32 CurLevelSize = (1 << CurXMip) * (1 << CurYMip);
			NumElements += CurLevelSize;
		}

		NumElements >>= 1;
		MobileHzbBuffer.Initialize(sizeof(uint32), NumElements, PF_R32_UINT);
	}

	//CreateResource
	FPooledRenderTargetDesc MobileHZBFurthestDesc = FPooledRenderTargetDesc::Create2DDesc(HzbSize, PF_R16F, FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, NumMips);
#if SL_USE_MOBILEHZB
	GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, MobileHZBTexture, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#else
	GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, View.HZB, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#endif

}

FMobileHzbResource::~FMobileHzbResource() {
	if (FMobileHzbSystem::bUseCompute) {
		MobileHzbBuffer.Release();
	}
	GRenderTargetPool.FreeUnusedResource(MobileHZBTexture);
}


void FMobileHzbSystem::InitialResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {
	check(!MobileHzbResourcesPtr.IsValid());
	MobileHzbResourcesPtr = MakeUnique<FMobileHzbResource>(RHICmdList, View);
}
	
void FMobileHzbSystem::ReleaseResource() {
	check(MobileHzbResourcesPtr.IsValid());
	MobileHzbResourcesPtr.Reset();
}

void FMobileHzbSystem::ReduceBuffer(FRDGTextureSRVRef RDGTexutreMip, const FRWBuffer& MobileHzbBuffer, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel) {

	if (CurOutHzbMipLevel >= 1) {
		return;
	}
	FIntPoint SrcSize = RDGTexutreMip->Desc.Texture->Desc.Extent;
	FIntPoint DstSize = FIntPoint::DivideAndRoundUp(MobileHzbResourcesPtr->HzbSize, 1 << CurOutHzbMipLevel);

	FMobileHZBParameters ShaderParameters;
	ShaderParameters.InvSize = FVector2D(1.f / float(SrcSize.X),1.f / float(SrcSize.X));
	ShaderParameters.InputViewportMaxBound = FVector2D(float(View.ViewRect.Max.X - 0.5f) / float(SrcSize.X),float(View.ViewRect.Max.Y - 0.5f) / float(SrcSize.Y));
	ShaderParameters.DispatchThreadIdToBufferUV = FVector4(2.0f / float(SrcSize.X), 2.0f / float(SrcSize.Y), View.ViewRect.Min.X / float(SrcSize.X), View.ViewRect.Min.Y / float(SrcSize.Y));
	ShaderParameters.HZBInvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
	ShaderParameters.ParentTextureMip = RDGTexutreMip;
	ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();


	FMobileHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildCS::FParameters>();
	PassParameters->Shared = ShaderParameters;
	PassParameters->FurthestHZBOutput = MobileHzbBuffer.UAV;

	FMobileHZBBuildCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FMobileHZBBuildCS::FDimMipLevelCount>(1);


	TShaderMapRef<FMobileHZBBuildCS> ComputeShader(View.ShaderMap, PermutationVector);

	// TODO(RDG): remove ERDGPassFlags::GenerateMips to use FComputeShaderUtils::AddPass().
	ClearUnusedGraphResources(ComputeShader, PassParameters);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("ReduceHZB(mips=[%d;%d])", CurOutHzbMipLevel, 1),
		PassParameters,
		ERDGPassFlags::Compute ,
		[PassParameters, ComputeShader](FRHICommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FIntVector(64, 64, 1));
		}
	);
}

void FMobileHzbSystem::ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel) {

	//Mip为0可能有两种情况,所以需要额外DstMipLevel

	FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(RDGTexutreMip->Desc.Texture->Desc.Extent, 1 << RDGTexutreMip->Desc.MipLevel);
	FIntPoint DstSize = FIntPoint::DivideAndRoundUp(MobileHzbResourcesPtr->HzbSize, 1 << CurOutHzbMipLevel);

	FMobileHZBParameters ShaderParameters;
	ShaderParameters.InvSize = FVector2D(1.f / float(SrcSize.X), 1.f / float(SrcSize.X));
	ShaderParameters.InputViewportMaxBound = FVector2D(float(View.ViewRect.Max.X - 0.5f) / float(SrcSize.X),float(View.ViewRect.Max.Y - 0.5f) / float(SrcSize.Y));
	ShaderParameters.DispatchThreadIdToBufferUV = FVector4(2.0f / float(SrcSize.X), 2.0f / float(SrcSize.Y), View.ViewRect.Min.X / float(SrcSize.X), View.ViewRect.Min.Y / float(SrcSize.Y));
	ShaderParameters.HZBInvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
	ShaderParameters.ParentTextureMip = RDGTexutreMip;
	ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();


	FMobileHZBBuildPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildPS::FParameters>();
	PassParameters->Shared = ShaderParameters;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(RDGFurthestHZBTexture, ERenderTargetLoadAction::ENoAction, CurOutHzbMipLevel);

	FMobileHZBBuildPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FMobileHZBBuildPS::FDimSceneDepth>(CurOutHzbMipLevel == 0);  //use SceneTexture Only Mipmap is 0 

	TShaderMapRef<FMobileHZBBuildPS> PixelShader(View.ShaderMap, PermutationVector);

	// TODO(RDG): remove ERDGPassFlags::GenerateMips to use FPixelShaderUtils::AddFullscreenPass().
	ClearUnusedGraphResources(PixelShader, PassParameters);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("MobileDownsampleHZB(mip=%d) %dx%d", CurOutHzbMipLevel, DstSize.X, DstSize.Y),
		PassParameters,
		CurOutHzbMipLevel ? (ERDGPassFlags::Raster | ERDGPassFlags::GenerateMips) : ERDGPassFlags::Raster,
		[PassParameters, &View, PixelShader, DstSize](FRHICommandList& RHICmdList)
		{
			FPixelShaderUtils::DrawFullscreenPixelShader(RHICmdList, View.ShaderMap, PixelShader, *PassParameters, FIntRect(0, 0, DstSize.X, DstSize.Y));
		}
	);

}

void FMobileHzbSystem::MobileRasterBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	FRDGBuilder GraphBuilder(RHICmdList);
	const auto& SceneTexture = FMobileHzbSystem::bUseCompute ? SceneContext.SceneDepthZ : SceneContext.GetSceneColor();
	FRDGTextureRef RDGSceneTexutre = GraphBuilder.RegisterExternalTexture(SceneTexture, TEXT("RDGSceneTexture"));
	FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGSceneTexutre, 0));

	//RG的FRDGTextureSRVDesc也是封装FRHITextureSRVCreateInfo,也可以直接手动管理
	FRDGTextureRef RDGFurthestHZBTexture = GraphBuilder.RegisterExternalTexture(MobileHzbResourcesPtr->MobileHZBTexture);
	ReduceMips(RDGSceneTexutreMip, RDGFurthestHZBTexture, View, GraphBuilder, 0);

	// Reduce the next mips
	int32 MaxMipBatchSize = FMobileHzbSystem::bUseCompute ? FMobileHzbSystem::kMaxMipBatchSize : 1;
	for (int32 StartDestMip = MaxMipBatchSize; StartDestMip < /*RDGFurthestHZBTexture->Desc.NumMips*/CVarMobileHzbBuildLevel.GetValueOnRenderThread(); StartDestMip += MaxMipBatchSize) {
		FRDGTextureSRVRef RDGHzbSrvMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGFurthestHZBTexture, StartDestMip - 1));
		ReduceMips(RDGHzbSrvMip, RDGFurthestHZBTexture, View, GraphBuilder, StartDestMip);
	}

	// Update the view.
	View.HZBMipmap0Size = MobileHzbResourcesPtr->HzbSize;
	GraphBuilder.Execute();
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EGfxToGfx, MobileHzbResourcesPtr->MobileHZBTexture->GetRenderTargetItem().ShaderResourceTexture);
}

void FMobileHzbSystem::MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	check(FMobileHzbSystem::bUseCompute);

	const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	//因为RT必须是RG资源，所以让RG管理资源
	FRDGBuilder GraphBuilder(RHICmdList);
	const auto& SceneTexture = SceneContext.SceneDepthZ;
	FRDGTextureRef RDGSceneTexutre = GraphBuilder.RegisterExternalTexture(SceneTexture, TEXT("RDGSceneTexture"));
	FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGSceneTexutre, 0));

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EGfxToCompute, MobileHzbResourcesPtr->MobileHzbBuffer.UAV);
	ReduceBuffer(RDGSceneTexutreMip, MobileHzbResourcesPtr->MobileHzbBuffer, View, GraphBuilder, 0);

	// Update the view
	//#TODO: OcclusionTest也使用CS,这里就可以干掉,再根据动态传入的Query数量动态Dispatch
	View.HZBMipmap0Size = MobileHzbResourcesPtr->HzbSize;
	GraphBuilder.Execute();
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToCompute, MobileHzbResourcesPtr->MobileHzbBuffer.UAV);
}


void FMobileSceneRenderer::MobileRenderHZB(FRHICommandListImmediate& RHICmdList) {

	SCOPED_DRAW_EVENT(RHICmdList, MobileHZB);

	FSceneViewState* ViewState = (FSceneViewState*)Views[0].State;

	if (ViewState && ViewState->HZBOcclusionTests.GetNum() != 0) {

		if (!FMobileHzbSystem::MobileHzbResourcesPtr) {
			FMobileHzbSystem::InitialResource(RHICmdList, Views[0]);
		}

		//Hiz generator
		{
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBOcclusionGenerator));	
			FMobileHzbSystem::MobileRasterBuildHZB(RHICmdList, Views[0]);
			//FMobileHzbSystem::MobileComputeBuildHZB(RHICmdList, Views[0]);
		}

		//Issuse Hiz Occlusion Query
		{
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBCopyOcclusionSubmit));
#if SL_USE_MOBILEHZB
			ViewState->HZBOcclusionTests.MobileSubmit(RHICmdList, Views[0]);
			ViewState->HZBOcclusionTests.SetValidFrameNumber(ViewState->OcclusionFrameCounter);
#else
			ViewState->HZBOcclusionTests.Submit(RHICmdList, Views[0]);
#endif
		}
	}

}
