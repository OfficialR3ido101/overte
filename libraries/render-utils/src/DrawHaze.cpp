//
//  DrawHaze.cpp
//  libraries/render-utils/src
//
//  Created by Nissim Hadar on 9/1/2017.
//  Copyright 2015 High Fidelity, Inc.
//  Copyright 2024 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DrawHaze.h"

#include <gpu/Context.h>
#include <shaders/Shaders.h>

#include <graphics/ShaderConstants.h>
#include "render-utils/ShaderConstants.h"

#include "StencilMaskPass.h"
#include "FramebufferCache.h"
#include "LightStage.h"

namespace ru {
    using render_utils::slot::texture::Texture;
    using render_utils::slot::buffer::Buffer;
}

namespace gr {
    using graphics::slot::texture::Texture;
    using graphics::slot::buffer::Buffer;
}

gpu::PipelinePointer DrawHaze::_hazePipeline = nullptr;
gpu::PipelinePointer DrawHaze::_separateHazePipeline = nullptr;
gpu::PipelinePointer DrawHaze::_separateHazeBackgroundPipeline = nullptr;

void DrawHaze::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    const auto hazeFrame = inputs.get0();
    const auto& hazeStage = renderContext->args->_scene->getStage<HazeStage>();
    graphics::HazePointer haze;
    if (hazeStage && hazeFrame->_elements.size() > 0) {
        haze = hazeStage->getElement(hazeFrame->_elements.front());
    }

    if (!haze) {
        return;
    }

    const auto outputBuffer = inputs.get1();
    const auto framebuffer = inputs.get2();
    const auto transformBuffer = inputs.get3();
    const auto lightingModel = inputs.get4();
    const auto lightFrame = inputs.get5();

    auto depthBuffer = framebuffer->getLinearDepthTexture();

    if (!lightingModel->isHazeEnabled() || !haze->isActive()) {
        return;
    }

    RenderArgs* args = renderContext->args;

    if (!_hazePipeline) {
        gpu::ShaderPointer program = gpu::Shader::createProgram(shader::render_utils::program::haze);

        auto createState = []() {
            gpu::StatePointer state = std::make_shared<gpu::State>();
            state->setBlendFunction(true,
                                    gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::INV_SRC_ALPHA,
                                    gpu::State::FACTOR_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::ONE);
            return state;
        };

        // Mask out haze on the tablet
        auto hazeState = createState();
        PrepareStencil::testMask(*hazeState);
        _hazePipeline = gpu::PipelinePointer(gpu::Pipeline::create(program, hazeState));

        // For our separated passes, we perform one pass on anything marked shape, and one on just the background
        auto hazeSeparatedState = createState();
        PrepareStencil::testShape(*hazeSeparatedState);
        _separateHazePipeline = gpu::PipelinePointer(gpu::Pipeline::create(program, hazeSeparatedState));

        gpu::ShaderPointer backgroundProgram = gpu::Shader::createProgram(shader::render_utils::program::haze_background);
        auto hazeBackgroundSeparatedState = createState();
        PrepareStencil::testBackground(*hazeBackgroundSeparatedState);
        _separateHazeBackgroundPipeline = gpu::PipelinePointer(gpu::Pipeline::create(backgroundProgram, hazeBackgroundSeparatedState));
    }

    auto outputFramebufferSize = glm::ivec2(outputBuffer->getSize());

    gpu::doInBatch("DrawHaze::run", args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);
        batch.setFramebuffer(outputBuffer);

        batch.setViewportTransform(args->_viewport);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setModelTransform(gpu::Framebuffer::evalSubregionTexcoordTransform(outputFramebufferSize, args->_viewport));

        batch.setUniformBuffer(graphics::slot::buffer::Buffer::HazeParams, haze->getHazeParametersBuffer());

        batch.setUniformBuffer(ru::Buffer::DeferredFrameTransform, transformBuffer->getFrameTransformBuffer());
        batch.setUniformBuffer(ru::Buffer::LightModel, lightingModel->getParametersBuffer());
        batch.setResourceTexture(ru::Texture::HazeLinearDepth, depthBuffer);
        auto lightStage = args->_scene->getStage<LightStage>();
        if (lightStage) {
            graphics::LightPointer keyLight;
            keyLight = lightStage->getCurrentKeyLight(*lightFrame);
            if (keyLight) {
                batch.setUniformBuffer(gr::Buffer::KeyLight, keyLight->getLightSchemaBuffer());
            }
        }

        if (!_separateBackgroundPass) {
            batch.setPipeline(_hazePipeline);
            batch.draw(gpu::TRIANGLE_STRIP, 4);
        } else {
            batch.setPipeline(_separateHazePipeline);
            batch.draw(gpu::TRIANGLE_STRIP, 4);

            batch.setPipeline(_separateHazeBackgroundPipeline);
            batch.draw(gpu::TRIANGLE_STRIP, 4);
        }
    });
}
