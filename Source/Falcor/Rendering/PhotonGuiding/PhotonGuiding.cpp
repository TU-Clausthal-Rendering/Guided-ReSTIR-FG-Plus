#include "PhotonGuiding.h"

namespace Falcor
{
    //Shader Paths and UI elements
    namespace
    {
        const std::string kShaderFolder = "Rendering/PhotonGuiding/"; 
        const std::string kShaderReduce = kShaderFolder + "Reduce.cs.slang";
        const std::string kShaderUpdateHistograms = kShaderFolder + "UpdateHistograms.cs.slang"; 
        const std::string kShaderUpdateGuidingMaps = kShaderFolder + "UpdateGuidingMaps.cs.slang";
        const std::string kShaderDynamicPMin = kShaderFolder + "DynamicPhotonMin.cs.slang";
        const std::string kShaderDebug = kShaderFolder + "DebugView.cs.slang";

        const std::string kShaderModel = "6_6";

        const uint kAutomaticMappingCount = 1000; // Enable mapping if light count is bigger than this value
        const uint kMappingInvalidIndex = 0xFFFF; // Invalid index for mapping
        const uint kCounterIndexMapping = 0;      // Counter Buffer Index used in Mapping
        const uint kCounterIndexDynamicPhoton = 1;// Counter Buffer Index uesd for dynamic minimum photon count

        //UI Dropdown lists
        const Gui::DropdownList kGuidingMapResolutionDropdownList = {
            {8, "8"},
            {16, "16"},
            {32, "32"},
            {64, "64"},
            {128, "128"},
            {256, "256"},
            {512, "512"}
        };
    }

    PhotonGuiding::PhotonGuiding(ref<Device> pDevice, ref<Scene> pScene, RenderContext* pRenderContext)
    {
        mpDevice = pDevice;

        if (!pScene) {
            throw std::exception("PhotonGuiding: Was initialized with an empty scene!");
        }

        mpScene = pScene;
        prepareResources(pRenderContext);
    }

    PhotonGuiding::~PhotonGuiding()
    {
        clearResources();
    }

    void PhotonGuiding::update(RenderContext* pRenderContext, uint maxPhotonsDistributed)
    {
        FALCOR_PROFILE(pRenderContext, "PhotonGuiding_UpdateResources");

        //Increase to minimum needed amount. A warning is triggered in the UI in this case (see getPhotonDispatchSize())
        maxPhotonsDistributed = std::max(mMinPhotonNeededForGuiding, maxPhotonsDistributed);

        //Check if resources needs to be rebuild 
        prepareResources(pRenderContext);

        //Get per light and total contribution
        reduceContributionPass(pRenderContext);

        //For debug view, directional guiding maps should be cleard, as some remains from other frames can be visible otherwise
        if(mOptions.debugEnable)
        {
            pRenderContext->clearUAV(mpGuidingMapsDirection->getUAV(0).get(), uint4(0));
        }

        //
        //Update the Guiding Maps.
        //
        
        //First temporally accumulate the light resources
        updateHistogramsPass(pRenderContext, false);

        //Dynamic PMin
        if(mOptions.useDynamicPMin)
            dynamicallyPMinPass(pRenderContext);

        //Convert Light Histogram to Light Guiding Map by distributing the photons using the histogram as a guide
        updateGuidingMapsPass(pRenderContext, maxPhotonsDistributed, false);

        //Update the direction histogram per light (stored in an atlas) 
        updateHistogramsPass(pRenderContext, true);

        //Distribute the photons from each light to the directions, if enough photons are distributed for a light
        updateGuidingMapsPass(pRenderContext, maxPhotonsDistributed, true);


        //Clear Resources
        {
            FALCOR_PROFILE(pRenderContext, "ClearResources");
            //Clear contribution
            pRenderContext->clearUAV(mpContributionDirection->getUAV(0).get(), uint4(0));
            //Clear counter
            if(mOptions.useMappingScheme || mOptions.useDynamicPMin)
                pRenderContext->clearUAV(mpResourceCounter->getUAV(0).get(), uint4(0));
            //Clear Mapping Resources
            if(mOptions.useMappingScheme)
            {
                //Swap current and previous (ping-pong)
                std::swap(mpMapLightToDirectionPrev, mpMapLightToDirection);
                std::swap(mpHistogramDirection, mpHistogramDirectionPrev);

                //Clear
                pRenderContext->clearUAV(mpContributionLight->getUAV(0).get(), uint4(0));
                pRenderContext->clearUAV(mpMapDirectionToLight->getUAV(0).get(), uint4(mTotalLightCount));
                pRenderContext->clearUAV(mpMapLightToDirection->getUAV(0).get(), uint4(kMappingInvalidIndex));
            }
        }

        mGuidingIterationCount++;
    }

