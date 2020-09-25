#include "MobileHZB.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "RenderGraph.h"
#include "PixelShaderUtils.h"


BEGIN_SHADER_PARAMETER_STRUCT(FMobileSceneTextures, )
//Mobile��SceneDepth�ǲ���д����, ����ʹ��SceneColor
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MobileSceneColorBuffer)

END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FMobileHZBParameters, )
SHADER_PARAMETER(FVector4, DispatchThreadIdToBufferUV)
SHADER_PARAMETER(FVector2D, InputViewportMaxBound)
SHADER_PARAMETER(FVector2D, InvSize)

SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ParentTextureMip)
SHADER_PARAMETER_SAMPLER(SamplerState, ParentTextureMipSampler)

//SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
END_SHADER_PARAMETER_STRUCT()


class FMobileHZBBuildPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMobileHZBBuildPS);
	SHADER_USE_PARAMETER_STRUCT(FMobileHZBBuildPS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FMobileHZBParameters, Shared)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_FLOAT);
	}
};
IMPLEMENT_GLOBAL_SHADER(FMobileHZBBuildPS, "/Engine/Private/MobileHZB.usf", "HZBBuildPS", SF_Pixel);

static constexpr int32 kMaxMipBatchSize = 4;

void SetupMobileHZBParameters(FRDGBuilder& GraphBuilder, FMobileSceneTextures* OutTextures){
	const FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	OutTextures->MobileSceneColorBuffer = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));
}

