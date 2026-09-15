#pragma once
#include "Utils/Math/MathConstants.slangh"

import Scene.RaytracingInline; //TODO this is a bit overkill
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.MathHelpers;

//Global define that allows compliation even if shader defies are not set
#ifndef PHOTON_GUIDING_ENABLED
#define PHOTON_GUIDING_ENABLED 0
#endif

//A sample created from the Guiding Map
struct PhotonGuidingSample
{
    uint lightIndex;           //Light Index
    float photonFluxDivFactor; // Scale factor the flux needs to be divided by, as guiding changes the probabilities

    float2 directionUV;        // Random value in [0,1] for directional sampling

    bool isLightGuiding;       // True if light guiding was used
    uint _pad;
};

//A photon sampled from the scene
struct PhotonGuidingPhotonLightSample
{
    float3 posW;
    float normalX;
    float3 dirW;
    float normalY;
    float3 flux;
    float normalZ;

    property float3 normal {
        get { return float3(normalX, normalY, normalZ); }
        set {
            normalX = newValue.x;
            normalY = newValue.y;
            normalZ = newValue.z;
        }
    }
};

struct PhotonGuiding
{
    #if PHOTON_GUIDING_ENABLED
    static const uint kMappingInvalidIndex = 0xFFFF; //TODO move? 
    static const uint kDiscretizedContributionFactor = PHOTON_GUIDING_DISCRETIZED_CONTRIBUTION_FACTOR;
    static const uint kDiscretizedContributionMax = PHOTON_GUIDING_DISCRETIZED_CONTRIBUTION_MAX;
    static const bool kEnableMapping = PHOTON_GUIDING_USE_MAPPED_GM;

    // Gen Sample
    static const uint kTotalLightCount = PHOTON_GUIDING_TOTAL_LIGHT_COUNT;
    static const uint kTraversalBlockSize = PHOTON_GUIDING_TRAVERSAL_BLOCK_SIZE;
    static const uint kLightGuidingMapMaxMip = PHOTON_GUIDING_LIGHT_GM_MAX_MIP;
    static const uint kDirectionGuidingMapMaxMip = PHOTON_GUIDING_DIRECTION_GM_MAX_MIP;
    static const uint kMinPhotonsForDirectionalGuiding = PHOTON_GUIDING_MIN_PHOTONS_FOR_DIR_GUIDING;
    static const uint kAnalyticLightIndexOffset = PHOTON_GUIDING_ANALYTIC_LIGHT_OFFSET;
    static const uint kMapTextureSize = PHOTON_GUIDING_MAP_TEXTURE_SIZE;
    #endif

    uint gResolutionLightGM;        // Resolution for the light Contribution/Guiding Map
    uint gMapTextureResolution;    // Number of directional resources per row if mapping is used
    uint gResolutionGM;             // Resolution of a single directional guiding resource

    RWTexture2D<uint> gContributionLight;           //Contribution for each light. Can be nullptr
    RWTexture2D<uint> gContributionDirection;       // Contribution for all directions
    Texture2D<uint> gMapLightToDirection;           //Mapping from light index -> directional resource, or invalid. Can be nullptr

    Texture2D<uint> gGuidingMapLight;       //Light Guiding Map
    Texture2D<uint> gGuidingMapsDirection;  //Atlas with the directional Guiding Maps

    struct TraversalSample {
        uint2 pixel;        //Selected pixel of the traversal sample
        uint photonCount;   //Photon count of the selected cell
    };
};


//"Public" Functions
extension PhotonGuiding
{
    inline uint2 guidingMapUVToPixel(float2 guidingUV)
    {
    #if PHOTON_GUIDING_ENABLED
        return uint2(floor(guidingUV * gResolutionGM));
    #else
        return uint2(0);
    #endif
    }

