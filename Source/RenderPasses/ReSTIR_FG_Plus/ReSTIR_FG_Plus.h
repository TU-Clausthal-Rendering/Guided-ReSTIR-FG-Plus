#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Rendering/RTXDI/RTXDI.h"
#include "Rendering/Lights/EmissiveLightSampler.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EnvMapSampler.h"

#include "Rendering/AccelerationStructure/CustomAccelerationStructure.h"
#include "Rendering/PhotonGuiding/PhotonGuiding.h"

using namespace Falcor;

class ReSTIR_FG_Plus : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_FG_Plus, "ReSTIR_FG_Plus", "Real-Time Global Illumination with Caustics");

    static ref<ReSTIR_FG_Plus> create(ref<Device> pDevice, const Properties& props) { return make_ref<ReSTIR_FG_Plus>(pDevice, props); }

    ReSTIR_FG_Plus(ref<Device> pDevice, const Properties& props);

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
        uint diffuse = 3;   //Max Diffuse Bounces on the path
        uint specular = 3;  //Max Specular Bounces on the path
        uint delta = 10;    //Max Delta Bounces on the Path

        const uint pack() const
        {
            return (bounces & 0xFF) | ((diffuse & 0xFF) << 8) | ((specular & 0xFF) << 16) | ((delta & 0xFF) << 24);
        }
    };

    struct Options {

        //
        //Photon Tracing settings
        //
        PathLengthSettings photonPathLenght = {};       //Photon Path Length
        uint photonsDispatched = 1000000;               //Number of photons, that are distributed each frame
        uint photonBufferSizeGlobal = 2000000;          //Maximum global photons that can be stored
        uint photonBufferSizeCaustic = 2000000;         //Maximum caustic photons that can be stored
        float photonMixedLightRatio = 0.5f;             //Ratio if both analytic and emissive lights are used. 0 -> 0% Analytic, 100% Emissive
        //Radius
        bool photonUseAdaptiveRadius = true;            //Uses a photon radius that is dependent on the (linear) distance to the camera
        float2 photonAdaptiveRadius = float2(4.f, 2.f); //(Global|Caustic) Pixel Size scale for adptive radius
        float2 photonRadius = float2(0.008f, 0.002f);   //(Global|Caustic) Fixed World Space Radius
        //Optimization
        float photonGlobalRejection = 0.3f;             // Fixed probability that a global photon is not stored
        float photonASBuildBufferOverestimate = 1.15f; // Guard percentage for AS building (Uses (delayed) CPU Photon Counter to estimate)

        bool usePhotonGuiding = true;                   //Enable Photon Guiding
        //
        // Camera Path Tracing Settings
        //

        uint cameraMaxPathLength = 10;

        //
        // Resample Settings
        //

        bool enablePathResampling = true;                  // Resampling for paths is enabled
        uint pathNumberSpatialSamples = 1;                 // Number of spatial samples
        float pathSpatialResamplingRadius = 30.f;          // Spatial resampling radius
        uint pathConfidenceCap = 20;                       // Confidence cap for path reservoirs

        bool enableCausticResampling = true;               // Resampling for caustics is enabled
        uint causticConfidenceCap = 20;                    // Confidence Cap for caustic samples

        float jacobianDistanceThreshold = 0.001f;          // Threshold for Jacobian distances
        float normalAngleThreshold = 0.6f;                 // Cosine of maximum angle between both normals allowed
        float relativeDepthThreshold = 0.15f;              // Relative Depth threshold (is neighbor 0.1 = 10% as near as the current depth)

        bool shiftPhotonPaths = true;                     // Should photon paths be shifted (can be disables for static scenes)

        //
        // Material Options
        //

        bool useLambertianDiffuseBSDF = false;          // Diffuse BSDF used by ReSTIR PT and SuffixReSTIR
        uint diffuseClassificationBSDFLobes = 1;        // 0: Only Threshold is used. 1: BSDF lobes + threshold is used to determine if a surface is diffuse
        float specularRoughnessThreshold = 0.25f;       // Any material below this is considered specular
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

    //Traces the photons and builds the photon Acceleration Structure
    void tracePhotonsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Generates the initial Path Samples and initialized RTXDI Surfaces
    void traceCameraPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Collects caustic backprojections and initializes the caustic reservoir
    void backprojectCausticsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Shifts the photon pass for path reservoirs
    void shiftPhotonPathPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Shifts the Camera path reservoirs with Path Length > 0 by performing a retrace
    void shiftCameraPathPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass);

    //Reservoir Resampling for Path Reservoirs
    void resampleReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData, uint numPass);

    // Shift and backproject for the temporal caustic reservoirs
    void shiftCausticPathPass(RenderContext* pRenderContext, const RenderData& renderData);

    // Backproject temporal caustic reservoirs
    void backprojectTemporalCausticReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData);

    //Reservoir Resampling for Caustic Reservoirs
    void resampleReservoirCausticPass(RenderContext* pRenderContext, const RenderData& renderData);

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
    std::unique_ptr<PhotonGuiding> mpPhotonGuiding; //Photon Guiding

    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler; // Light Sampler
    std::unique_ptr<CustomAccelerationStructure> mpPhotonAS;      // Photon Accleration Structure
    std::unique_ptr<EnvMapSampler> mpEnvMapSampler;               // Env Map Sampler

    //
    // Parameters
    //
    Options mOptions = {};                     //Options for the renderer
    uint mFrameCount = 0;                      //Current Frame 
    uint mReservoirIndex = 0;                  //Track reservoir index for path reservoir
    uint2 mScreenRes = uint2(0, 0);            //Screen Resolution
    bool mResetScreenTex = false;        
    bool mOptionsChanged = false;

    // Light
    bool mHasLights = false;           // True if the scene has any light sources
    EmissiveLightSamplerType mEmissiveLightSamplerType = EmissiveLightSamplerType::Power;    //Emissive Sampler Type
    LightBVHSampler::Options mLightBVHOptions;                                               //(Cached) Options for Light BVH Sampler
    bool mRebuildLightSampler = false;                                                       //If true, Emissive Sampler is rebuild
    float3 mNeeLightSelectProb = float3(0.33f);                                              //Probability to select a NEE/Analytic/EnvMapSample

    //Trace Photon
    uint2 mPhotonDispatchDim = uint2(32);  //Dispatch Dims for photon.
    float mApproximatePixelDiagonal = 1.f; //Approximated pixel diagonal used for adaptive radius. Updated every frame in tracePhoton

    //ReSTIR Reservoirs
    bool mRebuildReservoirBuffer = false;               // Rebuild the reservoir buffer
    bool mClearReservoir = true;                        // Clears both reservoirs
    bool mCanResample = false;                          // Resampling is only allowed if last iterations reservoir was created
    
    uint mRNGNumPasses = 11;                             // Offset for RNG generator

    //Splatting, camera data from last frame
    float4x4 mTemporalCameraViewProjection = float4x4::identity();
    float3 mTemporalCameraPosition = float3(0);
    float3 mTemporalCameraForward = float3(0);

    //Photon Distribution
    uint2 mPhotonCountUI = uint2(mOptions.photonBufferSizeGlobal, mOptions.photonBufferSizeCaustic);
    bool mPhotonBufferSizeChanged = false;

    //Debug
    bool mClearDebugTexture = true; 

    //
    // Resources
    //
    ref<Texture> mpVBufferPrev;             // VBuffer Hit from last frame
    ref<Texture> mpViewPrev;                // Camera View from last frame
    ref<Buffer> mpPhotonAABB[2];            // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpPhotonData[2];            // Additional Photon data (flux, dir)
    ref<Buffer> mpPhotonHitInfo;            // Hit info for Photons. Is used for backprojections
    ref<Buffer> mpPhotonCounter;            // Photon Counter
    ref<Buffer> mpPhotonCounterCPU;         // CPU copy of counter for readback
    ref<Buffer> mpCausticReservoir[2];      // Reservoir for the Caustic sample
    ref<Buffer> mpPathReservoir[2];         // Reservoir storing the path in primary path space
    ref<Texture> mpReservoirShiftData[2];   // Shift data for path reservoir (path throughput and jacobian)

    ref<Texture> mpLightTraceHeadCounter;   //Screen size head buffer counter for light tracing to store the first hit
    ref<Buffer> mpLightTraceLinkedList;     //Linked List for light tracing

    //Photon Guiding additional resources
    ref<Buffer> mpPhotonGuidingData[2];     //Extra photon data needed for guiding
    ref<Texture> mpPhotonGuidingCausticReservoir[2]; //Extra data for caustic reservoirs

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

    RayTraceProgramHelper mTracePhotonPass;             // Trace Photons and build AS
    RayTraceProgramHelper mTraceCameraPass;             // Trace Camera rays and collect photons
    RayTraceProgramHelper mShiftCameraPathPass[2];      // Retrace the path Reservoirs needed for GRIS
    RayTraceProgramHelper mShiftPhotonPathPass;         // Retrace the photon path
    RayTraceProgramHelper mShiftCausticPathPass;        // Retrace the caustic path

    ref<ComputePass> mpBackprojectCausticSamplesPass;   // Backproject the caustic samples
    ref<ComputePass> mpBackprojectTemporalCausticReservoirsPass;    //Backprojectes caustic temporal reservoirs if photon shift is disabled
    ref<ComputePass> mpResampleReservoirPass;           // Resampling Pass for the Path Reservoirs
    ref<ComputePass> mpResampleReservoirCausticPass;    // Resampling Pass for Caustic Reservoirs
    ref<ComputePass> mpEvaluateReservoirsPass;          // Evaluates ReSTIR DI and FG reservoirs
};

