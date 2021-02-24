#include "MobileHZB.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PixelShaderUtils.h"

#define SL_USE_MOBILEHZB 1

static TAutoConsoleVariable<int32> CVarMobileUseRaster(
	TEXT("r.MobileUseRaster"),
	FMobileHzbSystem::bUseRaster,
	TEXT("Test Mali Device Raster"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarMobileBuildHZB(
	TEXT("r.MobileBuildHZB"),
	1,
	TEXT("Test Mali Device BuildHZB"),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarMobileUseSceneDepth(
	TEXT("r.MobileHZBUseSceneDepth"),
	0,
	TEXT("Test UseSceneDepth"),
	ECVF_RenderThreadSafe
);


DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Generator"), STAT_CLMM_HZBOcclusionGenerator, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("HZBOcclusion Submit"), STAT_CLMM_HZBCopyOcclusionSubmit, STATGROUP_CommandListMarkers);

BEGIN_SHADER_PARAMETER_STRUCT(FMobileHZBParameters, )
SHADER_PARAMETER(FVector4, HZBInvDeviceZToWorldZTransform)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ParentTextureMip)
SHADER_PARAMETER_SAMPLER(SamplerState, ParentTextureMipSampler)
END_SHADER_PARAMETER_STRUCT()


class FMobileHZBBuildPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildPS);
	SHADER_USE_PARAMETER_STRUCT(FMobileHZBBuildPS, FGlobalShader)

	class FDimSceneDepth : SHADER_PERMUTATION_BOOL("FDimSceneDepth"); //第一个Pass,从alpha中取LinearDepth
	class FUseSceneDepth : SHADER_PERMUTATION_BOOL("UseSceneDepth"); //测试使用深度图是否剔除更多

	using FPermutationDomain = TShaderPermutationDomain<FDimSceneDepth, FUseSceneDepth>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

};


IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildPS, "/Engine/Private/MobileHZB.usf", "HZBBuildPS", SF_Pixel);
	

void FMobileHzbSystem::ReduceMips(FRDGTextureSRVRef RDGTexutreMip, FRDGTextureRef RDGFurthestHZBTexture, FViewInfo& View, FRDGBuilder& GraphBuilder, uint32 CurOutHzbMipLevel) {

	//Mip为0可能有两种情况,所以需要额外DstMipLevel

	FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(RDGTexutreMip->Desc.Texture->Desc.Extent, 1 << RDGTexutreMip->Desc.MipLevel);
	FIntPoint DstSize = FIntPoint::DivideAndRoundUp(MobileHzbResourcesPtr->HzbSize, 1 << CurOutHzbMipLevel);

	FMobileHZBParameters ShaderParameters;
	ShaderParameters.HZBInvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
	ShaderParameters.ParentTextureMip = RDGTexutreMip;
	ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();


	FMobileHZBBuildPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildPS::FParameters>();
	PassParameters->Shared = ShaderParameters;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(RDGFurthestHZBTexture, ERenderTargetLoadAction::ENoAction, CurOutHzbMipLevel);

	FMobileHZBBuildPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FMobileHZBBuildPS::FDimSceneDepth>(CurOutHzbMipLevel == 0);  //use SceneTexture Only Mipmap is 0 
	PermutationVector.Set<FMobileHZBBuildPS::FUseSceneDepth>(CVarMobileUseSceneDepth.GetValueOnAnyThread() != 0);
	

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
	const auto& SceneTexture = SceneContext.GetSceneColor();

	FRDGTextureRef RDGSceneTexutre = GraphBuilder.RegisterExternalTexture(SceneTexture, TEXT("RDGSceneTexture"));
	FRDGTextureSRVRef RDGSceneTexutreMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGSceneTexutre, 0));

	//RG的FRDGTextureSRVDesc也是封装FRHITextureSRVCreateInfo,也可以直接手动管理
	FRDGTextureRef RDGFurthestHZBTexture = GraphBuilder.RegisterExternalTexture(MobileHzbResourcesPtr->MobileHZBTexture);
	ReduceMips(RDGSceneTexutreMip, RDGFurthestHZBTexture, View, GraphBuilder, 0);

	// Reduce the next mips
	int32 MaxMipBatchSize = 1;
	for (int32 StartDestMip = MaxMipBatchSize; StartDestMip < RDGFurthestHZBTexture->Desc.NumMips; StartDestMip += MaxMipBatchSize) {
		FRDGTextureSRVRef RDGHzbSrvMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(RDGFurthestHZBTexture, StartDestMip - 1));
		ReduceMips(RDGHzbSrvMip, RDGFurthestHZBTexture, View, GraphBuilder, StartDestMip);
	}

	// Update the view.
	View.HZBMipmap0Size = MobileHzbResourcesPtr->HzbSize;
	GraphBuilder.Execute();
	//RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EGfxToGfx, MobileHzbResourcesPtr->MobileHZBTexture->GetRenderTargetItem().ShaderResourceTexture);
}

