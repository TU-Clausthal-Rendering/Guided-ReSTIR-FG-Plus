#include "ReSTIR_FG_Plus.h"
#include <memory>
#include <utility>
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"
#include "Utils/Math/FalcorMath.h"

#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Rendering/Lights/EmissiveUniformSampler.h"

namespace
{
    const std::string kShaderFolder = "RenderPasses/ReSTIR_FG_Plus/";
    const std::string kShaderTracePhotons = kShaderFolder + "TracePhotons.rt.slang";
    const std::string kShaderTraceCamera = kShaderFolder + "TraceCamera.rt.slang";
    const std::string kShaderBackprojectCaustics = kShaderFolder + "BackprojectCaustics.cs.slang";
    const std::string kShaderResamplingPathReservoir = kShaderFolder + "ResamplePathReservoir.cs.slang";
    const std::string kShaderResamplingReservoirCaustic = kShaderFolder + "ResampleReservoirCaustic.cs.slang";
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
    registry.registerClass<RenderPass, ReSTIR_FG_Plus>();
}

ReSTIR_FG_Plus::ReSTIR_FG_Plus(ref<Device> pDevice, const Properties& props)
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

Properties ReSTIR_FG_Plus::getProperties() const
{
    return {}; //TODO
}

RenderPassReflection ReSTIR_FG_Plus::reflect(const CompileData& compileData)
{
    //In- and Output Textures
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ReSTIR_FG_Plus::renderUI(Gui::Widgets& widget) {
    bool changed = false; 

    //Lamda for Path Length UI Element
    auto pathLengthUI = [&](PathLengthSettings& pathLength) {
        changed |= widget.var("Max Bounces", pathLength.bounces, 0u, 255u, 1u);
        changed |= widget.var("Max Diffuse Bounces", pathLength.diffuse , 0u, 255u, 1u);
        changed |= widget.var("Max Specular Bounces", pathLength.specular, 0u, 255u, 1u);
        changed |= widget.var("Max Delta Bounces", pathLength.delta, 0u, 255u, 1u);
    };

    if (auto group = widget.group("Photon Options"))
    {
        //DispatchedPhotons
        changed |= group.var("Dispatched Photons", mOptions.photonsDispatched, 1024u, 67108864u, 1u); //Max is 8192^2
        group.text("Global Photons: " + std::to_string(mPhotonCountUI[0]) + " / " + std::to_string(mOptions.photonBufferSizeGlobal));
        group.text("Caustic photons: " + std::to_string(mPhotonCountUI[1]) + " / " + std::to_string( mOptions.photonBufferSizeCaustic));
        //Photon Buffer Size
        static uint2 photonBufferSizeUI = uint2(mOptions.photonBufferSizeGlobal, mOptions.photonBufferSizeCaustic); 
        group.text("Photon Buffer Size:");
        group.indent(10.f);
        group.var(" ##MaxPhotonUI", photonBufferSizeUI, 100u, 100000000u, 100);
        group.tooltip("First -> Global, Second -> Caustic");
        if(group.button("Apply", true)){
            mPhotonBufferSizeChanged = true;
            mOptions.photonBufferSizeGlobal = photonBufferSizeUI.x;
            mOptions.photonBufferSizeCaustic = photonBufferSizeUI.y;
        }
        group.indent(-10.f);

        group.var("Acceleration Structure Build Overestimate", mOptions.photonASBuildBufferOverestimate, 1.f, FLT_MAX, 0.001f);
        group.tooltip("Percentage the CPU photon count value (which is delayed by 1-3 frames) is overestimated to improve acceleration structure build time.");

        pathLengthUI(mOptions.photonPathLenght);

        //Radius Setting
        changed |= group.checkbox("Use Adaptive Photon Radius", mOptions.photonUseAdaptiveRadius);
        group.tooltip("Enables adaptive photon radius, that is equal to the projected camera pixel size, which depends on the distance from the camera to the hit point");
        if(mOptions.photonUseAdaptiveRadius)
        {
            changed |= group.var("Adaptive Scale (Global/Caustic)", mOptions.photonAdaptiveRadius, 0.f, FLT_MAX, 0.0001f);
        }
        else
        {
            changed |= group.var("Radius (Global/Caustic)", mOptions.photonRadius, 0.f, FLT_MAX, 0.000001f, false, "%.6f");
        }

        changed |= group.var("Global Photon Rejection Probability", mOptions.photonGlobalRejection, 0.f, 1.f, 0.0001f);
        group.tooltip("Fixed probability, that a global photon is rejected");
        changed |= group.var("Mixed Light Analytic Probability", mOptions.photonMixedLightRatio, 0.f, 1.f, 0.0001f);
        group.tooltip("Probability, that a photon is generated from an analytic/emissive light 0 -> 0% Analytic, 100% Emissive");
    }

    if (auto group = widget.group("PhotonGuiding"))
    {
        group.checkbox("Enable", mOptions.usePhotonGuiding);
        if (mpPhotonGuiding && mOptions.usePhotonGuiding)
        {
            mpPhotonGuiding->renderUI(group);
        }
    }

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

    if (auto group = widget.group("ReSTIR-FG+"))
    {
        group.var("Max Path Length", mOptions.cameraMaxPathLength, 1u, 64u, 1u);
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

        if (auto group2 = group.group("Path Resampling Options"))
        {
            group2.checkbox("Enable Resampling", mOptions.enablePathResampling);
            group2.var("Confidence Cap", mOptions.pathConfidenceCap);
            group2.var("Spatial Samples", mOptions.pathNumberSpatialSamples);
            group2.var("Spatial Sample Radius", mOptions.pathSpatialResamplingRadius);
        }
        
        if (auto group2 = group.group("Resampling Caustic options"))
        {
            group2.checkbox("Enable Resampling", mOptions.enableCausticResampling);
            group2.var("Confidence Cap", mOptions.causticConfidenceCap);
        }

        group.separator();
        group.text("Surface Rejection Options:");
        group.var("Normal Rejection Threshold", mOptions.normalAngleThreshold, 0.f, 1.0f, 0.001f);
        group.tooltip("Threshold of dot product between both reservoir face normals");
        group.var("Sample Distance Threshold", mOptions.jacobianDistanceThreshold, 0.f, FLT_MAX, 0.001f);
        group.tooltip("Minimal distance a sample needs to travel to allow reconnection.");
        group.var("Relative depth threshold", mOptions.relativeDepthThreshold, 0.f, 10.0f, 0.001f);
        group.tooltip("Only resamples if relative depth is similar. E.g. 0.15 -> relative depth should be a maximum of 15% different");

        group.checkbox("Photon Path Shift", mOptions.shiftPhotonPaths);
        group.tooltip("If enabled photon shift is performed. On static scenes, photon shift can always be assumed true and therefore does not need to be performed.");

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

void ReSTIR_FG_Plus::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) {
    // Reset Scene
    mpScene = pScene;

    //Reset all passes and sampling helpers
    mpPhotonAS.reset();
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

        //On scenes without animated (dynamic) geometry, photon shifting can be disabled
        if(!mpScene->hasDynamicGeometry())
            mOptions.shiftPhotonPaths = false;
    }
}

void ReSTIR_FG_Plus::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
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
    ref<Texture> pDebugTex = nullptr;
    if(renderData[kOutputDebug])
        pDebugTex = renderData[kOutputDebug]->asTexture();
    if (mClearDebugTexture && pDebugTex)
        pRenderContext->clearTexture(pDebugTex.get());
    
    //Disables Resampling for the frame to clear reservoirs
    if (mClearReservoir)
    {
        mCanResample = false;
        mClearReservoir = false;
    }

    //Update RNG constants
    mRNGNumPasses = 8 + (3 * mOptions.pathNumberSpatialSamples);

    //Init ReSTIR DI
    const auto& pMotionVectors = renderData[kInputMotionVectors]->asTexture();
    if (!mpRTXDI)
        mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);

    //Init Photon Guiding
    if (mOptions.usePhotonGuiding && !mpPhotonGuiding)
    {
        mpPhotonGuiding = std::make_unique<PhotonGuiding>(mpDevice, mpScene, pRenderContext);
        mCanResample = false;
    }else if (!mOptions.usePhotonGuiding && mpPhotonGuiding)
    {
        mpPhotonGuiding.reset();
    }

    //Prepare needed Falcor light sampling structs and all Resources
    prepareLightingStructure(pRenderContext);
    if (!mHasLights)
    {
        logWarningOnce("Scene has no lights, pass will not execute!");
        return;
    }
    prepareResources(pRenderContext, renderData);

    //Prepare ReSTIR DI
    mpRTXDI->beginFrame(pRenderContext, mScreenRes);

    //Update Guiding Textures
    if (mOptions.usePhotonGuiding)
        mpPhotonGuiding->update(pRenderContext, mOptions.photonsDispatched);

    //Trace Photons and builds the photon acceleration structure. Also backprojects caustic photons into the camera
    tracePhotonsPass(pRenderContext, renderData);

    //Caustic Backprojection
    backprojectCausticsPass(pRenderContext, renderData);

    //Creates initial Reservoirs samples for Path and Caustic Reservoirs. Also fills the Surface structure for RTXDI
    traceCameraPass(pRenderContext, renderData);

    //ReSTIR DI update
    mpRTXDI->update(pRenderContext, pMotionVectors);

    //Resampling with Caustic Reservoirs
    if(mCanResample && mOptions.enableCausticResampling)
    {
        if(mOptions.shiftPhotonPaths)
            shiftCausticPathPass(pRenderContext, renderData);
        else
            backprojectTemporalCausticReservoirsPass(pRenderContext, renderData);

        resampleReservoirCausticPass(pRenderContext, renderData);
    }

    //Resampling with Path Reservoirs
    const uint resampleIterations = 1 + mOptions.pathNumberSpatialSamples;
    for(uint i=0; i<resampleIterations && mOptions.enablePathResampling && mCanResample; i++)
    {
        //Ping-Pong swap for spatial resampling. Not used for first iteration (temporal)
        if(i != 0)
            mReservoirIndex++;

        //Shift the photon passes from last frame
        if(i == 0 && mOptions.shiftPhotonPaths)
            shiftPhotonPathPass(pRenderContext, renderData);
        
        //Shift and resample path reservoirs
        shiftCameraPathPass(pRenderContext, renderData, i);
        resampleReservoirsPass(pRenderContext, renderData, i);
    }

    //Finalize Reservoirs
    evaluateReservoirsPass(pRenderContext, renderData);

    //End ReSTIR DI frame
    mpRTXDI->endFrame(pRenderContext);

    //Copy Camera data for splatting
    const CameraData& camData = mpScene->getCamera()->getData();
    mTemporalCameraViewProjection = camData.viewProjMatNoJitter;
    mTemporalCameraPosition = camData.posW;
    mTemporalCameraForward = math::normalize(camData.cameraW);

    //Visualize Photon Guiding
    if(mOptions.usePhotonGuiding)
    {
        if(pDebugTex)
            mpPhotonGuiding->renderDebugView(pRenderContext, pDebugTex);
    }

    mFrameCount++;
    mReservoirIndex++;
    mCanResample = true;
}

