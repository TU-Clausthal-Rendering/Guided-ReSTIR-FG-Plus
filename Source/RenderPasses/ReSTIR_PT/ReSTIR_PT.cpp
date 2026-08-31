/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "ReSTIR_PT.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Rendering/Lights/EmissiveUniformSampler.h"
namespace
{
    const std::string kShaderFolder = "RenderPasses/ReSTIR_PT/";
    const std::string kShaderTraceCamera = kShaderFolder + "TraceCamera.rt.slang";
    const std::string kShaderResample = kShaderFolder + "Resample.cs.slang";
    const std::string kShaderEvaluateReservoirs = kShaderFolder + "EvaluateReservoirs.cs.slang";

    const std::string kShaderModel = "6_5";

    // Render Pass inputs and outputs
    const std::string kInputVBuffer = "vbuffer";
    const std::string kInputView = "view";
    const std::string kInputMotionVectors = "mvec";

    const Falcor::ChannelList kInputChannels{
        {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
        {kInputView, "gView", "View Vector from camera perspective"},
        {kInputMotionVectors, "gMotionVectors", "Motion vector buffer (float format)"},
    };

    //Outputs
    const std::string kOutputColor = "color";
    const std::string kOutputDebug = "debug";

    const Falcor::ChannelList kOutputChannels
    {
        {kOutputColor, "gOutColor", "HDR output color", false /*optional*/, ResourceFormat::RGBA32Float},
        {kOutputDebug, "gDebug", "Debug Texture", true, ResourceFormat::RGBA32Float}
    };

    const Gui::DropdownList kDiffuseClassification = {{0 , "RoughnessThreshold"},{1 , "BSDFLobes"}};

}; // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ReSTIR_PT>();
}

ReSTIR_PT::ReSTIR_PT(ref<Device> pDevice, const Properties& props)
    : RenderPass(pDevice)
{
     if (!mpDevice->isShaderModelSupported(Device::ShaderModel::SM6_5))
    {
        throw RuntimeError("ReSTIR_FG: Shader Model 6.5 is not supported by the current device");
    }
    if (!mpDevice->isFeatureSupported(Device::SupportedFeatures::RaytracingTier1_1))
    {
        throw RuntimeError("ReSTIR_FG: Raytracing Tier 1.1 is not supported by the current device");
    }

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
}

Properties ReSTIR_PT::getProperties() const
{
    return {};
}