void MobileBuildHZB(FRDGBuilder& GraphBuilder, const FMobileSceneTextures& SceneTextures, FViewInfo& View) {
	FIntPoint HZBSize;
	int32 NumMips;
	{
		const int32 NumMipsX = FMath::FloorLog2(View.ViewRect.Width());
		const int32 NumMipsY = FMath::FloorLog2(View.ViewRect.Height());

		NumMips = FMath::Max(NumMipsX, NumMipsY);

		// Must be power of 2
		HZBSize = FIntPoint(1 << NumMipsX, 1 << NumMipsY);
	}

	//#TODO: CS? 
	constexpr bool bUseCompute = false;
	int32 MaxMipBatchSize = bUseCompute ? kMaxMipBatchSize : 1;

	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2DDesc(
		HZBSize, PF_R16F,
		FClearValueBinding::None,
		TexCreate_None,
		TexCreate_ShaderResource | (bUseCompute ? TexCreate_UAV : TexCreate_RenderTargetable),
		/* bInForceSeparateTargetAndShaderResource = */ false,
		NumMips);

	FRDGTextureRef FurthestHZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("MobileHZBFurthest"));

	auto ReduceMips = [&](
		FRDGTextureSRVRef ParentTextureMip, int32 StartDestMip, FVector4 DispatchThreadIdToBufferUV, FVector2D InputViewportMaxBound)
	{
		FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(ParentTextureMip->Desc.Texture->Desc.Extent, 1 << int32(ParentTextureMip->Desc.MipLevel));

		FMobileHZBParameters ShaderParameters;
		ShaderParameters.InvSize = FVector2D(1.0f / SrcSize.X, 1.0f / SrcSize.Y);
		ShaderParameters.InputViewportMaxBound = InputViewportMaxBound;
		ShaderParameters.DispatchThreadIdToBufferUV = DispatchThreadIdToBufferUV;
		ShaderParameters.ParentTextureMip = ParentTextureMip;
		ShaderParameters.ParentTextureMipSampler = TStaticSamplerState<SF_Point>::GetRHI();
		//ShaderParameters.View = View.ViewUniformBuffer;

		FIntPoint DstSize = FIntPoint::DivideAndRoundUp(HZBSize, 1 << StartDestMip);

		if (bUseCompute)
		{
			//int32 EndDestMip = FMath::Min(StartDestMip + kMaxMipBatchSize, NumMips);

			//FHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHZBBuildCS::FParameters>();
			//PassParameters->Shared = ShaderParameters;

			//for (int32 DestMip = StartDestMip; DestMip < EndDestMip; DestMip++)
			//{
			//	if (bOutputFurthest)
			//		PassParameters->FurthestHZBOutput[DestMip - StartDestMip] = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FurthestHZBTexture, DestMip));
			//	if (bOutputClosest)
			//		PassParameters->ClosestHZBOutput[DestMip - StartDestMip] = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ClosestHZBTexture, DestMip));
			//}

			//FHZBBuildCS::FPermutationDomain PermutationVector;
			//PermutationVector.Set<FHZBBuildCS::FDimMipLevelCount>(EndDestMip - StartDestMip);
			//PermutationVector.Set<FHZBBuildCS::FDimFurthest>(bOutputFurthest);
			//PermutationVector.Set<FHZBBuildCS::FDimClosest>(bOutputClosest);

			//TShaderMapRef<FHZBBuildCS> ComputeShader(View.ShaderMap, PermutationVector);

			//// TODO(RDG): remove ERDGPassFlags::GenerateMips to use FComputeShaderUtils::AddPass().
			//ClearUnusedGraphResources(ComputeShader, PassParameters);
			//GraphBuilder.AddPass(
			//	RDG_EVENT_NAME("ReduceHZB(mips=[%d;%d]%s%s) %dx%d",
			//		StartDestMip, EndDestMip - 1,
			//		bOutputClosest ? TEXT(" Closest") : TEXT(""),
			//		bOutputFurthest ? TEXT(" Furthest") : TEXT(""),
			//		DstSize.X, DstSize.Y),
			//	PassParameters,
			//	StartDestMip ? (ERDGPassFlags::Compute | ERDGPassFlags::GenerateMips) : ERDGPassFlags::Compute,
			//	[PassParameters, ComputeShader, DstSize](FRHICommandList& RHICmdList)
			//	{
			//		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, FComputeShaderUtils::GetGroupCount(DstSize, 8));
			//	});
		}
		else
		{

			FMobileHZBBuildPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMobileHZBBuildPS::FParameters>();
			PassParameters->Shared = ShaderParameters;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(FurthestHZBTexture, ERenderTargetLoadAction::ENoAction, StartDestMip);

			TShaderMapRef<FMobileHZBBuildPS> PixelShader(View.ShaderMap);

			// TODO(RDG): remove ERDGPassFlags::GenerateMips to use FPixelShaderUtils::AddFullscreenPass().
			ClearUnusedGraphResources(PixelShader, PassParameters);
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("MobileDownsampleHZB(mip=%d) %dx%d", StartDestMip, DstSize.X, DstSize.Y),
				PassParameters,
				StartDestMip ? (ERDGPassFlags::Raster | ERDGPassFlags::GenerateMips) : ERDGPassFlags::Raster,
				[PassParameters, &View, PixelShader, DstSize](FRHICommandList& RHICmdList)
				{
					FPixelShaderUtils::DrawFullscreenPixelShader(RHICmdList, View.ShaderMap, PixelShader, *PassParameters, FIntRect(0, 0, DstSize.X, DstSize.Y));
				});
		}
	};

	// Reduce first mips Closesy and furtherest are done at same time.
	{
		FIntPoint SrcSize = SceneTextures.MobileSceneColorBuffer->Desc.Extent;

		FRDGTextureSRVRef ParentTextureMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneTextures.MobileSceneColorBuffer));

		FVector4 DispatchThreadIdToBufferUV;
		DispatchThreadIdToBufferUV.X = 2.0f / float(SrcSize.X);
		DispatchThreadIdToBufferUV.Y = 2.0f / float(SrcSize.Y);
		DispatchThreadIdToBufferUV.Z = View.ViewRect.Min.X / float(SrcSize.X);
		DispatchThreadIdToBufferUV.W = View.ViewRect.Min.Y / float(SrcSize.Y);

		FVector2D InputViewportMaxBound = FVector2D(
			float(View.ViewRect.Max.X - 0.5f) / float(SrcSize.X),
			float(View.ViewRect.Max.Y - 0.5f) / float(SrcSize.Y));

		ReduceMips(ParentTextureMip,/* StartDestMip = */ 0, DispatchThreadIdToBufferUV, InputViewportMaxBound);
	}

	// Reduce the next mips
	for (int32 StartDestMip = MaxMipBatchSize; StartDestMip < NumMips; StartDestMip += MaxMipBatchSize)
	{
		FIntPoint SrcSize = FIntPoint::DivideAndRoundUp(HZBSize, 1 << int32(StartDestMip - 1));

		FVector4 DispatchThreadIdToBufferUV;
		DispatchThreadIdToBufferUV.X = 2.0f / float(SrcSize.X);
		DispatchThreadIdToBufferUV.Y = 2.0f / float(SrcSize.Y);
		DispatchThreadIdToBufferUV.Z = 0.0f;
		DispatchThreadIdToBufferUV.W = 0.0f;

		FVector2D InputViewportMaxBound(1.0f, 1.0f);

		{
			FRDGTextureSRVRef ParentTextureMip = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(FurthestHZBTexture, StartDestMip - 1));
			ReduceMips(ParentTextureMip,StartDestMip, DispatchThreadIdToBufferUV, InputViewportMaxBound);
		}
	}

	// Update the view.
	View.HZBMipmap0Size = HZBSize;

	GraphBuilder.QueueTextureExtraction(FurthestHZBTexture, &View.HZB);
}