    uint2 PhotonGuiding::getPhotonDispatchSize(uint numberOfPhotons)
    {
        FALCOR_ASSERT(numberOfPhotons > 0);

        //Ensure that number of photons are at least enough to cover min photons
        uint minPhotonsPerLight = mOptions.useDynamicPMin ?
            std::min(mOptions.dynamicPMinPhotonsMinMax.x, mOptions.dynamicPMinPhotonsMinMax.y) :
            mOptions.reservedPhotonsPerLight;
        mMinPhotonNeededForGuiding =  (mTotalLightCount + 16) * minPhotonsPerLight;
        if(numberOfPhotons < mMinPhotonNeededForGuiding)
        {
            mWarningNotEnoughDispatchedPhotons = true;
            numberOfPhotons = mMinPhotonNeededForGuiding;
        }else
            mWarningNotEnoughDispatchedPhotons = false;

        //Check if the optimized dispatch size was calculated last frame
        if(numberOfPhotons == mTracePhotonNumberOfPhotonsLastFrame)
            return mOptimalTracePhotonDispatchDims;

        //Convert into block coordinates
        const uint blockSize = mOptions.traversalBlockSize;
        const uint blockArea = blockSize * blockSize;
        //Minimum number of blocks needed
        const uint requiredBlocks = (numberOfPhotons / blockArea) + 1;

        //Start near the square root
        uint startY = static_cast<uint>(std::sqrt(requiredBlocks));
        uint2 bestDims = uint2(0);
        uint bestArea = std::numeric_limits<uint>::max();
        uint bestDiff = std::numeric_limits<uint>::max();

        // Search downward from the square root
        // Loop is aborted, as soon as first local optimum is found (to remain as square as possible)
        for(uint y = startY; y>= 1; y--)
        {
            uint x = (requiredBlocks / y) + 1;
            uint area = x * y;
            uint diff = (x>y) ? (x-y) : (y-x);

            if(area < bestArea || (area == bestArea && diff < bestDiff))
            {
                bestDims = uint2(x,y);
                bestDiff = diff;
                bestArea = area;
            }
            else //Abort after first local optimum
            {
                break;
            }
        }

        //Store result
        mTracePhotonNumberOfPhotonsLastFrame = numberOfPhotons;
        mOptimalTracePhotonDispatchDims = bestDims * blockSize;
        return mOptimalTracePhotonDispatchDims;

    }

    DefineList PhotonGuiding::getDefines()
    {
        DefineList defines = {};
        defines.add("PHOTON_GUIDING_ENABLED", "1"); 
        defines.add("PHOTON_GUIDING_DISCRETIZED_CONTRIBUTION_FACTOR", std::to_string(mOptions.discretizedContributionFactor));
        defines.add("PHOTON_GUIDING_DISCRETIZED_CONTRIBUTION_MAX", std::to_string(mOptions.discretizedContributionMax));
        defines.add("PHOTON_GUIDING_USE_MAPPED_GM", std::to_string(mOptions.useMappingScheme));

        //Create sample defines
        defines.add("PHOTON_GUIDING_TOTAL_LIGHT_COUNT", std::to_string(mTotalLightCount));
        defines.add("PHOTON_GUIDING_TRAVERSAL_BLOCK_SIZE", std::to_string(mOptions.traversalBlockSize));
        defines.add("PHOTON_GUIDING_LIGHT_GM_MAX_MIP", std::to_string(mpGuidingMapLight->getMipCount() - 1));
        defines.add("PHOTON_GUIDING_DIRECTION_GM_MAX_MIP", std::to_string(mMipLevelsDirGM - 1));
        defines.add("PHOTON_GUIDING_MIN_PHOTONS_FOR_DIR_GUIDING", std::to_string(mOptions.photonNeededForGM));
        defines.add("PHOTON_GUIDING_ANALYTIC_LIGHT_OFFSET", std::to_string(mEmissiveLightCount));
        defines.add("PHOTON_GUIDING_MAP_TEXTURE_SIZE", std::to_string(mResolutionMap));

        return defines;
    }

    void PhotonGuiding::setShaderData(const ShaderVar& rootVar)
    {
        auto var = rootVar["gPhotonGuiding"];

        var["gResolutionLightGM"] = mResolutionLightGM;
        var["gMapTextureResolution"] = mResolutionMap;
        var["gResolutionGM"] = mOptions.guidingMapResolution;

        var["gContributionLight"] = mpContributionLight;
        var["gContributionDirection"] = mpContributionDirection;
        var["gMapLightToDirection"] = mpMapLightToDirectionPrev; //Previous is used, as ping pong swap happends at the end of update

        var["gGuidingMapLight"] = mpGuidingMapLight;
        var["gGuidingMapsDirection"] = mpGuidingMapsDirection;
    }