RenderPassReflection ReSTIR_PT::reflect(const CompileData& compileData)
{
    //In- and Output Textures
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ReSTIR_PT::renderUI(Gui::Widgets& widget)
{
    bool changed = false; 

    //Lamda for Path Length UI Element
    auto pathLengthUI = [&](PathLengthSettings& pathLength) {
        changed |= widget.var("Max Bounces", pathLength.bounces, 0u, 255u, 1u);
        changed |= widget.var("Max Diffuse Bounces", pathLength.diffuse , 0u, 255u, 1u);
        changed |= widget.var("Max Specular Bounces", pathLength.specular, 0u, 255u, 1u);
        changed |= widget.var("Max Delta Bounces", pathLength.delta, 0u, 255u, 1u);
    };

    if (auto group = widget.group("RTXDI"))
    {
        if (mpRTXDI)
        {
            mpRTXDI->renderUI(group);
        }
        else
        {
            group.text("Load a scene for RTXDI options");
        }
    }

    if (auto group = widget.group("ReSTIR_PT"))
    {
        pathLengthUI(mOptions.pathLength);
        group.tooltip(
            "Maximum Path length for a initial path sample. A path sample stops, when it encounters a diffuse surface"
        );

        if (auto group2 = group.group("NEE Options"))
        {
            mRebuildLightSampler |= group.dropdown("NEE Sampler", mEmissiveLightSamplerType);
            if (mEmissiveLightSamplerType == EmissiveLightSamplerType::LightBVH)
            {
                if (auto group3 = group.group("NEE Sampler Options"))
                {
                    mpEmissiveLightSampler->renderUI(group3);
                }
            }
        }
        
        group.checkbox("Enable Resampling", mOptions.enableResampling);
        group.var("Confidence Cap", mOptions.confidenceCap);
        group.var("Spatial Samples", mOptions.numberSpatialSamples);
        group.var("Spatial Sample Radius", mOptions.spatialResamplingRadius);
        
        
        group.separator();
        group.text("Surface Rejection Options:");
        group.var("Normal Rejection Threshold", mOptions.normalAngleThreshold, 0.f, 1.0f, 0.001f);
        group.tooltip("Threshold of dot product between both reservoir face normals");
        group.var("Sample Distance Threshold", mOptions.jacobianDistanceThreshold, 0.f, FLT_MAX, 0.001f);
        group.tooltip("Minimal distance a sample needs to travel to allow reconnection.");
        group.var("Relative depth threshold", mOptions.relativeDepthThreshold, 0.f, 10.0f, 0.001f);
        group.tooltip("Only resamples if relative depth is similar. E.g. 0.15 -> relative depth should be a maximum of 15% different");

        group.checkbox("Shift Reconnection Sample", mOptions.shiftReconnectionSample);
        group.tooltip("If enabled, shift is also performed for the reconnection samples during temporal resampling. On static scenes this can be assumed true and therefore does not need to be performed.");

        mClearReservoir = group.button("Clear Reservoirs");
    }

    if (auto group = widget.group("Material Options"))
    {
        changed |= group.checkbox("Use Lambertian Diffuse BSDF", mOptions.useLambertianDiffuseBSDF);
        group.tooltip("BSDF used by ReSTIR PT and Suffix ReSTIR prototype");

         group.text("Diffuse Surface Classification:");
        group.tooltip("Determines the diffuse surface classification: \n"
        "RoughnessThreshold: All surfaces higher than the threshold are considered diffuse \n"
        "BSDFLobes: Uses BSDF lobes + threshold on specular to determine if a path/surface is diffuse");
        group.indent(10.f);
        changed |= group.dropdown("##DiffuseClassification", kDiffuseClassification, mOptions.diffuseClassificationBSDFLobes);
        group.indent(-10.f);

        group.text("Roughness Threshold:");
        group.tooltip("Used to determine if a reconnection can be performed. Additionally if the classification is choosen, it is also used to determine if a surface is diffuse");
        group.indent(10.f);
        changed |= group.var("##RoughnessThreshold", mOptions.specularRoughnessThreshold, 0.f, 1.f, 0.001f);
        group.indent(-10.f);

        changed |= group.checkbox("Enable Alpha Test", mOptions.enableAlphaTest);
        changed |= group.checkbox("Evaluate Delta PDFs", mOptions.evaluateDeltaPDFs);

    }

    if (auto group = widget.group("Debug"))
    {
        group.checkbox("Clear Debug Texture", mClearDebugTexture);
        changed |= group.checkbox("Disable Direct Light", mOptions.debugDisableDirectLight);
        changed |= group.checkbox("Disable Indirect Light", mOptions.debugDisableIndirectLight);
        changed |= group.checkbox("Disable (Backprojected) Caustics", mOptions.debugDisableCaustics);
    }
}

void ReSTIR_PT::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Reset Scene
    mpScene = pScene;

    //Reset all passes and sampling helpers
    mpEmissiveLightSampler.reset();
    mpRTXDI.reset();
    mResetScreenTex = true;
    resetAllRenderPasses();

    if (mpScene)
    {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        //On scenes without animated (dynamic) geometry, shift of the reconnection sample can be disabled
        if(!mpScene->hasDynamicGeometry())
            mOptions.shiftReconnectionSample = false;
    }
}