void ReSTIR_FG_Plus::resetAllRenderPasses()
{
    mTracePhotonPass = RayTraceProgramHelper::create();
    mTraceCameraPass = RayTraceProgramHelper::create();
    mShiftCameraPathPass[0] = RayTraceProgramHelper::create();
    mShiftCameraPathPass[1] = RayTraceProgramHelper::create();
    mpBackprojectCausticSamplesPass.reset();
    mpResampleReservoirPass.reset();
    mpResampleReservoirCausticPass.reset();
    mpEvaluateReservoirsPass.reset();
}

void ReSTIR_FG_Plus::prepareLightingStructure(RenderContext* pRenderContext)
{
    // Make sure that the emissive light is up to date
    auto& pLights = mpScene->getLightCollection(pRenderContext);
    pLights->prepareSyncCPUData(pRenderContext);

    bool emissiveUsed = mpScene->useEmissiveLights();
    bool analyticUsed = mpScene->useAnalyticLights();

    mHasLights = analyticUsed || emissiveUsed;

    //Initialize Emissive Light Sampler
    if (emissiveUsed)
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

void ReSTIR_FG_Plus::prepareResources(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Reset buffers when screen resolution or certain options changed
    auto& screenDims = renderData.getDefaultTextureDims();
    if (screenDims.x != mScreenRes.x || screenDims.y != mScreenRes.y)
    {
        mScreenRes = screenDims;
        mResetScreenTex = true;
    }

    //Reset photon guiding extra buffers
    if(!mOptions.usePhotonGuiding && mpPhotonGuidingData[0])
    {
        mpPhotonGuidingData[0].reset();
        mpPhotonGuidingData[1].reset();
        mpPhotonGuidingCausticReservoir[0].reset();
        mpPhotonGuidingCausticReservoir[1].reset();
    }

    if (mPhotonBufferSizeChanged)
    {
        mpPhotonAABB[0].reset();
        mpPhotonAABB[1].reset();
        mpPhotonData[0].reset();
        mpPhotonData[1].reset();
        mpPhotonGuidingData[0].reset();
        mpPhotonGuidingData[1].reset();
        mpLightTraceLinkedList.reset();
        mpPhotonAS.reset();
        mPhotonBufferSizeChanged = false;
    }

    //Buffers that exist two times
    for (uint i = 0; i < 2; i++)
    {
        uint photonBufferSize = i == 0 ? mOptions.photonBufferSizeGlobal : mOptions.photonBufferSizeCaustic;
        if (!mpPhotonAABB[i])
        {
            mpPhotonAABB[i] = Buffer::createStructured(
                mpDevice, sizeof(AABB), photonBufferSize , ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpPhotonAABB[i]->setName("PhotonAABB" + std::to_string(i));
        }
        if (!mpPhotonData[i])
        {
            mpPhotonData[i] = Buffer::createStructured(
                mpDevice, sizeof(float) * 12, photonBufferSize, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpPhotonData[i]->setName("PhotonData" + std::to_string(i));
        }

        if(!mpPhotonGuidingData[i] && mOptions.usePhotonGuiding)
        {
            mpPhotonGuidingData[i] = Buffer::createStructured(
                mpDevice, sizeof(uint) * 2, photonBufferSize, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpPhotonGuidingData[i]->setName("PhotonGuidingData" + std::to_string(i));
        }

        if (!mpCausticReservoir[i] || mResetScreenTex)
        {
            mCanResample = false;
            mpCausticReservoir[i] = Buffer::createStructured(
                mpDevice, 64u, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpCausticReservoir[i]->setName("CausticReservoir" + std::to_string(i));
        }
        if ((!mpPhotonGuidingCausticReservoir[i] || mResetScreenTex) && mOptions.usePhotonGuiding)
        {
            mpPhotonGuidingCausticReservoir[i] = Texture::create2D(mpDevice, mScreenRes.x, mScreenRes.y,
                ResourceFormat::RG32Uint, 1u,1u, nullptr, ResourceBindFlags::UnorderedAccess|ResourceBindFlags::ShaderResource);
            mpPhotonGuidingCausticReservoir[i]->setName("CausticReservoirGuidingData" + std::to_string(i));
        }

        if (!mpPathReservoir[i] || mResetScreenTex)
        {
            mCanResample = false;
            mpPathReservoir[i] = Buffer::createStructured(
                mpDevice, 96u, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                Buffer::CpuAccess::None, nullptr, false
            );
            mpPathReservoir[i]->setName("PathReservoir_" + std::to_string(i));
        }
        if(!mpReservoirShiftData[i] || mResetScreenTex)
        {
            mpReservoirShiftData[i] = Texture::create2D(
                mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
            mpReservoirShiftData[i]->setName("ReservoirShiftData" + std::to_string(i));
        }
    }

    //Photon Counters
    if (!mpPhotonCounter)
    {
        mpPhotonCounter = Buffer::createStructured(
            mpDevice, sizeof(uint), 2, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None,
            nullptr, false
        );
        mpPhotonCounter->setName("PhotonCounter");

        mpPhotonCounterCPU = Buffer::createStructured(
            mpDevice, sizeof(uint), 2, ResourceBindFlags::None, Buffer::CpuAccess::Read, nullptr, false
        );
        mpPhotonCounterCPU->setName("PhotonCounterCPU");
    }

    //Light Trace resources
    if (!mpLightTraceHeadCounter || mResetScreenTex)
    {
        mpLightTraceHeadCounter = Texture::create2D(
            mpDevice, mScreenRes.x, mScreenRes.y, ResourceFormat::R32Int, 1u, 1u, nullptr,
            ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
        );
        mpLightTraceHeadCounter->setName("LightTraceHeadCounter");
        pRenderContext->clearUAV(mpLightTraceHeadCounter->getUAV(0).get(), uint4(uint(-1)));
    }

    if (!mpLightTraceLinkedList || mResetScreenTex)
    {
        //Linked list is used for photons and screen backprojection
        uint linkedListSize = std::max(mOptions.photonBufferSizeCaustic, mScreenRes.x * mScreenRes.y);
        mpLightTraceLinkedList = Buffer::createStructured(
            mpDevice, sizeof(uint), linkedListSize, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            Buffer::CpuAccess::None, nullptr, false
        );
        mpLightTraceLinkedList->setName("LightTraceLinkedList");
        pRenderContext->clearUAV(mpLightTraceLinkedList->getUAV(0).get(), uint4(uint(-1)));
    }

    if(!mpPhotonHitInfo)
    {
        mpPhotonHitInfo = Buffer::createStructured(
            mpDevice, sizeof(uint) * 4, mOptions.photonBufferSizeCaustic, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            Buffer::CpuAccess::None, nullptr, false
        );
        mpPhotonHitInfo->setName("PhotonHitInfo");
    }

    //Copy of surface from last frame
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

    // Create the Photon Acceleration Structure
    if (!mpPhotonAS)
    {
        std::vector<uint64_t> aabbCount = {mOptions.photonBufferSizeGlobal, mOptions.photonBufferSizeCaustic};
        std::vector<uint64_t> aabbGPUAddress = {mpPhotonAABB[0]->getGpuAddress(), mpPhotonAABB[1]->getGpuAddress()};
        mpPhotonAS = std::make_unique<CustomAccelerationStructure>(
            mpDevice, aabbCount, aabbGPUAddress, CustomAccelerationStructure::BuildMode::FastBuild,
            CustomAccelerationStructure::UpdateMode::None
        );
    }

    mResetScreenTex = false; //Reset finished
}

void ReSTIR_FG_Plus::tracePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "TracePhotons");

    //Clear Photon Counter
    pRenderContext->clearUAV(mpPhotonCounter->getUAV().get(), uint4(0));

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mOptions.photonBufferSizeGlobal));
        defines.add("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mOptions.photonBufferSizeCaustic));
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights()? "1" : "0");
        defines.add("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights()? "1" : "0");
        defines.add("USE_ADAPTIVE_PHOTON_RADIUS", mOptions.photonUseAdaptiveRadius ? "1" : "0");
        defines.add(getMaterialDefines());
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        if(mOptions.usePhotonGuiding)
            defines.add(mpPhotonGuiding->getDefines());

        return defines;
    };

    // Init Shader
    if (!mTracePhotonPass.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTracePhotons);
        desc.setMaxPayloadSize(sizeof(float) * 4);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        if (!mpScene->hasProceduralGeometry())
            desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mTracePhotonPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mTracePhotonPass.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getTypeConformances()));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("IS_SHIFT", "0");
        defines.add("IS_SHIFT_PATH", "0");
        defines.add("IS_SHIFT_CAUSTIC", "0");
        defines.add(getRuntimeDefines());

        mTracePhotonPass.pProgram = RtProgram::create(mpDevice, desc, defines);
    }
    //Update Defines
    mTracePhotonPass.pProgram->addDefines(getRuntimeDefines());
    
    // Program Vars
    if (!mTracePhotonPass.pVars)
        mTracePhotonPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mTracePhotonPass.pVars);
    auto var = mTracePhotonPass.pVars->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);

    if(mOptions.usePhotonGuiding)
        mpPhotonGuiding->setShaderData(var);

    //Shader dispatch dims
    //TODO optimize for non-guiding case, so that similar lights are traced in the same workgroup
    if(mOptions.usePhotonGuiding)
    {
        mPhotonDispatchDim = mpPhotonGuiding->getPhotonDispatchSize(mOptions.photonsDispatched);
    }else
    {
        uint photonDispatchSide = static_cast<uint>(std::floor(sqrt(mOptions.photonsDispatched)));
        photonDispatchSide = std::max(32u, photonDispatchSide);
        mPhotonDispatchDim = uint2(photonDispatchSide);
    }

    //Approximated pixel diagonal at length 1 for adaptive photon radius
    if (mOptions.photonUseAdaptiveRadius)
    {
        // Update Image plane distance
        auto& cameraData = mpScene->getCamera()->getData();
        // Get normalized pixel area
        float h = cameraData.frameHeight / cameraData.focalLength; //Normalized Frame height
        float w = h * cameraData.aspectRatio;
        float wPix = w / mScreenRes.x;
        float hPix = h / mScreenRes.y;

        mApproximatePixelDiagonal = sqrt((wPix * wPix) + (hPix * hPix));
    }

    //Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPhotonRadius"] = mOptions.photonUseAdaptiveRadius ? mOptions.photonAdaptiveRadius : mOptions.photonRadius;
    var["CB"]["gPackedPathLength"] = mOptions.photonPathLenght.pack();
    var["CB"]["gGlobalRejectionProb"] = mOptions.photonGlobalRejection;
    var["CB"]["gDispatchDimension"] = mPhotonDispatchDim;
    var["CB"]["gScreenDimensions"] = mScreenRes;
    var["CB"]["gMixedLightsAnalyticProbability"] = mOptions.photonMixedLightRatio;
    var["CB"]["gNormalizedPixelDiagonal"] = mApproximatePixelDiagonal;

    //Output Buffers
    for (uint32_t i = 0; i < 2; i++)
    {
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPhotonData"][i] = mpPhotonData[i];
        if(mOptions.usePhotonGuiding)
            var["gPhotonGuidingData"][i] = mpPhotonGuidingData[i];
    }
    var["gPhotonCounter"] = mpPhotonCounter;

    //Backprojection
    var["gLightTraceHeadCounter"] = mpLightTraceHeadCounter;
    var["gLightTraceLinkedList"] = mpLightTraceLinkedList;
    var["gPhotonHitInfo"] = mpPhotonHitInfo;

    //Dispatch raytracing shader
    mpScene->raytrace(pRenderContext, mTracePhotonPass.pProgram.get(), mTracePhotonPass.pVars, uint3(mPhotonDispatchDim, 1));

    pRenderContext->uavBarrier(mpLightTraceHeadCounter.get());

    //
    //Build the AS for that frame
    //

    //Clear values after the counter
    std::vector<ref<Buffer>> aabbs = {mpPhotonAABB[0], mpPhotonAABB[1]};
    mpPhotonAS->clearAABBBuffers(pRenderContext, aabbs, true, mpPhotonCounter); //Clears unused slots 

    // Copy the PhotonCounter to a CPU Buffer (asynchronous, read GPU value can be a couple of frames old)
    pRenderContext->copyBufferRegion(mpPhotonCounterCPU.get(), 0, mpPhotonCounter.get(), 0, sizeof(uint2));
    void* data = mpPhotonCounterCPU->map(Buffer::MapType::Read);
    std::memcpy(&mPhotonCountUI, data, sizeof(uint2));
    mpPhotonCounterCPU->unmap();
        
    //Build acceleration structure
    uint2 currentPhotons = mFrameCount > 0 ?
        uint2(float2(mPhotonCountUI) * mOptions.photonASBuildBufferOverestimate) :
        uint2(mOptions.photonBufferSizeGlobal, mOptions.photonBufferSizeCaustic);
    std::vector<uint64_t> photonBuildSize = {
        std::min(mOptions.photonBufferSizeGlobal, currentPhotons[0]), std::min(mOptions.photonBufferSizeCaustic, currentPhotons[1])
    };
    mpPhotonAS->update(pRenderContext, photonBuildSize);
}