void FMobileSceneRenderer::MobileRenderHZB(FRHICommandListImmediate& RHICmdList) {

	SCOPED_DRAW_EVENT(RHICmdList, MobileHZB);

	FSceneViewState* ViewState = (FSceneViewState*)Views[0].State;

	//�����߳�д��ֵʱ��������,��ʵ�������ж�����ν
	if (DoHZBOcclusion() && ViewState && ViewState->HZBOcclusionTests.IsValidFrame(ViewState->OcclusionFrameCounter)/* && ViewState->HZBOcclusionTests.GetNum() != 0*/) {
		//Hiz generator
		//{
		//	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
		//	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.GetSceneColorSurface());

		//	FMobileSceneTextures HZBParameter;
		//	FRDGBuilder GraphBuilder(RHICmdList);
		//	SetupMobileHZBParameters(GraphBuilder, &HZBParameter);
		//	MobileBuildHZB(GraphBuilder, HZBParameter, Views[0]);
		//	GraphBuilder.Execute();
		//}

		//Issuse Hiz Occlusion Query
		{
			ViewState->HZBOcclusionTests.MobileSubmit(RHICmdList, Views[0]);
		}

		//Flush Command,ȷ����ǰ֡�ı�ִ�е������ǵ���һ֡һ��ִ��
		// Hint to the RHI to submit commands up to this point to the GPU if possible.  Can help avoid CPU stalls next frame waiting
		// for these query results on some platforms.
		{
			RHICmdList.SubmitCommandsHint();
		}
	}




	//	if (bHZBOcclusion && ViewState && ViewState->HZBOcclusionTests.GetNum() != 0)
	//	{
	//		check(ViewState->HZBOcclusionTests.IsValidFrame(ViewState->OcclusionFrameCounter));

	//		SCOPED_DRAW_EVENT(RHICmdList, HZB);
	//		ViewState->HZBOcclusionTests.Submit(RHICmdList, View);
	//	}
	//}

	////async ssao only requires HZB and depth as inputs so get started ASAP
	//if (CanOverlayRayTracingOutput(Views[0]) && GCompositionLighting.CanProcessAsyncSSAO(Views))
	//{
	//	GCompositionLighting.ProcessAsyncSSAO(RHICmdList, Views);
	//}

	//return bHZBOcclusion;
}