void ReSTIR_PT::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Return early if there is no scene
    if (!mpScene)
        return;

    //Add refresh flag if options changed
    auto& dict = renderData.getDictionary();
    auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
    if (mOptionsChanged)
    {
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }

    //Clear Debug Texture
    if (mClearDebugTexture && renderData[kOutputDebug])
        pRenderContext->clearTexture(renderData[kOutputDebug]->asTexture().get());

    //Disables Resampling for the frame to clear reservoirs
    if (mClearReservoir)
    {
        mCanResample = false;
        mClearReservoir = false;
    }

    //Init ReSTIR DI
    const auto& pMotionVectors = renderData[kInputMotionVectors]->asTexture();
    if (!mpRTXDI)
        mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);

    //Prepare needed Falcor light sampling structs and all Resources
    prepareLightingStructure(pRenderContext);
    prepareResources(pRenderContext, renderData);

    //Prepare ReSTIR DI
    mpRTXDI->beginFrame(pRenderContext, mScreenRes);

    //Create Initial Samples for indirect illumination. Also fills the Surface structure for RTXDI
    traceInitialPathsPass(pRenderContext, renderData);

    //ReSTIR DI update (resampling)
    mpRTXDI->update(pRenderContext, pMotionVectors);

    //Resampling for the PT Reservoirs
    const uint resampleIterations = 1 + mOptions.numberSpatialSamples;
    for(uint i=0; i<resampleIterations && mOptions.enableResampling && mCanResample; i++)
    {
        //Ping-Pong swap for spatial resampling. Not used for first iteration (temporal)
        if(i != 0)
            mReservoirIndex++;

        //Update sample generator seed for reservoir offset (shared by both resampling functions)
        mReservoirSelectSGSeed = mSGSeed;
        mSGSeed++;

        //Shift and resample path reservoirs
        shiftPathPass(pRenderContext, renderData, i);
        resampleReservoirsPass(pRenderContext, renderData, i);
    }

    //Finalize Reservoirs
    evaluateReservoirsPass(pRenderContext, renderData);

    //End ReSTIR DI frame
    mpRTXDI->endFrame(pRenderContext);

    mFrameCount++;
    mReservoirIndex++;
    mCanResample = true;
}

void ReSTIR_PT::resetAllRenderPasses()
{
    mTraceInitialPathPass = RayTraceProgramHelper::create();
    for(uint i=0; i<4; i++)
        mTraceShiftPass[i] = RayTraceProgramHelper::create();          

    mpResampleReservoirPass.reset();    
    mpEvaluateReservoirsPass.reset();    
}