void ReSTIR_FG_Plus::traceCameraPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "InitialSamples");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(mpRTXDI->getDefines());
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        return defines;
    };

    //Initialize Shader
    if (!mTraceCameraPass.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTraceCamera);
        desc.setMaxPayloadSize(16u); //Size of packed VBuffer Hit
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        if (!mpScene->hasProceduralGeometry())
            desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mTraceCameraPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mTraceCameraPass.pBindingTable;
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

        mTraceCameraPass.pProgram = RtProgram::create(mpDevice, desc, defines);
    }

    //Update defines that can change at runtime
    mTraceCameraPass.pProgram->addDefines(getRuntimeDefines());
    if (mpEmissiveLightSampler)
        mTraceCameraPass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

    //Program Vars
    if (!mTraceCameraPass.pVars)
        mTraceCameraPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mTraceCameraPass.pVars);

    //Bind Resources to shader
    auto var = mTraceCameraPass.pVars->getRootVar();
    //Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gMaxPathLength"] = mOptions.cameraMaxPathLength;
    var["CB"]["gJacobianDistanceThreshold"] = mOptions.jacobianDistanceThreshold;
    var["CB"]["gNeeSelectProbabilites"] = mNeeLightSelectProb;

    //RTXDI Resources
    mpRTXDI->setShaderData(var);
    //NEE Structures for Path Resampling 
    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->setShaderData(var["Light"]["gEmissiveSampler"]);
    if (mpEnvMapSampler)
        mpEnvMapSampler->setShaderData(var["Light"]["gEnvMapSampler"]);

    //Input Resources
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gView"] = renderData[kInputView]->asTexture();
    mpPhotonAS->bindTlas(var, "gPhotonAS");
    for (uint32_t i = 0; i < 2; i++)
    {
        var["gPhotonAABB"][i] = mpPhotonAABB[i];
        var["gPhotonData"][i] = mpPhotonData[i];
        if(mOptions.usePhotonGuiding)
            var["gPhotonGuidingData"][i] = mpPhotonGuidingData[i];
    }

    //Output Resources
    var["gPathReservoir"] = mpPathReservoir[mReservoirIndex % 2];

    //Dispatch Shader
    mpScene->raytrace(pRenderContext, mTraceCameraPass.pProgram.get(), mTraceCameraPass.pVars, uint3(mScreenRes, 1));
}