    bool PhotonGuiding::renderUI(Gui::Widgets& widget)
    {
        bool changed = false;
        if(mWarningNotEnoughDispatchedPhotons)
            widget.text("WARNING: Not enough photons dispatched. Amount is automatically adjusted to:\n " + std::to_string(mTracePhotonNumberOfPhotonsLastFrame));

        widget.text("Tipp: Changing the values will trigger recompilation. \n Use \"Strg+Space\" to stop rendering when changing value. \n Press \"Strg+Space\" again to resume.");

        //TODO Move text on top of dropdown
        //TODO Add tooltips
        mRebuildResources |= widget.dropdown("Guiding Map Resolution", kGuidingMapResolutionDropdownList, mOptions.guidingMapResolution);
        widget.separator();
        changed |= widget.var("Contribution Discretization Factor", mOptions.discretizedContributionFactor, 1u, UINT_MAX, 1u);
        changed |= widget.var("Contribution Max Value", mOptions.discretizedContributionMax, mOptions.discretizedContributionFactor, UINT_MAX, 1u);
        widget.separator();
        changed |= widget.var("Exponential Moving Average (alpha)", mOptions.exponentialMovingAverageFactor, 0.f, 1.f, 0.0001f);
        if(auto group = widget.group("Mapping"))
        {
            group.text("Mapping is enabled by default on scenes with more than 1000 light sources");
            mRebuildResources |= group.checkbox("Enable", mOptions.useMappingScheme);
            mRebuildResources |= group.dropdown("Maximum Directional Resources", kGuidingMapResolutionDropdownList,mOptions.mappingDirGMCount); //"Reuse" Resolution dropdown
            changed |= group.var("Min Photon to create a Guiding Map", mOptions.mappingPhotonNeededToCreate, mOptions.photonNeededForGM, UINT_MAX, 1u);
        }

        if(auto group = widget.group("Dynamic PMin"))
        {
            group.text("Dynamic PMin is enabled by default on scenes with more than 1000 light sources");
            group.checkbox("Enable", mOptions.useDynamicPMin);
            group.var("Min/Max Distance", mOptions.dynamicPMinMinMaxDistance, 0.f, FLT_MAX, 0.0001f);
            group.var("Min/Max Photons", mOptions.dynamicPMinPhotonsMinMax, 1, UINT_MAX, 1u);
        }
        changed |= widget.var("Reserved Photons per Light", mOptions.reservedPhotonsPerLight, 1u, UINT_MAX, 1u);
        changed |= widget.var("Reserved Photons per Direction Cell", mOptions.reservedPhotonsPerDirection, 1u, UINT_MAX, 1u);
        uint minPhotonsNeeded = mOptions.reservedPhotonsPerDirection * mOptions.guidingMapResolution * mOptions.guidingMapResolution;
        changed |= widget.var("Light Photons needed to create Directional Guiding Map", mOptions.photonNeededForGM, minPhotonsNeeded, UINT_MAX, 1u);
        widget.separator();
        widget.dropdown("Trace Photon: Block Size", kGuidingMapResolutionDropdownList, mOptions.traversalBlockSize);
        
        changed |= mRebuildResources;

        //Debug settings, will not change the "changed" variable, as these settings have no influence over normal rendering
        if (auto group = widget.group("Debug")) {
            group.checkbox("Enable", mOptions.debugEnable);
            if(mOptions.debugEnable)
            {
                group.text("Changing Debug Variables will not trigger recompilation.");
                //Show Mapping Texture
                if (mOptions.useMappingScheme)
                {
                    group.checkbox("Mapping show directional Guiding Maps", mOptions.debugMappingShowDirectionalGM);
                    group.tooltip("Shows the mapped directional guiding resources. WARNING: Can flicker due to random index assigment for each frame");
                }

                //Select Light Index
                group.var("Selected Light Index", mOptions.debugSelectedLightIndex, -1, int(mTotalLightCount) - 1, 1);
                group.tooltip("-1 -> disabled. Otherwise it shows the GM of the selected light. Enable the Light Helper for easier selection of the Guiding Map that should be magnified.");
                group.checkbox("Select Light Helper", mOptions.debugSelectLightHelperMode);
                group.tooltip("Enables a helper mode to vizualize which light is currently selected");
                if(mOptions.debugSelectLightHelperMode)
                    group.rgbColor("Light Helper Color",mOptions.debugSelectLightHelperModeColor);

                //Color and scale for GMs
                group.var("Scale Light Guiding Map", mOptions.debugLightGMScaleFactor, 0.f, FLT_MAX, 0.001f);
                group.rgbColor("Light Guiding Map Color", mOptions.debugLightGMColor);
                group.var("Scale Direction Guiding Maps", mOptions.debugDirGMScaleFactor, 0.f, FLT_MAX, 0.001f);
                group.rgbColor("Direction Guiding Maps Color", mOptions.debugDirGMColor);
            }
        }

        return changed;
    }