    /* Applies random subpixel jitter to a local guiding map and returns the local UV coordinates
       \param[in] localPixel: Local pixel within a directional guiding map
       \param[in] randomVal: Random variable in [0,1]
       \return UV coordinates with subpixel jitter
    */
    inline float2 getUVWithSubpixelJitter(uint2 localPixel, float2 randomVal)
    {
    #if PHOTON_GUIDING_ENABLED
        float2 uvMin = float2(localPixel) / gResolutionGM;     // UV of top-left of the choosen pixel
        float2 uvMax = float2(localPixel + 1) / gResolutionGM; // UV of bottom-right of the choosen pixel
        return (uvMin + (uvMax - uvMin) * randomVal);       // Final uv with random jitter within the pixel
    #else
        return float2(0);
    #endif
    }

    /*  Records guiding contribution of a photon
        \param[in] lightIndex: Index of the light the photon was dispatched from
        \param[in] guidingPixel: Local pixel coordinates of the directional GM the sample was generated from
        \param[in] contribution: Contribution of the sample
    */
    void addGuidingContribution(uint lightIndex, uint2 guidingPixel, float contribution)
    {
    #if PHOTON_GUIDING_ENABLED
        //Early out
        if (contribution <= 0)
            return;

        // Discretize the contribution, as an atomic add is used to count the contrition at each cell
        uint discretizedContribution = uint(round(kDiscretizedContributionFactor * contribution));
        discretizedContribution = clamp(discretizedContribution, 1, kDiscretizedContributionMax); //Clamp

        // If the mapping is enabled, most lights will not have a directional Guiding Map,
        // therefore the contribution needs to be recorded per light. Else, light contribution can be inferred
        // from the directional contribution.
        // Additionally, if the mapping is valid, the guiding pixel needs to be offset by the mapping
        // index, as the input guiding pixel is stored in local pixel coordinates when mapping is enabled.
        uint2 lightCell = uint2(lightIndex % gResolutionLightGM, lightIndex / gResolutionLightGM);
        if (kEnableMapping)
        {
            //Get cell pixel and add contribution
            InterlockedAdd(gContributionLight[lightCell], discretizedContribution);

            // Check if mapping is valid and return if it is invalid
            uint mapIdx = gMapLightToDirection[lightCell];
            if (mapIdx >= kMappingInvalidIndex)
                return;

            // Offset guiding pixel with the mapping index
            guidingPixel += uint2(mapIdx % gMapTextureResolution, mapIdx / gMapTextureResolution) * gResolutionGM;
        } else //Calculate atlas offset for the pixels
        {
            guidingPixel += lightCell * gResolutionGM;
        }

        // Record the directional contribution
        InterlockedAdd(gContributionDirection[guidingPixel], discretizedContribution);
    #endif
    }