void ReSTIR_FG_Plus::backprojectCausticsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "BackprojectCaustics");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        return defines;
    };

    //Initialize Compute Pass
    if(!mpBackprojectCausticSamplesPass)
    {
         Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderBackprojectCaustics).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getRuntimeDefines());
       
        mpBackprojectCausticSamplesPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpBackprojectCausticSamplesPass);
    //Runtime defines
    mpBackprojectCausticSamplesPass->getProgram()->addDefines(getRuntimeDefines());

    //Set vars
    auto var = mpBackprojectCausticSamplesPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var); // Set scene data
    mpSampleGenerator->setShaderData(var);                    // Sample generator

    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gScreenDims"] = mScreenRes;

    var["gLightTraceHeadCounter"] = mpLightTraceHeadCounter;
    var["gLightTraceLinkedList"] = mpLightTraceLinkedList;
    var["gPhotonData"] = mpPhotonData[1]; //Caustic photon data
    var["gPhotonHitInfo"] = mpPhotonHitInfo;
    if(mOptions.usePhotonGuiding)
    {
        var["gPhotonGuidingData"] = mpPhotonGuidingData[1];
        var["gCausticReservoirGuidingData"] = mpPhotonGuidingCausticReservoir[mFrameCount % 2];
    }

    var["gCausticReservoir"] = mpCausticReservoir[mFrameCount % 2];

     // Execute
    mpBackprojectCausticSamplesPass->execute(pRenderContext, uint3(mScreenRes, 1));

    pRenderContext->uavBarrier(mpLightTraceHeadCounter.get());
}

