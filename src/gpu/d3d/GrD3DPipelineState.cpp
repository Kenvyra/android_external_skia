/*
 * Copyright 2020 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/d3d/GrD3DPipelineState.h"

#include "include/private/SkTemplates.h"
#include "src/gpu/GrProgramInfo.h"
#include "src/gpu/GrStencilSettings.h"
#include "src/gpu/d3d/GrD3DBuffer.h"
#include "src/gpu/d3d/GrD3DGpu.h"
#include "src/gpu/d3d/GrD3DRootSignature.h"
#include "src/gpu/d3d/GrD3DTexture.h"
#include "src/gpu/glsl/GrGLSLFragmentProcessor.h"
#include "src/gpu/glsl/GrGLSLGeometryProcessor.h"
#include "src/gpu/glsl/GrGLSLXferProcessor.h"

GrD3DPipelineState::GrD3DPipelineState(
        gr_cp<ID3D12PipelineState> pipelineState,
        sk_sp<GrD3DRootSignature> rootSignature,
        const GrGLSLBuiltinUniformHandles& builtinUniformHandles,
        const UniformInfoArray& uniforms, uint32_t uniformSize,
        uint32_t numSamplers,
        std::unique_ptr<GrGLSLPrimitiveProcessor> geometryProcessor,
        std::unique_ptr<GrGLSLXferProcessor> xferProcessor,
        std::unique_ptr<std::unique_ptr<GrGLSLFragmentProcessor>[]> fragmentProcessors,
        int fragmentProcessorCnt,
        size_t vertexStride,
        size_t instanceStride)
    : fPipelineState(std::move(pipelineState))
    , fRootSignature(std::move(rootSignature))
    , fBuiltinUniformHandles(builtinUniformHandles)
    , fGeometryProcessor(std::move(geometryProcessor))
    , fXferProcessor(std::move(xferProcessor))
    , fFragmentProcessors(std::move(fragmentProcessors))
    , fFragmentProcessorCnt(fragmentProcessorCnt)
    , fDataManager(uniforms, uniformSize)
    , fNumSamplers(numSamplers)
    , fVertexStride(vertexStride)
    , fInstanceStride(instanceStride) {}

void GrD3DPipelineState::setData(const GrRenderTarget* renderTarget,
                                 const GrProgramInfo& programInfo) {
    this->setRenderTargetState(renderTarget, programInfo.origin());

    GrFragmentProcessor::PipelineCoordTransformRange transformRange(programInfo.pipeline());
    fGeometryProcessor->setData(fDataManager, programInfo.primProc(), transformRange);
    GrFragmentProcessor::CIter fpIter(programInfo.pipeline());
    GrGLSLFragmentProcessor::Iter glslIter(fFragmentProcessors.get(), fFragmentProcessorCnt);
    for (; fpIter && glslIter; ++fpIter, ++glslIter) {
        glslIter->setData(fDataManager, *fpIter);
    }
    SkASSERT(!fpIter && !glslIter);

    {
        SkIPoint offset;
        GrTexture* dstTexture = programInfo.pipeline().peekDstTexture(&offset);

        fXferProcessor->setData(fDataManager, programInfo.pipeline().getXferProcessor(),
                                dstTexture, offset);
    }
}

void GrD3DPipelineState::setRenderTargetState(const GrRenderTarget* rt, GrSurfaceOrigin origin) {
    // Load the RT height uniform if it is needed to y-flip gl_FragCoord.
    if (fBuiltinUniformHandles.fRTHeightUni.isValid() &&
        fRenderTargetState.fRenderTargetSize.fHeight != rt->height()) {
        fDataManager.set1f(fBuiltinUniformHandles.fRTHeightUni, SkIntToScalar(rt->height()));
    }

    // set RT adjustment
    SkISize dimensions = rt->dimensions();
    SkASSERT(fBuiltinUniformHandles.fRTAdjustmentUni.isValid());
    if (fRenderTargetState.fRenderTargetOrigin != origin ||
        fRenderTargetState.fRenderTargetSize != dimensions) {
        fRenderTargetState.fRenderTargetSize = dimensions;
        fRenderTargetState.fRenderTargetOrigin = origin;

        float rtAdjustmentVec[4];
        fRenderTargetState.getRTAdjustmentVec(rtAdjustmentVec);
        fDataManager.set4fv(fBuiltinUniformHandles.fRTAdjustmentUni, 1, rtAdjustmentVec);
    }
}

void GrD3DPipelineState::setAndBindTextures(const GrPrimitiveProcessor& primProc,
                                            const GrSurfaceProxy* const primProcTextures[],
                                            const GrPipeline& pipeline) {
    SkASSERT(primProcTextures || !primProc.numTextureSamplers());

    struct SamplerBindings {
        GrSamplerState fState;
        GrD3DTexture* fTexture;
    };
    SkAutoSTMalloc<8, SamplerBindings> samplerBindings(fNumSamplers);
    int currTextureBinding = 0;

    for (int i = 0; i < primProc.numTextureSamplers(); ++i) {
        SkASSERT(primProcTextures[i]->asTextureProxy());
        const auto& sampler = primProc.textureSampler(i);
        auto texture = static_cast<GrD3DTexture*>(primProcTextures[i]->peekTexture());
        samplerBindings[currTextureBinding++] = { sampler.samplerState(), texture };
    }

    GrFragmentProcessor::CIter fpIter(pipeline);
    GrGLSLFragmentProcessor::Iter glslIter(fFragmentProcessors.get(), fFragmentProcessorCnt);
    for (; fpIter && glslIter; ++fpIter, ++glslIter) {
        for (int i = 0; i < fpIter->numTextureSamplers(); ++i) {
            const auto& sampler = fpIter->textureSampler(i);
            samplerBindings[currTextureBinding++] =
            { sampler.samplerState(), static_cast<GrD3DTexture*>(sampler.peekTexture()) };
        }
    }
    SkASSERT(!fpIter && !glslIter);

    if (GrTexture* dstTexture = pipeline.peekDstTexture()) {
        samplerBindings[currTextureBinding++] = {
                GrSamplerState::Filter::kNearest, static_cast<GrD3DTexture*>(dstTexture) };
    }

    // TODO: bind descriptors
    SkASSERT(fNumSamplers == currTextureBinding);
}

void GrD3DPipelineState::bindBuffers(const GrBuffer* indexBuffer, const GrBuffer* instanceBuffer,
                                     const GrBuffer* vertexBuffer,
                                     GrD3DDirectCommandList* commandList) {
    // Here our vertex and instance inputs need to match the same 0-based bindings they were
    // assigned in the PipelineState. That is, vertex first (if any) followed by instance.
    if (auto* d3dVertexBuffer = static_cast<const GrD3DBuffer*>(vertexBuffer)) {
        SkASSERT(!d3dVertexBuffer->isCpuBuffer());
        SkASSERT(!d3dVertexBuffer->isMapped());
        auto* d3dInstanceBuffer = static_cast<const GrD3DBuffer*>(instanceBuffer);
        if (d3dInstanceBuffer) {
            SkASSERT(!d3dInstanceBuffer->isCpuBuffer());
            SkASSERT(!d3dInstanceBuffer->isMapped());
        }
        commandList->setVertexBuffers(0, d3dVertexBuffer, fVertexStride,
                                      d3dInstanceBuffer, fInstanceStride);
    }
    if (auto* d3dIndexBuffer = static_cast<const GrD3DBuffer*>(indexBuffer)) {
        SkASSERT(!d3dIndexBuffer->isCpuBuffer());
        SkASSERT(!d3dIndexBuffer->isMapped());
        commandList->setIndexBuffer(d3dIndexBuffer);
    }
}
