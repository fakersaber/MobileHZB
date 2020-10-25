#include "MobileHZB.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PixelShaderUtils.h"

#define SL_USE_MOBILEHZB 1

DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Generator"), STAT_CLMM_HZBOcclusionGenerator, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Submit"), STAT_CLMM_HZBCopyOcclusionSubmit, STATGROUP_CommandListMarkers);

//SHADER_PARAMETER_SRV(Texture2D, ParentTextureMip)

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

	class FDimMipLevelCount : SHADER_PERMUTATION_RANGE_INT("DIM_MIP_LEVEL_COUNT", 1, FMobileHZB::kMaxMipBatchSize);
	using FPermutationDomain = TShaderPermutationDomain<FDimMipLevelCount>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, FurthestHZBOutput)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildPS, "/Engine/Private/MobileHZB.usf", "HZBBuildPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCS, "/Engine/Private/MobileHZB.usf", "HZBBuildCS", SF_Compute);


void FMobileHZB::InitialResource(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	//Initial Size
	const int32 NumMipsX = FMath::Max(FMath::CeilLogTwo(View.ViewRect.Width()) - 1, 1u);
	const int32 NumMipsY = FMath::Max(FMath::CeilLogTwo(View.ViewRect.Height()) - 1, 1u);
	int32 NumMips = FMath::Max(NumMipsX, NumMipsY);
	//int32 NumMips = 1;
	// Must be power of 2
	FIntPoint HZBSize = FIntPoint(1 << NumMipsX, 1 << NumMipsY);


	//CreateResource
	EPixelFormat DecsFormat = FMobileHZB::bUseCompute ? PF_R32_FLOAT : PF_R16F;
	uint32 DecsResourceFlag = FMobileHZB::bUseCompute ? TexCreate_ShaderResource | TexCreate_UAV : TexCreate_ShaderResource | TexCreate_RenderTargetable;
	FPooledRenderTargetDesc MobileHZBFurthestDesc = FPooledRenderTargetDesc::Create2DDesc(
		HZBSize, DecsFormat,
		FClearValueBinding::None, TexCreate_HideInVisualizeTexture,
		DecsResourceFlag, false, NumMips);

	GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, View.HZB, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::Transient);

	//CreateMipmap View
	//auto& MobileHZBFurthest = View.HZB->GetRenderTargetItem();
	//if (FMobileHZB::bUseCompute) {

	//}
	//else {
	//	MobileHZBFurthest.SRVs.Empty(NumMips);
	//	for (uint8 MipLevel = 0; MipLevel < NumMips; MipLevel++){
	//		//RG的FRDGTextureSRVDesc也是封装FRHITextureSRVCreateInfo,直接手动管理
	//		FRHITextureSRVCreateInfo MipSRVDesc(MipLevel, 1ul);
	//		MobileHZBFurthest.SRVs.Emplace(MipSRVDesc, RHICreateShaderResourceView(reinterpret_cast<FTexture2DRHIRef&>(MobileHZBFurthest.ShaderResourceTexture), MipSRVDesc));
	//	}
	//}
}
	