    /* Samples a light and direction using the Guiding Maps
    */
    bool sampleGuidedLightAndDirection(uint2 pixel, uint dispatchDimX, float2 randomVal, out PhotonGuidingSample pgs)
    {
        pgs = {};
    #if PHOTON_GUIDING_ENABLED
        if (kTotalLightCount == 0)
            return false;

        // Get 1D index for traversal. For increased locality of samples, we linearize in blocks
        // Note dispatchSize of the shader should be a multiple of kTraversalBlockSize
        uint2 blockPixel = pixel % kTraversalBlockSize; //Local block index
        uint2 blockID = pixel / kTraversalBlockSize;    // 2D-ID for the block
        uint localBlockIndex = blockPixel.x + blockPixel.y * kTraversalBlockSize;
        uint blockOffset = blockID.x * (kTraversalBlockSize * kTraversalBlockSize) + blockID.y * dispatchDimX * kTraversalBlockSize; // Note one block size cancels out in the added term
        uint photonID = localBlockIndex + blockOffset; //Unique photon ID for traversal

        // Get max photons from the highest mip of the light guiding map
        uint maxPhotons = gGuidingMapLight.Load(uint3(0, 0, kLightGuidingMapMaxMip));

        //Abort early if photon is not distributed by the guiding map
        if (photonID >= maxPhotons)
            return false;

        // Top-Down quadtree traversal for the light
        TraversalSample lightTS = topDownMipMapTraversal(gGuidingMapLight, photonID, kLightGuidingMapMaxMip);

        // Get 1D light index from the mip index after traversal
        pgs.lightIndex = lightTS.pixel.x + lightTS.pixel.y * gResolutionLightGM;

        // Check, if the light has enough photons to use directional guiding
        bool useDirectionalGuiding = lightTS.photonCount >= kMinPhotonsForDirectionalGuiding;
        //useDirectionalGuiding = false;

        if (kEnableMapping)
        {
            uint mapIndex = gMapLightToDirection[lightTS.pixel];
            //Check if mapping is valid
            if (mapIndex < kMappingInvalidIndex)
            {
                lightTS.pixel = uint2(mapIndex % kMapTextureSize, mapIndex / kMapTextureSize); // Light Index -> Map Index
            } else //Mapping invalid
            {
                useDirectionalGuiding = false; 
            }
        }


        // Check if the current photon is directionally guided or should be distributed randomly
        // The discrepancy between light GM photon count and directional GM photon count is due to the
        // lower bound approximations for the photon count in each cell
        if (useDirectionalGuiding)
        {
            float dirGMTotalPhotons = gGuidingMapsDirection.Load(uint3(lightTS.pixel, kDirectionGuidingMapMaxMip));
            useDirectionalGuiding = photonID < dirGMTotalPhotons;
        }

        // If the directional Guiding Map is valid, traverse to get the corresponding directional cell
        if (useDirectionalGuiding)
        {
            TraversalSample dirTS = topDownMipMapTraversal(gGuidingMapsDirection, photonID, kDirectionGuidingMapMaxMip, lightTS.pixel);

            // Create photon sample from the selected cell
            uint2 localIndex = dirTS.pixel % gResolutionGM; // Local index
            pgs.directionUV = getUVWithSubpixelJitter(localIndex, randomVal);

            // Divide factor for the lights flux. Total photon count cancels out, as a photon is divided by the number of light paths
            pgs.photonFluxDivFactor = float(dirTS.photonCount) * gResolutionGM * gResolutionGM;
        } else //Use a random direction instead to create the sample
        {
            pgs.directionUV = randomVal;
            pgs.photonFluxDivFactor = float(lightTS.photonCount); // Total Photon count cancels out, as a photon is divided by the number of light paths
        }

        pgs.isLightGuiding = !useDirectionalGuiding;

        return true;
    #else
        return false;
    #endif
    }

    bool createPhotonSample(PhotonGuidingSample pgs, inout SampleGenerator sg , out PhotonGuidingPhotonLightSample pls)
    {
        pls = {};
    #if PHOTON_GUIDING_ENABLED
        // Is the light emissive or analytic?
        bool isAnalytic = pgs.lightIndex >= kAnalyticLightIndexOffset;

        float3 woLocal = float3(0);

        if (isAnalytic) //Point or Spot
        {
            // Get scenes light data with the analytic index
            const uint analyticLightIndex = pgs.lightIndex - kAnalyticLightIndexOffset;
            LightData lightData = gScene.getLight(analyticLightIndex);
            // Only Point (and Spot) lights are supported
            if (lightData.type != uint(LightType::Point))
                return false;

            float spotAngle = lightData.cosOpeningAngle;
            // Depending on the opening angle, the light is a spot or point light
            if (lightData.openingAngle < M_2PI) //Spot Light
            {
                // TODO support penumbra
                woLocal = sample_cone(pgs.directionUV, spotAngle);
            } else // Point Light
            {
                spotAngle = 0; //Just to be sure
                woLocal = sample_sphere(pgs.directionUV);
            }
            float invLightPdf = M_2PI * (1.0 - spotAngle);

            if (invLightPdf <= 0.f)
                return false;

            pls.normal = lightData.dirW;
            pls.posW = lightData.posW;
            pls.flux = lightData.intensity * invLightPdf;
        } else //Emissive Triangle
        {
            // Get emissive triangle data
            const EmissiveTriangle tri = gScene.lightCollection.getTriangle(pgs.lightIndex);
            const float3 barycentrics = sample_triangle(sampleNext2D(sg)); //Position on the triangle is random
            pls.posW = tri.getPosition(barycentrics);
            pls.normal = tri.normal;

            // Sample emissive material of the triangle
            float2 uv = tri.getTexCoord(barycentrics);
            pls.flux = gScene.materials.evalEmissive(tri.materialID, uv) * tri.area;

            pls.posW = computeRayOrigin(pls.posW, pls.normal); // Offset as this light is real geometry

            // Sample direction in a cosine lobe
            float lightDirPDF = 1.0;
            woLocal = sample_cosine_hemisphere_concentric(pgs.directionUV, lightDirPDF);
            if (lightDirPDF <= 0.) //Invalid direction
                return false;
            pls.flux *= M_PI; // Lambert Emitter
        }

        // Convert direction local -> world
        float3 bitangent = perp_stark(pls.normal);
        float3 tangent = cross(bitangent, pls.normal);
        pls.dirW = woLocal.x * tangent + woLocal.y * bitangent + woLocal.z * pls.normal;

        // Apply flux scale factor
        pls.flux /= pgs.photonFluxDivFactor;
        return true;
    #else
        return false;
    #endif
    }
}