void ReSTIR_PT::prepareLightingStructure(RenderContext* pRenderContext)
{
    // Make sure that the emissive light is up to date
    auto& pLights = mpScene->getLightCollection(pRenderContext);
    pLights->prepareSyncCPUData(pRenderContext);

    //Initialize Emissive Light Sampler
    if (mpScene->useEmissiveLights())
    {
        if (!mpEmissiveLightSampler || mRebuildLightSampler)
        {
            resetAllRenderPasses();
            FALCOR_ASSERT(pLights && pLights->getActiveLightCount(pRenderContext) > 0);
            switch (mEmissiveLightSamplerType)
            {
            case Falcor::EmissiveLightSamplerType::Uniform:
                mpEmissiveLightSampler = std::make_unique<EmissiveUniformSampler>(pRenderContext, mpScene);
                break;
            case Falcor::EmissiveLightSamplerType::LightBVH:
                mpEmissiveLightSampler = std::make_unique<LightBVHSampler>(pRenderContext, mpScene, mLightBVHOptions);
                break;
            case Falcor::EmissiveLightSamplerType::Power:
                mpEmissiveLightSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, mpScene);
                break;
            case Falcor::EmissiveLightSamplerType::Null:
            default:
                FALCOR_UNREACHABLE();
                break;
            }

            mRebuildLightSampler = false;
        }
    }
    else //Destroy emissive sampler if it was set and scene does not use emissive lights
    {
        if (mpEmissiveLightSampler)
        {
            if (auto lightBVHSampler = dynamic_cast<LightBVHSampler*>(mpEmissiveLightSampler.get()))
            {
                mLightBVHOptions = lightBVHSampler->getOptions();
            }
            mpEmissiveLightSampler = nullptr;
            resetAllRenderPasses();
        }
    }

    //Update once per frame
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->update(pRenderContext);

    // Initialize Enviroment Map sampler
    // Reset/Rebuild if env map changed
    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::EnvMapChanged))
    {
        mpEnvMapSampler = nullptr;
    }

    //Create sampler
    if (mpScene->useEnvLight())
    {
        if (!mpEnvMapSampler)
        {
            mpEnvMapSampler = std::make_unique<EnvMapSampler>(mpDevice, mpScene->getEnvMap());
        }
    }
    else //Destroy sampler if env map was removed
    {
        if (mpEnvMapSampler)
        {
            mpEnvMapSampler = nullptr;
            resetAllRenderPasses();
        }
    }

    //Update NEE selection probability
    mNeeLightSelectProb =
        float3(mpScene->useEmissiveLights() ? 1.f : 0.f, mpScene->useAnalyticLights() ? 1.f : 0.f, mpScene->useEnvLight() ? 1.f : 0.f);
    mNeeLightSelectProb /= mNeeLightSelectProb.x + mNeeLightSelectProb.y + mNeeLightSelectProb.z;
}

void ReSTIR_PT::prepareResources(RenderContext* pRenderContext, const RenderData& renderData)
{
    const uint reservoirStructSize = 96u; //Size of the reservoir struct (see StructsAndHelpers.slang)
    const uint shiftSurfaceStructSize = 24u; //Size of the shift surface struct (see StructsAndHelpers.slang)

    //Reset buffers when screen resolution or certain options changed
    auto& screenDims = renderData.getDefaultTextureDims();
    if (screenDims.x != mScreenRes.x || screenDims.y != mScreenRes.y)
    {
        mScreenRes = screenDims;
        mResetScreenTex = true;
    }

    //Reservoir and shift data (e.g. ping pong)
    for (uint i = 0; i < 2; i++)
    {
        if(!mpReservoir[i] || mResetScreenTex)
        {
            mCanResample = false;
            mpReservoir[i] = Buffer::createStructured(
                mpDevice, reservoirStructSize, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpReservoir[i]->setName("Reservoir_" + std::to_string(i));
        }

        if(!mpReservoirShiftData[i] || mResetScreenTex)
        {
            mpReservoirShiftData[i] = Texture::create2D(
                mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
            mpReservoirShiftData[i]->setName("ReservoirShiftData_" + std::to_string(i));
        }

        if(!mpReservoirShiftSurface[i] || mResetScreenTex)
        {
            mpReservoirShiftSurface[i] = Buffer::createStructured(mpDevice, shiftSurfaceStructSize, mScreenRes.x * mScreenRes.y, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, nullptr, false);
            mpReservoirShiftSurface[i]->setName("ReservoirShiftSurface_" + std::to_string(i));
        }

        if(!mpShiftedRcThp[i] || mResetScreenTex)
        {
            mpShiftedRcThp[i] = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA32Float, 1u, 1u, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
            mpShiftedRcThp[i]->setName("ShiftedRcThp" + std::to_string(i));
        }
    }

    //Surface info from last frame
    if (!mpVBufferPrev || mResetScreenTex)
    {
        auto pVBuffer = renderData[kInputVBuffer]->asTexture();
        mpVBufferPrev = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, pVBuffer->getFormat(), 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpVBufferPrev->setName("VBufferPrev");
    }

    if (!mpViewPrev || mResetScreenTex)
    {
        auto pView = renderData[kInputView]->asTexture();
        mpViewPrev = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, pView->getFormat(), 1u, 1u, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
        );
        mpViewPrev->setName("ViewPrev");
    }

    mResetScreenTex = false; //Reset finished
}

void ReSTIR_PT::traceInitialPathsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "InitialSamples");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(mpRTXDI->getDefines());
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");

        return defines;
    };

    //Initialize Shader
    if (!mTraceInitialPathPass.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTraceCamera);
        desc.setMaxPayloadSize(16u);    //Size of packed VBuffer Hit
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        if (!mpScene->hasProceduralGeometry())
            desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mTraceInitialPathPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mTraceInitialPathPass.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getTypeConformances()));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }

        DefineList defines = {};
        defines.add(mpScene->getSceneDefines());
        defines.add("IS_SHIFT", "0");
        defines.add("SHIFT_IS_CURRENT", "0");
        defines.add("SHIFT_RECONNECTION_SAMPLE", "0");

        mTraceInitialPathPass.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    //Update defines that can change at runtime
    mTraceInitialPathPass.pProgram->addDefines(getRuntimeDefines());
    if (mpEmissiveLightSampler)
        mTraceInitialPathPass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

    //Program Vars
    if (!mTraceInitialPathPass.pVars)
        mTraceInitialPathPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mTraceInitialPathPass.pVars);

    //Bind Resources to shader
    auto var = mTraceInitialPathPass.pVars->getRootVar();

    //Constant Buffer
    var["CB"]["gSGSeed"] = mSGSeed;
    var["CB"]["gPackedPathLength"] = mOptions.pathLength.pack();
    var["CB"]["gJacobianDistanceThreshold"] = mOptions.jacobianDistanceThreshold;
    var["CB"]["gNeeSelectProbabilites"] = mNeeLightSelectProb;

    //RTXDI Resources
    mpRTXDI->setShaderData(var);

    //NEE Structures 
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["Light"]["gEmissiveSampler"]);
    if (mpEnvMapSampler)
        mpEnvMapSampler->setShaderData(var["Light"]["gEnvMapSampler"]);

    //Input Resources
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gView"] = renderData[kInputView]->asTexture();

    //Output Resources
    var["gPathReservoir"] = mpReservoir[mReservoirIndex % 2];

    //Dispatch Shader
    mpScene->raytrace(pRenderContext, mTraceInitialPathPass.pProgram.get(), mTraceInitialPathPass.pVars, uint3(mScreenRes, 1));

    mSGSeed += 2;   //Two Sample generators are used in this pass
}