void ReSTIR_FG_Plus::shiftPhotonPathPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ShiftPhotonPath");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mOptions.photonBufferSizeGlobal));
        defines.add("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mOptions.photonBufferSizeCaustic));
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights()? "1" : "0");
        defines.add("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights()? "1" : "0");
        defines.add("USE_ADAPTIVE_PHOTON_RADIUS", mOptions.photonUseAdaptiveRadius ? "1" : "0");
        defines.add(getMaterialDefines());
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        if(mOptions.usePhotonGuiding)
            defines.add(mpPhotonGuiding->getDefines());

        return defines;
    };

    // Init Shader
    if (!mShiftPhotonPathPass.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTracePhotons);
        desc.setMaxPayloadSize(sizeof(float) * 4);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        if (!mpScene->hasProceduralGeometry())
            desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mShiftPhotonPathPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mShiftPhotonPathPass.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getTypeConformances()));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("IS_SHIFT", "1");
        defines.add("IS_SHIFT_PATH", "1");
        defines.add("IS_SHIFT_CAUSTIC", "0");
        defines.add(getRuntimeDefines());

        mShiftPhotonPathPass.pProgram = RtProgram::create(mpDevice, desc, defines);
    }
    //Update Defines
    mShiftPhotonPathPass.pProgram->addDefines(getRuntimeDefines());
    
    // Program Vars
    if (!mShiftPhotonPathPass.pVars)
        mShiftPhotonPathPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mShiftPhotonPathPass.pVars);
    auto var = mShiftPhotonPathPass.pVars->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);

    if(mOptions.usePhotonGuiding)
        mpPhotonGuiding->setShaderData(var);

    //Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPhotonRadius"] = mOptions.photonUseAdaptiveRadius ? mOptions.photonAdaptiveRadius : mOptions.photonRadius;
    var["CB"]["gPackedPathLength"] = mOptions.photonPathLenght.pack();
    var["CB"]["gGlobalRejectionProb"] = mOptions.photonGlobalRejection;
    var["CB"]["gDispatchDimension"] = mPhotonDispatchDim;
    var["CB"]["gScreenDimensions"] = mScreenRes;
    var["CB"]["gMixedLightsAnalyticProbability"] = mOptions.photonMixedLightRatio;
    var["CB"]["gNormalizedPixelDiagonal"] = mApproximatePixelDiagonal;

    //Output Buffers
    var["gPathReservoir"] = mpPathReservoir[(mReservoirIndex + 1) % 2]; //Temporal Reservoir

    //Dispatch raytracing shader
    mpScene->raytrace(pRenderContext, mShiftPhotonPathPass.pProgram.get(), mShiftPhotonPathPass.pVars, uint3(mScreenRes.x, mScreenRes.y, 1));
}