    void PhotonGuiding::renderDebugView(RenderContext* pRenderContext, ref<Texture>& dstTexture)
    {
        if(!mOptions.debugEnable || !dstTexture)
            return;

        FALCOR_PROFILE(pRenderContext, "DebugView");

        auto getRuntimeDefines = [&](){
            DefineList defines;
            defines.add("USE_MAPPING", mOptions.useMappingScheme ? "1" : "0");
            defines.add("LIGHT_COUNT", std::to_string(mTotalLightCount));
            defines.add("LIGHT_GM_SIZE", std::to_string(mResolutionLightGM));
            defines.add("DIR_GM_SIZE", std::to_string(mResolutionDirGM));
            defines.add("GUIDING_TEXTURE_SIZE", std::to_string(mOptions.guidingMapResolution));
            defines.add("MAP_TEXTURE_SIZE", std::to_string(mResolutionMap));
            return defines;
        };

        //Create Compute Pass
        if (!mpDebugViewPass) {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderDebug).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            defines.add("MAPPING_INVALID_INDEX", std::to_string(kMappingInvalidIndex));
            defines.add(getRuntimeDefines());

            mpDebugViewPass = ComputePass::create(mpDevice, desc, defines, true);
        }

        mpDebugViewPass->getProgram()->addDefines(getRuntimeDefines()); //Update Runtime defines

        auto var = mpDebugViewPass->getRootVar();

        uint3 dispatchDim = uint3(dstTexture->getWidth(), dstTexture->getHeight(), 1);

        //Determine color scales
        float dirGMColorScale = mOptions.debugDirGMScaleFactor * mOptions.guidingMapResolution * mOptions.guidingMapResolution;
        if (mTracePhotonNumberOfPhotonsLastFrame > 0)
            dirGMColorScale /= mTracePhotonNumberOfPhotonsLastFrame;
        float lightGMColorScale = mOptions.debugLightGMScaleFactor;
        lightGMColorScale /= mOptions.reservedPhotonsPerDirection * mOptions.guidingMapResolution * mOptions.guidingMapResolution;

        var["CB"]["gDispatchSize"] = dispatchDim.xy();
        var["CB"]["gSelectedLightIndex"] = mOptions.debugSelectedLightIndex;
        var["CB"]["gMinCountToCreateGM"] = mOptions.photonNeededForGM;
        var["CB"]["gScaleLightGM"] = lightGMColorScale;
        var["CB"]["gColorLightGM"] = mOptions.debugLightGMColor;
        var["CB"]["gScaleDirGM"] = dirGMColorScale;
        var["CB"]["gColorDirGM"] = mOptions.debugDirGMColor;
        var["CB"]["gMappingShowDirectionalGM"] = mOptions.debugMappingShowDirectionalGM;
        var["CB"]["gSelectLightModeColor"] = mOptions.debugSelectLightHelperModeColor;
        var["CB"]["gUseLightSelectModeHelper"] = mOptions.debugSelectLightHelperMode;

        var["gGuidingMapLight"] = mpGuidingMapLight;
        var["gGuidingMapDirection"] = mpGuidingMapsDirection;
        var["gMapLightToDirection"] = mpMapLightToDirectionPrev; //Previous is used, as ping pong swap happends at the end of update

        var["gDebug"] = dstTexture;