void FMobileSceneRenderer::MobileSubmitHzb(FRHICommandListImmediate& RHICmdList) {
	//Issuse Hiz Occlusion Query
	FSceneViewState* ViewState = (FSceneViewState*)Views[0].State;

	if (ViewState && ViewState->HZBOcclusionTests.GetNum() != 0) {
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBCopyOcclusionSubmit));
#if SL_USE_MOBILEHZB
		ViewState->HZBOcclusionTests.MobileSubmit(RHICmdList, Views[0]);
		ViewState->HZBOcclusionTests.SetValidFrameNumber(ViewState->OcclusionFrameCounter);
#else
		ViewState->HZBOcclusionTests.Submit(RHICmdList, Views[0]);
		View.HZB.SafeRelease();
#endif
	}
}

//----------------------------------------New System----------------------------------------
TGlobalResource<FMobileHzbResource>* FMobileHzbSystem::MobileHzbResourcesPtr = nullptr;

void FMobileHzbResource::ReleaseDynamicRHI() {
	MobileHZBBuffer_GPU.Release();
	MobileHZBTexture.SafeRelease(); //#TODO: 释放时机晚于SceneRenderTarget, 不能使用RT Pool管理, 直接释放
}

void FMobileHzbResource::InitDynamicRHI() {
	//Dev下两种资源都创建
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	//Create Texture
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		HzbSize = FIntPoint(FMobileHzbSystem::kHzbTexWidth, FMobileHzbSystem::kHzbTexHeight);
		int32 NumMips = FMobileHzbSystem::kHZBMaxMipmap;
		//CreateResource
		FPooledRenderTargetDesc MobileHZBFurthestDesc = FPooledRenderTargetDesc::Create2DDesc(
			HzbSize,
			PF_R16F,
			FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, NumMips);
#if SL_USE_MOBILEHZB
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, MobileHZBTexture, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#else
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, View.HZB, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#endif
	}

	//Create Buffer
	{
		constexpr int32 PerElementSize = sizeof(float);
		constexpr auto BufferElements = FMobileHzbSystem::kHzbTexWidth * FMobileHzbSystem::kHzbTexHeight * 2;
		MobileHZBBuffer_GPU.Initialize(PerElementSize, BufferElements, BUF_Static);
	}

#else
	if (bUseRaster) {
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		HzbSize = FIntPoint(FMobileHzbSystem::kHzbTexWidth, FMobileHzbSystem::kHzbTexHeight);
		int32 NumMips = FMobileHzbSystem::kHZBMaxMipmap;
		//CreateResource
		FPooledRenderTargetDesc MobileHZBFurthestDesc = FPooledRenderTargetDesc::Create2DDesc(
			HzbSize,
			PF_R16F,
			FClearValueBinding::None, TexCreate_HideInVisualizeTexture, TexCreate_ShaderResource | TexCreate_RenderTargetable, false, NumMips);
#if SL_USE_MOBILEHZB
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, MobileHZBTexture, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#else
		GRenderTargetPool.FindFreeElement(RHICmdList, MobileHZBFurthestDesc, View.HZB, TEXT("MobileHZBFurthest"), /*bDoWritableBarrier*/false, ERenderTargetTransience::NonTransient);
#endif
	}
	else {
		constexpr int32 PerElementSize = sizeof(float);
		constexpr auto BufferElements = FMobileHzbSystem::kHzbTexWidth * FMobileHzbSystem::kHzbTexHeight * 2;
		MobileHZBBuffer_GPU.Initialize(PerElementSize, BufferElements, BUF_Static);
	}
#endif

}

void FMobileHzbSystem::InitialResource() {
	static TGlobalResource<FMobileHzbResource> SingleMobileHzbResource;
	MobileHzbResourcesPtr = &SingleMobileHzbResource;
}

class FMobileHZBBuildCSLevel0 : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildCSLevel0);

public:

	FMobileHZBBuildCSLevel0() : FGlobalShader() {}

	FMobileHZBBuildCSLevel0(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer) {
		HZBInvDeviceZToWorldZTransform.Bind(Initializer.ParameterMap, TEXT("HZBInvDeviceZToWorldZTransform"));
		ParentSceneTexture.Bind(Initializer.ParameterMap, TEXT("ParentSceneTexture"));
		ParentSceneTextureSampler.Bind(Initializer.ParameterMap, TEXT("ParentSceneTextureSampler"));
		HzbStructuredBufferUAV.Bind(Initializer.ParameterMap, TEXT("HzbStructuredBufferUAV_Zero"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return true;
	}

	void BindParameters(FRHICommandList& RHICmdList, const FViewInfo& View, FRHITexture* InParentSceneTexture, FRWBufferStructured* HzbStructuredBuffer) {
		SetShaderValue(RHICmdList, RHICmdList.GetBoundComputeShader(), HZBInvDeviceZToWorldZTransform, View.InvDeviceZToWorldZTransform);
		SetTextureParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), ParentSceneTexture, InParentSceneTexture);
		SetSamplerParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), ParentSceneTextureSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, HzbStructuredBuffer->UAV);
	}


	void UnBindParameters(FRHICommandList& RHICmdList) {
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, nullptr);
	}