void ReSTIR_FG_Plus::shiftCameraPathPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass)
{
    FALCOR_PROFILE(pRenderContext, "ShiftPathReservoir");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0"); 
        return defines;
    };

    //Perform shift for current and previous sample
    for(uint i=0; i<2; i++)
    {
        auto& pass = mShiftCameraPathPass[i];
        //Init Shader
        if (!pass.pProgram)
        {
            RtProgram::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderTraceCamera);
            desc.setMaxPayloadSize(sizeof(float) * 4);
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

            DefineList defines = {};
            defines.add(mpScene->getSceneDefines());
            defines.add("IS_SHIFT", "1");
            defines.add("SHIFT_IS_CURRENT", i==0 ? "1" : "0");

            pass.pProgram = RtProgram::create(mpDevice, desc, defines);
        }
        //Defines that can change on runtime
        pass.pProgram->addDefines(getRuntimeDefines());
        if (mpEmissiveLightSampler)
            pass.pProgram->addDefines(mpEmissiveLightSampler->getDefines());

        //Program Vars
        if (!pass.pVars)
            pass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

        FALCOR_ASSERT(pass.pVars);
        auto var = pass.pVars->getRootVar();

        //Constant Buffer
        var["CB"]["gFrameCount"] = mFrameCount;
        var["CB"]["gMaxPathLength"] = mOptions.cameraMaxPathLength;
        var["CB"]["gJacobianDistanceThreshold"] = mOptions.jacobianDistanceThreshold;
        var["CB"]["gNeeSelectProbabilites"] = mNeeLightSelectProb;

        var["ShiftCB"]["gNumResamplingPass"] = numPass;
        var["ShiftCB"]["gSpatialSampleRadius"] = mOptions.pathSpatialResamplingRadius;

        // NEE Structures for Path Resampling 
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
        var["gPathReservoir"] = mpPathReservoir[reservoirIndex];

        //Output Resources
        var["gShiftData"] = mpReservoirShiftData[i];

        //Dispatch Shader
        mpScene->raytrace(pRenderContext, pass.pProgram.get(), pass.pVars, uint3(mScreenRes, 1));
    } 
}