        mpDebugViewPass->execute(pRenderContext, dispatchDim);
    }

    void PhotonGuiding::clearResources()
    {
        mpGuidingMapLight = nullptr;
        mpGuidingMapsDirection = nullptr;
        mpHistogramLight = nullptr;
        mpHistogramDirection = nullptr;
        mpContributionDirection = nullptr;
        mpResourceCounter = nullptr;
        mpContributionLight = nullptr;
        mpHistogramDirectionPrev = nullptr;
        mpMapLightToDirection = nullptr;
        mpMapLightToDirectionPrev = nullptr;
        mpMapDirectionToLight = nullptr;
        mpReservedPhoton= nullptr;
    }

    void PhotonGuiding::prepareResources(RenderContext* pRenderContext)
    {
        //Ensure that Falcors emissive lights are up to date
        auto& pLights = mpScene->getLightCollection(pRenderContext);

        //Check if resources needs to be resetted
        if (pLights->getTotalLightCount() != mEmissiveLightCount || mpScene->getLightCount() != mAnalyticLightCount || mRebuildResources)
        {
            mEmissiveLightCount = pLights->getTotalLightCount();
            mAnalyticLightCount = mpScene->getLightCount();
            mTotalLightCount = mEmissiveLightCount + mAnalyticLightCount;

            clearResources();

            //If total lights > the threshold, enable mapping by default. Disable on rebuild
            if((mTotalLightCount > kAutomaticMappingCount) && !mRebuildResources)
            {
                mOptions.useMappingScheme = true;
                mOptions.useDynamicPMin = true;
            }

            mRebuildResources = false;
        }

        //Create Resources
        if (!mpGuidingMapLight)
        {
           float minSideLen = math::ceil(math::sqrt(float(mTotalLightCount)));  // ceiled pixel width/length
           mResolutionLightGM = uint(pow(2.f, math::ceil(math::log2(minSideLen)))); //Nearest power 2

           mpGuidingMapLight = Texture::create2D(
                mpDevice, mResolutionLightGM, mResolutionLightGM, ResourceFormat::R32Uint, 1u, Texture::kMaxPossible, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
           mpGuidingMapLight->setName("PhotonGuiding:GuidingMapsLight");
        }

        if (!mpGuidingMapsDirection)
        {
            uint numGuidingTextures = mOptions.useMappingScheme ? mOptions.mappingDirGMCount : mTotalLightCount;
            //Calc atlas texture size
            float minSideLen = math::ceil(math::sqrt(float(numGuidingTextures * mOptions.guidingMapResolution * mOptions.guidingMapResolution))); // ceiled pixel
            mResolutionDirGM = uint(pow(2.f, math::ceil(math::log2(minSideLen)))); //Nearest power 2
            mMipLevelsDirGM = uint(round(math::log2(float(mOptions.guidingMapResolution)))) + 1;

            mpGuidingMapsDirection = Texture::create2D(
                mpDevice, mResolutionDirGM, mResolutionDirGM, ResourceFormat::R32Uint, 1u, mMipLevelsDirGM, nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
            );
            mpGuidingMapsDirection->setName("PhotonGuiding:GuidingMapsDirection");
        }

        if (!mpHistogramLight)
        {
            mpHistogramLight = Texture::create2D(
                mpDevice, mResolutionLightGM, mResolutionLightGM, ResourceFormat::R32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
            mpHistogramLight->setName("PhotonGuiding:HistogramLight");
        }

        if(!mpHistogramDirection)
        {
            mpHistogramDirection = Texture::create2D(
                mpDevice, mResolutionDirGM, mResolutionDirGM, ResourceFormat::R32Float, 1u, 1u, nullptr,
                ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource
            );
            std::string name = "PhotonGuiding:HistogramDirection";
            if(mOptions.useMappingScheme)
                name += "_0";
            mpHistogramDirection->setName(name);
        }

        if (!mpContributionDirection)
        {
            uint mips = mOptions.useMappingScheme ? mMipLevelsDirGM : Texture::kMaxPossible;
            mpContributionDirection = Texture::create2D(
                    mpDevice, mResolutionDirGM, mResolutionDirGM, ResourceFormat::R32Uint, 1u, mips, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
            mpContributionDirection->setName("PhotonGuiding:ContributionDirection");
        }

        if ((mOptions.useDynamicPMin || mOptions.useMappingScheme) && !mpResourceCounter)
        {
            mpResourceCounter = Buffer::createStructured(mpDevice, sizeof(uint), 2,  ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, Buffer::CpuAccess::None, nullptr, false);
            mpResourceCounter->setName("PhotonGuiding:ResourceCounter");
        }

        if (mOptions.useMappingScheme) {
            if (!mpContributionLight)
            {
                mpContributionLight = Texture::create2D(
                    mpDevice, mResolutionLightGM, mResolutionLightGM, ResourceFormat::R32Uint, 1u, Texture::kMaxPossible, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess | ResourceBindFlags::RenderTarget
                );
                mpContributionLight->setName("PhotonGuiding:ContributionLight");
            }

            if (!mpHistogramDirectionPrev)
            {
                mpHistogramDirectionPrev = Texture::create2D(
                    mpDevice, mResolutionDirGM, mResolutionDirGM, ResourceFormat::R32Float, 1u, mMipLevelsDirGM, nullptr,
                    ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess
                );
                mpHistogramDirectionPrev->setName("PhotonGuiding:mpHistogramDirection_1");
            }

            if (!mpMapLightToDirection || !mpMapLightToDirectionPrev)
            {
                mpMapLightToDirection = Texture::create2D(mpDevice, mResolutionLightGM, mResolutionLightGM,
                    ResourceFormat::R16Uint, 1u,1u, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
                mpMapLightToDirection->setName("PhotonGuiding:MapLightToDirection_0");
                
                mpMapLightToDirectionPrev = Texture::create2D(mpDevice, mResolutionLightGM, mResolutionLightGM,
                    ResourceFormat::R16Uint, 1u,1u, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
                mpMapLightToDirectionPrev->setName("PhotonGuiding:MapLightToDirection_1");
            }

            if (!mpMapDirectionToLight)
            {
                ResourceFormat resourceFormat = mTotalLightCount > 0xFFFF ? ResourceFormat::R32Uint : ResourceFormat::R16Uint;
                mResolutionMap = mResolutionDirGM / mOptions.guidingMapResolution;
                mpMapDirectionToLight = Texture::create2D(mpDevice, mResolutionMap, mResolutionMap, resourceFormat, 1u, 1u,
                    nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
                mpMapDirectionToLight->setName("PhotonGuiding::MapDirectionToLight");
            }
        }

        if (mOptions.useDynamicPMin) {
            if (!mpReservedPhoton)
            {
                mpReservedPhoton = Texture::create2D(mpDevice, mResolutionLightGM, mResolutionLightGM, ResourceFormat::R16Uint,
                     1u, 1u,nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
                mpReservedPhoton->setName("PhotonGuiding:ReservedPhotonsPerLight");
            }
        }
    }

    void PhotonGuiding::reduceLoop(RenderContext* pRenderContext, ref<Texture>& pContributionTex, const uint startMipLevel, const uint dstMipLevel, const bool forceMipMapGen)
    {
        /* A loop for the reduce pass. Uses work group reduce if there are 5 or more mip levels left and uses a simple mip reduce for the remaining levels.
        * Could be optimized further, but current state is sufficently fast
        */

        auto var = mpReducePass->getRootVar();
        bool useMipReduce = forceMipMapGen;         //Mip reduce is used if remaining levels <5 or if forced
        uint increments = forceMipMapGen ? 1 : 5; //The optimized workgroup reduce can handle exactly 5 levels

        for(uint mip = startMipLevel; mip < dstMipLevel; mip += increments)
        {
             //If there are less than 5 levels left, switch to Mip based reduce
            if (!useMipReduce && (mip + increments) >= dstMipLevel)
            {
                increments = 1;
                useMipReduce = true;
            }
            uint dstMip = mip + increments;
            //Use half resolution of src mip level, as the four nearest samples are always summed
            uint3 dispatchDims = uint3(pContributionTex->getWidth(mip), pContributionTex->getHeight(mip), 2u) / 2u; 

            //Set shader resources
            var["CB"]["gDstSize"] = dispatchDims.xy();
            var["CB"]["gUseMip"] = useMipReduce;

            var["gSrc"].setSrv(pContributionTex->getSRV(mip, 1u));
            var["gDst"].setUav(pContributionTex->getUAV(dstMip, 0u, 1u));

            mpReducePass->execute(pRenderContext, dispatchDims);
        }
    }

    void PhotonGuiding::reduceContributionPass(RenderContext* pRenderContext) {
        FALCOR_PROFILE(pRenderContext, "ReduceContribution");

        //Initialize the shader
        if (!mpReducePass)
        {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderReduce).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            mpReducePass = ComputePass::create(mpDevice, desc, defines, true);
        }
                
        //Depending if the mapping scheme is used or not, different reductions are needed
        //If it is not used, the directional contribution is reduced to light and then total contribution
        //When the mapping scheme is used, only the light contribution needs to be reduced to total contribution
        if(!mOptions.useMappingScheme) //Directional Contribution 2 step reduce (direction->light->total)
        {
            const uint mipLevelMax = mpContributionDirection->getMipCount() - 1;
            const uint mipLevelLight = mMipLevelsDirGM-1;     //Mip level of the light contribution

            reduceLoop(pRenderContext, mpContributionDirection, 0, mipLevelLight); //(direction->light)
            reduceLoop(pRenderContext, mpContributionDirection, mipLevelLight, mipLevelMax); //(light->total)

        }else //Light Contribution 1 step reduce (light->total)
        {
            const uint mipLevelMax = mpContributionLight->getMipCount() - 1;
            reduceLoop(pRenderContext, mpContributionLight, 0, mipLevelMax); //(light->total)
        }        
    }

    void PhotonGuiding::updateHistogramsPass(RenderContext* pRenderContext, bool isDirectionalResource)
    {
        std::string profileName = "UpdateHistogram_";
        profileName += isDirectionalResource ? "Direction" : "Light";
        FALCOR_PROFILE(pRenderContext, profileName);

        //Shared runtime defines for Histogram and Guiding Map update
        auto getRuntimeDefines = [&]()
        {
            DefineList defines;
            defines.add("LIGHT_COUNT", std::to_string(mTotalLightCount));
            defines.add("USE_MAPPING", mOptions.useMappingScheme ? "1" : "0");
            defines.add("LIGHT_HISTOGRAM_SIZE", std::to_string(mResolutionLightGM));
            defines.add("MAP_TEXTURE_SIZE", std::to_string(mResolutionMap));
            return defines;
        };

        //Get Compute Pass
        uint passIndex = isDirectionalResource ? 1 : 0;
        ref<ComputePass>& pUpdateHistogramPass = mpUpdateHistogramsPass[passIndex];

        //Create Shader
        if (!pUpdateHistogramPass)
        {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderUpdateHistograms).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            defines.add("IS_DIRECTIONAL", isDirectionalResource ? "1" : "0");
            defines.add(getRuntimeDefines());
            
            pUpdateHistogramPass = ComputePass::create(mpDevice, desc, defines, true);
        }

        pUpdateHistogramPass->getProgram()->addDefines(getRuntimeDefines()); //Update defines
        auto var = pUpdateHistogramPass->getRootVar();

        //Set resources
        var["CB"]["gDispatchSize"] = isDirectionalResource ? mResolutionDirGM : mResolutionLightGM;
        var["CB"]["gIterationCount"] = mGuidingIterationCount;
        var["CB"]["gDirectionalResourceSize"] = mOptions.guidingMapResolution;
        var["CB"]["gExponentialMovingAverageFactor"] = mOptions.exponentialMovingAverageFactor;

        // Depending if mapping is used, different contribution textures are used
        // If mapping is disabled, light contribution is stored at a mipmap level of the directional contribution
        // else, a seperate resource is bound.
        if (mOptions.useMappingScheme)
        {
            if(isDirectionalResource)
            {
                var["gTotalContribution"].setSrv(mpContributionLight->getSRV(0,1u));
                var["gContribution"].setSrv(mpContributionDirection->getSRV(0,1u));
                var["gHistogram"] = mpHistogramDirection;

                var["gHistogramPrev"] = mpHistogramDirectionPrev;
                var["gMapDirectionGMToLightIndex"] = mpMapDirectionToLight;
                var["gMapLightIndexToDirectionalGMPrev"] = mpMapLightToDirectionPrev;
            }else
            {
                var["gTotalContribution"].setSrv(mpContributionLight->getSRV(mpContributionLight->getMipCount()-1,1u));
                var["gContribution"].setSrv(mpContributionLight->getSRV(0,1u));
                var["gHistogram"] = mpHistogramLight;
            }
        }
        else
        {
            if (isDirectionalResource)
            {
                var["gTotalContribution"].setSrv(mpContributionDirection->getSRV(mMipLevelsDirGM - 1,1u));
                var["gContribution"].setSrv(mpContributionDirection->getSRV(0,1u));
                var["gHistogram"] = mpHistogramDirection;
            }
            else
            {
                var["gTotalContribution"].setSrv(mpContributionDirection->getSRV(mpContributionDirection->getMipCount()-1,1u));
                var["gContribution"].setSrv(mpContributionDirection->getSRV(mMipLevelsDirGM - 1,1u));
                var["gHistogram"] = mpHistogramLight;
            }
        }
        
        uint3 dispatchIndex = isDirectionalResource ?
            uint3(mResolutionDirGM,mResolutionDirGM,1) :
            uint3(mResolutionLightGM,mResolutionLightGM,1);

        pUpdateHistogramPass->execute(pRenderContext, dispatchIndex);
    }

    void PhotonGuiding::updateGuidingMapsPass(RenderContext* pRenderContext, uint maxPhotonsDistributed, bool isDirectionalResource)
    {
        std::string profileName = "UpdateGuidingMap_";
        profileName += isDirectionalResource ? "Direction" : "Light";
        FALCOR_PROFILE(pRenderContext, profileName);

        //Shared runtime defines for Histogram and Guiding Map update
        auto getRuntimeDefines = [&]()
        {
            DefineList defines;
            defines.add("LIGHT_COUNT", std::to_string(mTotalLightCount));
            defines.add("RESERVED_PHOTONS_PER_CELL", isDirectionalResource ?
                std::to_string(mOptions.reservedPhotonsPerDirection) :
                std::to_string(mOptions.reservedPhotonsPerLight));
            defines.add("PHOTONS_NEEDED_FOR_DIR_GM", std::to_string(mOptions.photonNeededForGM));

            defines.add("USE_MAPPING", mOptions.useMappingScheme ? "1" : "0");
            defines.add("USE_DYNAMIC_PMIN", mOptions.useDynamicPMin ? "1" : "0");
            defines.add("MAPPING_PHOTONS_NEEDED_TO_CREATE", std::to_string(mOptions.mappingPhotonNeededToCreate));
            defines.add("MAP_TEXTURE_SIZE", std::to_string(mResolutionMap));
            defines.add("LIGHT_GM_SIZE", std::to_string(mResolutionLightGM));

            return defines;
        };

        //Get Compute Pass
        uint passIndex = isDirectionalResource ? 1 : 0;
        ref<ComputePass>& pUpdateGuidingMapsPass = mpUpdateGuidingMapsPass[passIndex];

        //Create Shader
        if (!pUpdateGuidingMapsPass)
        {
            Program::Desc desc;
            desc.addShaderLibrary(kShaderUpdateGuidingMaps).csEntry("main").setShaderModel(kShaderModel);

            DefineList defines;
            defines.add("IS_DIRECTIONAL", isDirectionalResource ? "1" : "0");
            defines.add("MAPPING_INVALID_INDEX", std::to_string(kMappingInvalidIndex));
            defines.add("COUNTER_MAPPING_INDEX", std::to_string(kCounterIndexMapping));
            defines.add("COUNTER_PMIN_INDEX", std::to_string(kCounterIndexDynamicPhoton));
            defines.add(getRuntimeDefines());
            
            pUpdateGuidingMapsPass = ComputePass::create(mpDevice, desc, defines, true);
        }

        pUpdateGuidingMapsPass->getProgram()->addDefines(getRuntimeDefines()); //Update defines
        auto var = pUpdateGuidingMapsPass->getRootVar();

        //Calculate the free photons for the light Guiding Map
        uint freePhotonsPerLight = maxPhotonsDistributed;
        if(!isDirectionalResource && !mOptions.useDynamicPMin)
        {
            freePhotonsPerLight = maxPhotonsDistributed - mOptions.reservedPhotonsPerLight * mTotalLightCount;
        }

         //Set resources
        var["CB"]["gDispatchSize"] = isDirectionalResource ? mResolutionDirGM : mResolutionLightGM;
        var["CB"]["gDirectionalResourceSize"] = mOptions.guidingMapResolution;
        var["CB"]["gFreePhotonsLight"] = freePhotonsPerLight;
        var["CB"]["gDynMinPhotonsPerLight"] = std::min(mOptions.dynamicPMinPhotonsMinMax.x, mOptions.dynamicPMinPhotonsMinMax.y); //Fallback min photons

        auto pGuidingMap = isDirectionalResource ? mpGuidingMapsDirection : mpGuidingMapLight;
        var["gHistogram"] = isDirectionalResource ? mpHistogramDirection : mpHistogramLight;
        var["gGuidingMap"] = pGuidingMap;
        if(isDirectionalResource)
            var["gGuidingMapLight"] = mpGuidingMapLight;

        //Bind Mapping Resources on the light pass
        if(mOptions.useMappingScheme )
        {
            var["gMapDirGMToLightIndex"] = mpMapDirectionToLight;
            if(!isDirectionalResource) //Light pass exclusive resources
            {
                var["gMapLightIndexToDirGMPrev"] = mpMapLightToDirectionPrev;
                var["gMapLightIndexToDirGM"] = mpMapLightToDirection;
                var["gCounter"] = mpResourceCounter;
            }
        }

        //Bind dynamic PMin resources
        if (mOptions.useDynamicPMin && !isDirectionalResource) {
            var["gCounter"] = mpResourceCounter;
            var["gPMinPerLight"] = mpReservedPhoton;
        }

        uint3 dispatchIndex = isDirectionalResource ?
            uint3(mResolutionDirGM,mResolutionDirGM,1) :
            uint3(mResolutionLightGM,mResolutionLightGM,1);

        pUpdateGuidingMapsPass->execute(pRenderContext, dispatchIndex);

        //Reuse the reduce loop to generate MipMaps for the Guiding Maps, that are used for top-down quadtree traversal
        reduceLoop(pRenderContext, pGuidingMap, 0, pGuidingMap->getMipCount()-1, true);

    }

    void PhotonGuiding::dynamicallyPMinPass(RenderContext* pRenderContext)
    {
        FALCOR_PROFILE(pRenderContext, "DynamicPMin");
        //Create Shader
        if (!mpDynamicPMinPass)
        {
            Program::Desc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShaderDynamicPMin).csEntry("main").setShaderModel(kShaderModel);
            desc.addTypeConformances(mpScene->getTypeConformances());

            DefineList defines;
            defines.add("LIGHT_COUNT", std::to_string(mTotalLightCount));
            defines.add("ANALYTIC_START_INDEX", std::to_string(mEmissiveLightCount));
            defines.add(mpScene->getSceneDefines());

            mpDynamicPMinPass = ComputePass::create(mpDevice, desc, defines, true);
        }

        //Set shader variables
        auto var = mpDynamicPMinPass->getRootVar();
        mpScene->setRaytracingShaderData(pRenderContext, var);

        var["CB"]["gMinDist"] = std::min(mOptions.dynamicPMinMinMaxDistance.x, mOptions.dynamicPMinMinMaxDistance.y);
        var["CB"]["gMaxDist"] = std::max(mOptions.dynamicPMinMinMaxDistance.x, mOptions.dynamicPMinMinMaxDistance.y);
        var["CB"]["gMinPhotons"] = std::min(mOptions.dynamicPMinPhotonsMinMax.x, mOptions.dynamicPMinPhotonsMinMax.y);
        var["CB"]["gMaxPhotons"] = std::max(mOptions.dynamicPMinPhotonsMinMax.x, mOptions.dynamicPMinPhotonsMinMax.y);
        var["CB"]["gLightGMSize"] = mResolutionLightGM;

        var["gReservedPhotonsPerLight"] = mpReservedPhoton;
        var["gResourceCounter"] = mpResourceCounter;

        mpDynamicPMinPass->execute(pRenderContext, uint3(mResolutionLightGM, mResolutionLightGM, 1));
    }
} //namespace Falcor