//"Private" Functions
extension PhotonGuiding
{
    #if PHOTON_GUIDING_ENABLED
    /* Top-Down Quadtree traversal using mipmaps.
    */
    TraversalSample topDownMipMapTraversal(Texture2D<uint> tex, inout uint photonID, const uint maxMipLevel, uint2 mipIndex = uint2(0))
    {
        // Get the light index by traversing (top-down quadtree traversal) the light Guiding Map
        //uint2 mipIndex = uint2(0);     // Current traversal index (start top left of 2x2 block)
        uint selectedCellPhotons = 0;  // Photons for the cell after traversal

        // Start traversal at 2nd highest mip level (with size 2x2)
        [unroll]
        for (int mipLevel = maxMipLevel - 1; mipLevel >= 0; mipLevel--)
        {
            // Update Mip index to one level deeper. Corresponds to top-left of the 2x2 block
            mipIndex *= 2;

            // Fetch samples in 2x2 block
            uint4 photonCells;
            photonCells[0] = tex.Load(uint3(mipIndex.x, mipIndex.y, mipLevel));         // Top-Left
            photonCells[1] = tex.Load(uint3(mipIndex.x + 1, mipIndex.y, mipLevel));     // Top-Right
            photonCells[2] = tex.Load(uint3(mipIndex.x, mipIndex.y + 1, mipLevel));     // Bottom-Left
            photonCells[3] = tex.Load(uint3(mipIndex.x + 1, mipIndex.y + 1, mipLevel)); // Bottom-Right

            // Select one of the four cells using the photonID
            uint sumPhotons = 0;   // Sum of looked at cells
            uint selectedCell = 0; // in [0,3]; uses Z-Curve pattern
            for (; selectedCell < 4; selectedCell++)
            {
                uint sumWithCell = sumPhotons + photonCells[selectedCell];
                if (photonID < sumWithCell)
                    break;
                sumPhotons = sumWithCell;
            }

            // Update photonID for next iteration by subtracting all not choosen looked at photon cells
            photonID -= sumPhotons;

            // Set mipIndex to choosen cell
            if (selectedCell == 1 || selectedCell == 3) // Left offset (1,3)
                mipIndex.x += 1;
            if (selectedCell > 1) // Bottom offset (2,3)
                mipIndex.y += 1;

            // End of traversal reached, store cell photon count
            if (mipLevel == 0)
                selectedCellPhotons = photonCells[selectedCell];
        }
        TraversalSample ts;
        ts.pixel = mipIndex;
        ts.photonCount = selectedCellPhotons;
        return ts;
    }
    #endif
}

// Global Bound Block
ParameterBlock<PhotonGuiding> gPhotonGuiding;