void ReSTIR_PT::shiftPathPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass)
{
    FALCOR_PROFILE(pRenderContext, "ShiftPath");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(mpRTXDI->getDefines());
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
        return defines;
    };   

    for(uint i=0; i<2; i++)
    {
        uint passIndex = i;
        //In scenes with animation, the whole pass needs to be shifted for temporal resampling
        if(mOptions.shiftReconnectionSample && numPass == 0)
            passIndex += 2; 
        auto& pass = mTraceShiftPass[passIndex];
        //Initialize Shader
        if (!pass.pProgram)
        {
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderTraceCamera);
            desc.setMaxPayloadSize(16u);    //Size of packed VBuffer Hit
            desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
            desc.setMaxTraceRecursionDepth(1);
            if (!mpScene->hasProceduralGeometry())
                desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

            pass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = pass.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getTypeConformances()));
            sbt->setMiss(0, desc.addMiss("miss"));

            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
            }

            //Defines that are fixed
            DefineList defines = {};
            defines.add(mpScene->getSceneDefines());
            defines.add("IS_SHIFT", "1");
            defines.add("SHIFT_IS_CURRENT", i==0 ? "1" : "0");
            defines.add("SHIFT_RECONNECTION_SAMPLE", passIndex>=2 ? "1" : "0");

            pass.pProgram = RtProgram::create(mpDevice, desc, defines);
        }

        //Update defines that can change at runtime
        pass.pProgram->addDefines(getRuntimeDefines());
        if (mpEmissiveLightSampler)
            pass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

        //Program Vars
        if (!pass.pVars)
            pass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
        FALCOR_ASSERT(pass.pVars);

        //Bind Resources to shader
        auto var = pass.pVars->getRootVar();

        //Constant Buffer
        var["CB"]["gSGSeed"] = mSGSeed;
        var["CB"]["gPackedPathLength"] = mOptions.pathLength.pack();
        var["CB"]["gJacobianDistanceThreshold"] = mOptions.jacobianDistanceThreshold;
        var["CB"]["gNeeSelectProbabilites"] = mNeeLightSelectProb;

        var["ShiftCB"]["gNumResamplingPass"] = numPass;
        var["ShiftCB"]["gSpatialSampleRadius"] = mOptions.spatialResamplingRadius;
        var["ShiftCB"]["gReservoirSGSeed"] = mReservoirSelectSGSeed;

        //NEE Structures 
        if (mpEmissiveLightSampler)
            mpEmissiveLightSampler->setShaderData(var["Light"]["gEmissiveSampler"]);
        if (mpEnvMapSampler)
            mpEnvMapSampler->setShaderData(var["Light"]["gEnvMapSampler"]);

        //Input Resources
        ref<Texture> vbufferTex = i==0 && numPass == 0 ? mpVBufferPrev : renderData[kInputVBuffer]->asTexture();
        ref<Texture> viewTex = i==0 && numPass == 0 ? mpViewPrev : renderData[kInputView]->asTexture();
        uint reservoirIndex = numPass == 0 ? (mReservoirIndex + i) % 2 : (mReservoirIndex + 1) % 2;

        var["gVBuffer"] = vbufferTex;
        var["gView"] = viewTex;
        var["gMVec"] = renderData[kInputMotionVectors]->asTexture();
        var["gPathReservoir"] = mpReservoir[reservoirIndex];

        //Output Resources
        var["gShiftData"] = mpReservoirShiftData[i];
        var["gShiftSurface"] = mpReservoirShiftSurface[i];
        if(passIndex >= 2)
            var["gShiftRcThp"] = mpShiftedRcThp[i];

        //Dispatch Shader
        mpScene->raytrace(pRenderContext, pass.pProgram.get(), pass.pVars, uint3(mScreenRes, 1));

        mSGSeed++;
    }
}