void ReSTIR_FG_Plus::resampleReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass)
{
    FALCOR_PROFILE(pRenderContext, "ResamplePathReservoirs");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        return defines;
    };

    //Initialize compute pass
    if (!mpResampleReservoirPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderResamplingPathReservoir).csEntry("main").setShaderModel(kShaderModel);
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
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gFrameDim"] = mScreenRes;
    var["CB"]["gConfidenceCap"] = mOptions.pathConfidenceCap;
    var["CB"]["gSpatialRadius"] = mOptions.pathSpatialResamplingRadius;
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
    var["gPathReservoirOther"] = mpPathReservoir[(mReservoirIndex + 1) % 2];
    var["gShiftData"] = mpReservoirShiftData[0];
    var["gShiftDataOther"] = mpReservoirShiftData[1];

    // In-/Output Resources
    var["gPathReservoir"] = mpPathReservoir[mReservoirIndex % 2];

    // Execute Compute Pass
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpResampleReservoirPass->execute(pRenderContext, uint3(targetDim, 1));
}

void ReSTIR_FG_Plus::shiftCausticPathPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ShiftCaustics");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mOptions.photonBufferSizeGlobal));
        defines.add("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mOptions.photonBufferSizeCaustic));
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights()? "1" : "0");
        defines.add("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights()? "1" : "0");
        defines.add("USE_ADAPTIVE_PHOTON_RADIUS", mOptions.photonUseAdaptiveRadius ? "1" : "0");
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        if(mOptions.usePhotonGuiding)
            defines.add(mpPhotonGuiding->getDefines());

        defines.add(getMaterialDefines());

        return defines;
    };

    // Init Shader
    if (!mShiftCausticPathPass.pProgram)
    {
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderTracePhotons);
        desc.setMaxPayloadSize(sizeof(float) * 4);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(1);
        if (!mpScene->hasProceduralGeometry())
            desc.setPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mShiftCausticPathPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mShiftCausticPathPass.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getTypeConformances()));
        sbt->setMiss(0, desc.addMiss("miss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }
        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add("IS_SHIFT", "1");
        defines.add("IS_SHIFT_PATH", "0");
        defines.add("IS_SHIFT_CAUSTIC", "1");
        defines.add(getRuntimeDefines());

        mShiftCausticPathPass.pProgram = RtProgram::create(mpDevice, desc, defines);
    }
    //Update Defines
    mShiftCausticPathPass.pProgram->addDefines(getRuntimeDefines());
    
    // Program Vars
    if (!mShiftCausticPathPass.pVars)
        mShiftCausticPathPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);
    FALCOR_ASSERT(mShiftCausticPathPass.pVars);
    auto var = mShiftCausticPathPass.pVars->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var);
    if(mOptions.usePhotonGuiding)
        mpPhotonGuiding->setShaderData(var);
        
    //Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPhotonRadius"] = mOptions.photonUseAdaptiveRadius ? mOptions.photonAdaptiveRadius : mOptions.photonRadius;
    var["CB"]["gPackedPathLength"] = mOptions.photonPathLenght.pack();
    var["CB"]["gGlobalRejectionProb"] = mOptions.photonGlobalRejection;
    var["CB"]["gDispatchDimension"] = mPhotonDispatchDim;
    var["CB"]["gScreenDimensions"] = mScreenRes;
    var["CB"]["gMixedLightsAnalyticProbability"] = mOptions.photonMixedLightRatio;
    var["CB"]["gNormalizedPixelDiagonal"] = mApproximatePixelDiagonal;

    //Output Buffers
    var["gCausticReservoir"] = mpCausticReservoir[(mFrameCount + 1) % 2]; //Temporal Reservoir
    var["gLightTraceHeadCounter"] = mpLightTraceHeadCounter;
    var["gLightTraceLinkedList"] = mpLightTraceLinkedList;
    if(mOptions.usePhotonGuiding)
        var["gCausticReservoirGuidingData"] = mpPhotonGuidingCausticReservoir[(mFrameCount + 1) % 2]; //Temporal guiding data

    //Dispatch raytracing shader
    mpScene->raytrace(pRenderContext, mShiftCausticPathPass.pProgram.get(), mShiftCausticPathPass.pVars, uint3(mScreenRes.x, mScreenRes.y, 1));

}