private:
	LAYOUT_FIELD(FShaderParameter, HZBInvDeviceZToWorldZTransform);
	LAYOUT_FIELD(FShaderResourceParameter, ParentSceneTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ParentSceneTextureSampler);
	LAYOUT_FIELD(FShaderResourceParameter, HzbStructuredBufferUAV);

};

class FMobileHZBBuildCSLevel1 : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildCSLevel1);

public:

	FMobileHZBBuildCSLevel1() : FGlobalShader() {}

	FMobileHZBBuildCSLevel1(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer) {
		HzbStructuredBufferUAV.Bind(Initializer.ParameterMap, TEXT("HzbStructuredBufferUAV_One"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) {
		return true;
	}

	void BindParameters(FRHICommandList& RHICmdList, const FViewInfo& View, FRWBufferStructured* HzbStructuredBuffer) {
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, HzbStructuredBuffer->UAV);
	}

	void UnBindParameters(FRHICommandList& RHICmdList) {
		SetUAVParameter(RHICmdList, RHICmdList.GetBoundComputeShader(), HzbStructuredBufferUAV, nullptr);
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, HzbStructuredBufferUAV);
};

IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCSLevel0, "/Engine/Private/MobileHZB.usf", "HZBBuildCSLevelZero", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildCSLevel1, "/Engine/Private/MobileHZB.usf", "HZBBuildCSLevelOne", SF_Compute);

void FMobileHzbSystem::MobileComputeBuildHZB(FRHICommandListImmediate& RHICmdList, FViewInfo& View) {

	//临时增加代码, 现在EndPass中没有UnbindRT
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	UnbindRenderTargets(RHICmdList);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	//Level0
	{
		const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
		const auto& SceneTexture = SceneContext.GetSceneColor();
		RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EGfxToCompute, SceneTexture->GetRenderTargetItem().ShaderResourceTexture);
		RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, EResourceTransitionPipeline::EComputeToCompute, FMobileHzbSystem::GetStructuredBufferRes()->UAV);
		constexpr auto DispatchX = FMobileHzbSystem::kHzbTexWidth / GroupSizeX;
		constexpr auto DispatchY = FMobileHzbSystem::kHzbTexHeight / GroupSizeY;
		TShaderMapRef<FMobileHZBBuildCSLevel0> HzbGeneratorShader(View.ShaderMap);
		RHICmdList.SetComputeShader(HzbGeneratorShader.GetComputeShader());
		HzbGeneratorShader->BindParameters(RHICmdList, View, SceneTexture->GetRenderTargetItem().ShaderResourceTexture, FMobileHzbSystem::GetStructuredBufferRes());
		RHICmdList.DispatchComputeShader(DispatchX, DispatchY, 1);
		HzbGeneratorShader->UnBindParameters(RHICmdList);
	}


	//Level1
	{
		//#TODO: 因为资源读写是同一个,要保证内存可见性？
		RHICmdList.TransitionResource(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EComputeToCompute, FMobileHzbSystem::GetStructuredBufferRes()->UAV);
		constexpr auto DispatchX = 2;
		constexpr auto DispatchY = 1;
		TShaderMapRef<FMobileHZBBuildCSLevel1> HzbGeneratorShader(View.ShaderMap);
		RHICmdList.SetComputeShader(HzbGeneratorShader.GetComputeShader());
		HzbGeneratorShader->BindParameters(RHICmdList, View, FMobileHzbSystem::GetStructuredBufferRes());
		RHICmdList.DispatchComputeShader(DispatchX, DispatchY, 1);
		HzbGeneratorShader->UnBindParameters(RHICmdList);
	}
	
}

void FMobileSceneRenderer::MobileBuildHzb(FRHICommandListImmediate& RHICmdList) {
	//Hiz generator
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (CVarMobileBuildHZB.GetValueOnAnyThread() == 0) {
		return;
	}
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_HZBOcclusionGenerator));
#endif


	if (
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		CVarMobileUseRaster.GetValueOnAnyThread() != 0
#else
		bUseRaster
#endif
		) 
	{
		FMobileHzbSystem::MobileRasterBuildHZB(RHICmdList, Views[0]);
	}
	else {
		FMobileHzbSystem::MobileComputeBuildHZB(RHICmdList, Views[0]);
	}

}