void ReSTIR_PT::resampleReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass)
{
    FALCOR_PROFILE(pRenderContext, "ResamplePathReservoirs");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add("SHIFT_RECONNECTION_SAMPLE", mOptions.shiftReconnectionSample ? "1" : "0");
        return defines;
    };

    //Initialize compute pass
    if (!mpResampleReservoirPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderResample).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getRuntimeDefines());

        mpResampleReservoirPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpResampleReservoirPass);
    mpResampleReservoirPass->getProgram()->addDefines(getRuntimeDefines()); // Runtime defines

    // Set shader variables
    auto var = mpResampleReservoirPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var); // Set scene data
    mpSampleGenerator->setShaderData(var);                 // Sample generator

    // Constant Buffer
    var["CB"]["gSGSeed"] = mSGSeed;
    var["CB"]["gReservoirOffsetSGSeed"] = mReservoirSelectSGSeed;
    var["CB"]["gFrameDim"] = mScreenRes;
    var["CB"]["gConfidenceCap"] = mOptions.confidenceCap;
    var["CB"]["gSpatialRadius"] = mOptions.spatialResamplingRadius;
    var["CB"]["gNormalThreshold"] = mOptions.normalAngleThreshold;
    var["CB"]["gJacobianDistanceThreshold"] = mOptions.jacobianDistanceThreshold;
    var["CB"]["gRelativeDepthThreshold"] = mOptions.relativeDepthThreshold;
    var["CB"]["gNumResamplingPass"] = numPass;

    // Input Resources
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gVBufferPrev"] = mpVBufferPrev;
    var["gView"] = renderData[kInputView]->asTexture();
    var["gViewPrev"] = mpViewPrev;
    var["gMVec"] = renderData[kInputMotionVectors]->asTexture();
    var["gPathReservoirOther"] = mpReservoir[(mReservoirIndex + 1) % 2];
    var["gShiftData"] = mpReservoirShiftData[0];
    var["gShiftDataOther"] = mpReservoirShiftData[1];
    var["gShiftSurface"] = mpReservoirShiftSurface[0];
    var["gShiftSurfaceOther"] = mpReservoirShiftSurface[1];
    var["gShiftRcThp"] = mpShiftedRcThp[0];
    var["gShiftRcThpOther"] = mpShiftedRcThp[1];

    // In-/Output Resources
    var["gPathReservoir"] = mpReservoir[mReservoirIndex % 2];

    // Execute Compute Pass
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpResampleReservoirPass->execute(pRenderContext, uint3(targetDim, 1));

    mSGSeed++;
}