void ReSTIR_FG_Plus::backprojectTemporalCausticReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "BackprojectTemporalCausticReservoirs");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());  
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        return defines;
    };

    //Initialize Compute Pass
    if(!mpBackprojectTemporalCausticReservoirsPass)
    {
         Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderBackprojectCaustics).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getRuntimeDefines());
        defines.add("BACKPROJECT_TMP_RESERVOIRS", "1");
       
        mpBackprojectTemporalCausticReservoirsPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpBackprojectTemporalCausticReservoirsPass);
    //Runtime defines
    mpBackprojectTemporalCausticReservoirsPass->getProgram()->addDefines(getRuntimeDefines());

    //Set vars
    auto var = mpBackprojectTemporalCausticReservoirsPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var); // Set scene data
    //mpSampleGenerator->setShaderData(var);                    // Sample generator

    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gScreenDims"] = mScreenRes;

    var["gLightTraceHeadCounter"] = mpLightTraceHeadCounter;
    var["gLightTraceLinkedList"] = mpLightTraceLinkedList;

    var["gCausticReservoir"] = mpCausticReservoir[(mFrameCount + 1) % 2]; //Temporal Reservoir

     // Execute
    mpBackprojectTemporalCausticReservoirsPass->execute(pRenderContext, uint3(mScreenRes, 1));

    pRenderContext->uavBarrier(mpLightTraceHeadCounter.get());
}

void ReSTIR_FG_Plus::resampleReservoirCausticPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ResamplingCaustics");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add(getMaterialDefines());
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");

        return defines;
    };

    // Initialize compute pass
    if (!mpResampleReservoirCausticPass)
    {
        Program::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderResamplingReservoirCaustic).csEntry("main").setShaderModel(kShaderModel);
        desc.addTypeConformances(mpScene->getTypeConformances());

        DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());
        defines.add(getRuntimeDefines());

        mpResampleReservoirCausticPass = ComputePass::create(mpDevice, desc, defines, true);
    }
    FALCOR_ASSERT(mpResampleReservoirCausticPass);
    mpResampleReservoirCausticPass->getProgram()->addDefines(getRuntimeDefines()); //Runtime define

    // Set variables
    auto var = mpResampleReservoirCausticPass->getRootVar();
    mpScene->setRaytracingShaderData(pRenderContext, var); // Set scene data
    mpSampleGenerator->setShaderData(var);                 // Sample generator

    // Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gFrameDim"] = mScreenRes;
    var["CB"]["gConfidenceCap"] = mOptions.causticConfidenceCap;
    var["CB"]["gPrevCamPos"] = mTemporalCameraPosition;
    var["CB"]["gPrevCamViewProjection"] = mTemporalCameraViewProjection;
    var["CB"]["gPrevCamForward"] = mTemporalCameraForward;

    // Input Resources
    var["gMVec"] = renderData[kInputMotionVectors]->asTexture();
    var["gCausticReservoirPrev"] = mpCausticReservoir[(mFrameCount + 1) % 2];
    var["gLightTraceLinkedList"] = mpLightTraceLinkedList;

    // In-/Output Resources
    var["gCausticReservoir"] = mpCausticReservoir[mFrameCount % 2];
    var["gLightTraceHeadCounter"] = mpLightTraceHeadCounter; //Is cleared

    if(mOptions.usePhotonGuiding)
    {
        var["gCausticReservoirGuidingDataPrev"] = mpPhotonGuidingCausticReservoir[(mFrameCount + 1) % 2];
        var["gCausticReservoirGuidingData"] = mpPhotonGuidingCausticReservoir[mFrameCount % 2];
    }

    // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpResampleReservoirCausticPass->execute(pRenderContext, uint3(targetDim, 1));

    pRenderContext->uavBarrier(mpLightTraceHeadCounter.get());
}

void ReSTIR_FG_Plus::evaluateReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "EvaluateReservoirs");

    auto getRuntimeDefines = [&](){
        DefineList defines = {};
        defines.add(getMaterialDefines());
        defines.add("USE_ENV_BACKROUND", mpScene->useEnvBackground() ? "1" : "0");
        defines.add("RNG_NUM_PASSES", std::to_string(mRNGNumPasses));
        defines.add(mpRTXDI->getDefines());
        defines.add("DISABLE_DIRECT", mOptions.debugDisableDirectLight ? "1" : "0");
        defines.add("DISABLE_INDIRECT", mOptions.debugDisableIndirectLight ? "1" : "0");
        defines.add("DISABLE_CAUSTICS", mOptions.debugDisableCaustics ? "1" : "0");
        defines.add("USE_PHOTON_GUIDING", mOptions.usePhotonGuiding ? "1" : "0");
        if(mOptions.usePhotonGuiding)
            defines.add(mpPhotonGuiding->getDefines());
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
    if(mOptions.usePhotonGuiding)
        mpPhotonGuiding->setShaderData(var);

    //Constant Buffer
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gFrameDim"] = mScreenRes;

    //RTXDI resources
    mpRTXDI->setShaderData(var);

    //Input
    var["gVBuffer"] = renderData[kInputVBuffer]->asTexture();
    var["gView"] = renderData[kInputView]->asTexture();
    var["gPathReservoir"] = mpPathReservoir[mReservoirIndex % 2];
    var["gCausticReservoir"] = mpCausticReservoir[mFrameCount % 2];
    if(mOptions.usePhotonGuiding)
        var["gCausticReservoirGuidingData"] = mpPhotonGuidingCausticReservoir[mFrameCount % 2];

    //Output
    var["gVBufferPrev"] = mpVBufferPrev;
    var["gViewPrev"] = mpViewPrev;
    var["gOutColor"] = renderData[kOutputColor]->asTexture();

    // Execute
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
    mpEvaluateReservoirsPass->execute(pRenderContext, uint3(targetDim, 1));
}

DefineList ReSTIR_FG_Plus::getMaterialDefines()
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

void ReSTIR_FG_Plus::RayTraceProgramHelper::initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator)
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