void FMobileHZB::ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel) {

	//Mip为0可能有两种情况,所以需要额外DstMipLevel

	FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(RDGTexutreMip->Desc.Texture->Desc.Extent, 1 << RDGTexutreMip->Desc.MipLevel);
	FIntPoint DstSize = FIntPoint::DivideAndRoundUp(RDGFurthestHZBTexture->Desc.Extent, 1 << CurOutHzbMipLevel);

	FMobileHZBParameters ShaderParameters;
	ShaderParameters.InvSize = FVector2D
	(
		1.f / float(SrcSize.X), 
		1.f / float(SrcSize.X)
	);
	ShaderParameters.InputViewportMaxBound = FVector2D
	(
		float(View.ViewRect.Max.X - 0.5f) / float(SrcSize.X),
		float(View.ViewRect.Max.Y - 0.5f) / float(SrcSize.Y)
	);
	ShaderParameters.DispatchThreadIdToBufferUV = FVector4
	(
		2.0f / float(SrcSize.X), 
		2.0f / float(SrcSize.Y), 
		View.ViewRect.Min.X / float(SrcSize.X), 
		View.ViewRect.Min.Y / float(SrcSize.Y)
	);
	ShaderParameters.HZBInvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
	ShaderParameters.ParentTextureMip = RDGTexutreMip;
	ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();

	if (FMobileHZB::bUseCompute)
	{
		int32 EndDestMip = FMath::Min(CurOutHzbMipLevel + FMobileHZB::kMaxMipBatchSize, static_cast<uint32>(RDGFurthestHZBTexture->Desc.NumMips));

		FMobileHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildCS::FParameters>();
		PassParameters->Shared = ShaderParameters;

		for (int32 MipIndex = CurOutHzbMipLevel; MipIndex < EndDestMip; MipIndex++) {
			PassParameters->FurthestHZBOutput = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RDGFurthestHZBTexture, MipIndex));
		}

		FMobileHZBBuildCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FMobileHZBBuildCS::FDimMipLevelCount>(EndDestMip - CurOutHzbMipLevel);


		TShaderMapRef<FMobileHZBBuildCS> ComputeShader(View.ShaderMap, PermutationVector);

		// TODO(RDG): remove ERDGPassFlags::GenerateMips to use FComputeShaderUtils::AddPass().
		ClearUnusedGraphResources(ComputeShader, PassParameters);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ReduceHZB(mips=[%d;%d]) %dx%d", CurOutHzbMipLevel, EndDestMip - 1, DstSize.X, DstSize.Y),
			PassParameters,
			CurOutHzbMipLevel ? (ERDGPassFlags::Compute | ERDGPassFlags::GenerateMips) : ERDGPassFlags::Compute,
			[PassParameters, ComputeShader, DstSize](FRHICommandList& RHICmdList)
			{
				FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FComputeShaderUtils::GetGroupCount(DstSize, 8));
			}
		);
	}
	else
	{
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
}

void FMobileHZB::MobileBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	//因为RT必须是RG资源，所以让RG管理资源
	FRDGBuilder GraphBuilder(RHICmdList);
	
	const auto& SceneTexture = FMobileHZB::bUseCompute ? SceneContext.SceneDepthZ : SceneContext.GetSceneColor();
	FRDGTextureRef RDGSceneTexutre = GraphBuilder.RegisterExternalTexture(SceneTexture, TEXT("RDGSceneTexture"));
	FRDGTextureRef RDGFurthestHZBTexture = GraphBuilder.RegisterExternalTexture(View.HZB);

	FIntPoint HZBSize = RDGFurthestHZBTexture->Desc.Extent;

	// Reduce first mips Closesy and furtherest are done at same time.
	{	
		FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGSceneTexutre, 0));
		ReduceMips(RDGSceneTexutreMip, RDGFurthestHZBTexture, View, GraphBuilder, 0);
	}


	// Reduce the next mips
	int32 MaxMipBatchSize = bUseCompute ? FMobileHZB::kMaxMipBatchSize : 1;
	for (int32 StartDestMip = MaxMipBatchSize; StartDestMip < RDGFurthestHZBTexture->Desc.NumMips; StartDestMip += MaxMipBatchSize)
	{
		FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGFurthestHZBTexture, StartDestMip - 1));
		ReduceMips(RDGSceneTexutreMip, RDGFurthestHZBTexture, View, GraphBuilder, StartDestMip);
	}

	// Update the view.
	View.HZBMipmap0Size = HZBSize;
	GraphBuilder.Execute();

	//最后手动Transition,因为RG并不知道后续资源用处
	if (FMobileHZB::bUseCompute) {
		RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToGfx, View.HZB->GetRenderTargetItem().ShaderResourceTexture);
	}
	else {
		RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EGfxToGfx, View.HZB->GetRenderTargetItem().ShaderResourceTexture);
	}
	
}


void FMobileSceneRenderer::MobileRenderHZB(FRHICommandListImmediate& RHICmdList) {

	SCOPED_DRAW_EVENT(RHICmdList, MobileHZB);

	FSceneViewState* ViewState = (FSceneViewState*)Views[0].State;

	if (ViewState && ViewState->HZBOcclusionTests.GetNum() != 0) {
		//Hiz generator
		{
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBOcclusionGenerator));
			FMobileHZB::InitialResource(RHICmdList, Views[0]);		
			FMobileHZB::MobileBuildHZB(RHICmdList, Views[0]);
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
