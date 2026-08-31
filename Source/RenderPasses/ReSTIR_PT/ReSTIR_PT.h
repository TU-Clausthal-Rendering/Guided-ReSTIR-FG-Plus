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
#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Rendering/RTXDI/RTXDI.h"
#include "Rendering/Lights/EmissiveLightSampler.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EnvMapSampler.h"

using namespace Falcor;

class ReSTIR_PT : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_PT, "ReSTIR_PT", "ReSTIR Path Tracing");

    static ref<ReSTIR_PT> create(ref<Device> pDevice, const Properties& props) { return make_ref<ReSTIR_PT>(pDevice, props); }

    ReSTIR_PT(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    struct PathLengthSettings {
        uint bounces = 10;  //Total Bounces
        uint diffuse = 4;   //Max Diffuse Bounces on the path
        uint specular = 4;  //Max Specular Bounces on the path
        uint delta = 10;    //Max Delta Bounces on the Path

        const uint pack() const
        {
            return (bounces & 0xFF) | ((diffuse & 0xFF) << 8) | ((specular & 0xFF) << 16) | ((delta & 0xFF) << 24);
        }
    };

    struct Options {

        //
        // Camera Path Tracing Settings
        //

        PathLengthSettings pathLength = {};

        //
        // Resample Settings
        //

        bool enableResampling = true;                       // Resampling for paths is enabled
        uint numberSpatialSamples = 1;                      // Number of spatial samples
        float spatialResamplingRadius = 30.f;               // Spatial resampling radius
        uint confidenceCap = 20;                            // Confidence cap for path reservoirs

        float jacobianDistanceThreshold = 0.001f;          // Threshold for Jacobian distances
        float normalAngleThreshold = 0.6f;                 // Cosine of maximum angle between both normals allowed
        float relativeDepthThreshold = 0.15f;              // Relative Depth threshold (is neighbor 0.1 = 10% as near as the current depth)

        bool shiftReconnectionSample = true;               //Should the path after reconnection also be shifted during temporal resampling (can be disabled for static scenes)

        //
        // Material Options
        //

        bool useLambertianDiffuseBSDF = false;          // Diffuse BSDF used by ReSTIR PT and SuffixReSTIR
        float specularRoughnessThreshold = 0.25f;       // Any material below this is considered specular
        uint diffuseClassificationBSDFLobes = 1;        // 0: Only Threshold is used. 1: BSDF lobes + threshold is used to determine if a surface is diffuse
        bool evaluateDeltaPDFs = false;                 // If set on true, delta pdfs are evaluated (always 0), else they are set to 1
        bool enableAlphaTest = true;                    // Alpha Test

        //
        // Debug Options
        //

        bool debugDisableDirectLight = false;           // Disable direct light in eval
        bool debugDisableIndirectLight = false;         // Disable indirect light without backprojected caustics in eval
        bool debugDisableCaustics = false;              // Disable backprojected caustics in eval
    };

    //Resets all render passes
    void resetAllRenderPasses();

    //Initializes the emissive sampler used to sample photons
    void prepareLightingStructure(RenderContext* pRenderContext);

    //Initializes and updates all textures and buffers
    void prepareResources(RenderContext* pRenderContext, const RenderData& renderData);

    //Generates the initial Path Samples and initialized RTXDI Surfaces
    void traceInitialPathsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Shifts the Camera path reservoirs with Path Length > 0 by performing a retrace
    void shiftPathPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass);

    //Reservoir Resampling for Path Reservoirs
    void resampleReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass);

    //Evaluate all Reservoirs (Path, Caustic and RTXDI)
    void evaluateReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Get Materials defines
    DefineList getMaterialDefines();

    //
    // Pointers
    //
    ref<Scene> mpScene;                     // Scene Pointer
    ref<SampleGenerator> mpSampleGenerator; // GPU Sample Gen
    std::unique_ptr<RTXDI> mpRTXDI;         // Ptr to RTXDI for direct use
    RTXDI::Options mRTXDIOptions;           // Options for RTXDI
    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    std::unique_ptr<EnvMapSampler> mpEnvMapSampler;               // Env Map Sampler

    //
    // Parameters
    //
    Options mOptions = {};                     //Options for the renderer
    uint mFrameCount = 0;                      //Current Frame
    uint mSGSeed = 0;                          //Sample Generator initialize Seed. Is increased after every use
    uint mReservoirIndex = 0;                  //Track reservoir index for path reservoir
    uint2 mScreenRes = uint2(0, 0);            //Screen Resolution
    bool mResetScreenTex = false;        
    bool mOptionsChanged = false;
    uint mReservoirSelectSGSeed = 0;           // Running sample generator seed for reservoir select sample offset (e.g. random pixel in spatial radius)

    // Light
    EmissiveLightSamplerType mEmissiveLightSamplerType = EmissiveLightSamplerType::Power; //Emissive Sampler Type
    LightBVHSampler::Options mLightBVHOptions;                                               //(Cached) Options for Light BVH Sampler
    bool mRebuildLightSampler = false;                                                       //If true, Emissive Sampler is rebuild
    float3 mNeeLightSelectProb = float3(0.33f);                                              //Probability to select a NEE/Analytic/EnvMapSample

    bool mClearReservoir = true;                        // Clears both reservoirs
    bool mCanResample = false;                          // Resampling is only allowed if last iterations reservoir was created

    //Debug
    bool mClearDebugTexture = true; 

    //
    // Resources
    //
    ref<Texture> mpVBufferPrev;             // VBuffer Hit from last frame
    ref<Texture> mpViewPrev;                // Camera View from last frame
    ref<Buffer> mpReservoir[2];             // Reservoir storing the path in primary path space
    ref<Texture> mpReservoirShiftData[2];   // Shift data for path reservoir (path throughput and jacobian)
    ref<Buffer> mpReservoirShiftSurface[2]; // Shift surface for reconnection hits
    ref<Texture> mpShiftedRcThp[2];            // Shifted Reconnection Thp

    //
    // Render Passes/Programs
    //
    struct RayTraceProgramHelper
    {
        ref<RtProgram> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;

        static const RayTraceProgramHelper create()
        {
            RayTraceProgramHelper r;
            r.pProgram = nullptr;
            r.pBindingTable = nullptr;
            r.pVars = nullptr;
            return r;
        }

        void initProgramVars(ref<Device> pDevice, ref<Scene> pScene, ref<SampleGenerator> pSampleGenerator);
    };

    RayTraceProgramHelper mTraceInitialPathPass;         // Trace initial Path
    RayTraceProgramHelper mTraceShiftPass[4];            // Retraces the shifted path. Uses same shader as InitialPath. Has 3 Versions with different defines

    ref<ComputePass> mpResampleReservoirPass;           // Resample the reservoirs
    ref<ComputePass> mpEvaluateReservoirsPass;          // Evaluates Reservoirs
};