void ReSTIR_PT::evaluateReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "EvaluateReservoirs");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add(mpRTXDI->getDefines());
        defines.add("DISABLE_DIRECT", mOptions.debugDisableDirectLight ? "1" : "0");
        defines.add("DISABLE_INDIRECT", mOptions.debugDisableIndirectLight ? "1" : "0");
        return defines;
    };

    // Create compute pass
    if (!mpEvaluateReservoirsPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderEvaluateReservoirs).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getRuntimeDefines());

        mpEvaluateReservoirsPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpEvaluateReservoirsPass);
    //Runtime Defines
    mpEvaluateReservoirsPass->getProgram()->addDefines(getRuntimeDefines());

    // Set variables
    auto var = mpEvaluateReservoirsPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var); // Set scene data
    mpSampleGenerator->setShaderData(var);                    // Sample generator
    
    //Constant Buffer
    var["CB"]["gSGSeed"] = mSGSeed;
    var["CB"]["gFrameDim"] = mScreenRes;

    //RTXDI resources
    mpRTXDI->setShaderData(var);

    //Input
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gView"] = renderData[kInputView]->asTexture();
    var["gPathReservoir"] = mpReservoir[mReservoirIndex % 2];
   
    //Output
    var["gVBufferPrev"] = mpVBufferPrev;
    var["gViewPrev"] = mpViewPrev;
    var["gOutColor"] = renderData[kOutputColor]->asTexture();

    // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpEvaluateReservoirsPass->execute(pRenderContext, uint3(targetDim, 1));

    mSGSeed++;
}

DefineList ReSTIR_PT::getMaterialDefines()
{
    DefineList defines;
    defines.add("DiffuseBrdf", mOptions.useLambertianDiffuseBSDF ? "DiffuseBrdfLambert" : "DiffuseBrdfFrostbite");
    defines.add("enableDiffuse", "1");
    defines.add("enableSpecular", "1");
    defines.add("enableTranslucency", "1");
    defines.add("ROUGHNESS_THRESHOLD", std::to_string(mOptions.specularRoughnessThreshold));
    defines.add("ENABLE_ALPHA_TEST" , mOptions.enableAlphaTest ? "1" : "0");
    defines.add("EVAL_DELTA_PDFS", mOptions.evaluateDeltaPDFs ? "1" : "0");
    defines.add("DIFF_CLASS_USE_LOBES", std::to_string(mOptions.diffuseClassificationBSDFLobes));
    return defines;
}

void ReSTIR_PT::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
{
    FALCOR_ASSERT(pProgram);

    // Configure program.
    pProgram->addDefines(pSampleGenerator->getDefines());
    pProgram->setTypeConformances(pScene->getTypeConformances());
    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    pVars = RtProgramVars::create(pDevice, pProgram, pBindingTable);

    // Bind utility classes into shared data.
    auto var = pVars->getRootVar();
    pSampleGenerator->setShaderData(var);
}
