#include "MaterialGraphDocument.h"

namespace material_tool
{
MaterialGraphDocument MaterialGraphDocument::WeatheredStone()
{
    MaterialGraphDocument d; std::string error;
    const auto stone = d.AddMaterialLayer("Stone Base", error);
    const auto dirt = d.AddMaterialLayer("Dirt Layer", error);
    const auto moss = d.AddMaterialLayer("Moss Layer", error);
    const auto connect = [&](std::uint32_t from, std::size_t output, std::uint32_t to, std::size_t input) {
        d.Connect(PinId(from, output), PinId(to, input), error, true);
    };
    const auto parameter = [&](const char* name, float value, std::array<float, 2> at) {
        const auto id = d.AddNode(NodeKind::ScalarParameter, at);
        auto& node = *d.FindNode(id); node.parameter = name; node.name = name; node.value[0] = value; return id;
    };
    const auto texture = [&](const char* name, std::array<float,4> fallback, bool srgb, std::array<float,2> at) {
        const auto input = d.AddNode(NodeKind::TextureParameter, at), sample = d.AddNode(NodeKind::TextureSample, {at[0]+300,at[1]});
        auto& node = *d.FindNode(input); node.parameter = name; node.name = name; node.value = fallback;
        d.FindNode(sample)->srgb = srgb; connect(input,0,sample,0); return sample;
    };
    const auto source = [&](std::uint32_t id) { return d.FindLayer(id)->sourceNode; };
    d.FindNode(source(stone))->settings[0] = .8f;
    d.FindNode(source(dirt))->value = {.12f,.075f,.035f,1}; d.FindNode(source(dirt))->settings[0] = .95f;
    d.FindNode(source(moss))->value = {.055f,.13f,.025f,1}; d.FindNode(source(moss))->settings[0] = .88f;

    const auto tiling = d.AddNode(NodeKind::Vector2Parameter, {-1200,-600});
    d.FindNode(tiling)->parameter = "StoneTiling"; d.FindNode(tiling)->name = "Stone Tiling"; d.FindNode(tiling)->value = {2,2,0,0};
    const auto uv = d.AddNode(NodeKind::TilingOffset, {-800,-600}); connect(tiling,0,uv,1);
    const auto albedo = texture("StoneBaseColor", {.6f,.58f,.56f,1}, true, {-1200,-300});
    const auto normal = texture("StoneNormal", {.5f,.5f,1,1}, false, {-1200,0});
    const auto height = texture("StoneHeight", {0,0,0,1}, false, {-1200,300});
    connect(uv,3,albedo,1); connect(uv,3,normal,1); connect(uv,3,height,1);
    connect(albedo,2,source(stone),0); connect(height,3,source(stone),6);
    const auto strength = parameter("StoneNormalStrength",1,{-800,300});
    const auto normalStrength = d.AddNode(NodeKind::NormalStrength, {-400,0});
    connect(normal,7,normalStrength,0); connect(strength,0,normalStrength,1); connect(normalStrength,2,source(stone),4);

    const auto cavity = d.AddNode(NodeKind::Cavity, {-1200,600}), ao = d.AddNode(NodeKind::AO, {-1200,800});
    const auto occlusion = d.AddNode(NodeKind::MaskInvert, {-900,800}); connect(ao,0,occlusion,0);
    const auto recess = d.AddNode(NodeKind::MaskCombine, {-600,600}); d.FindNode(recess)->settings[0]=1;
    connect(cavity,0,recess,0); connect(occlusion,1,recess,1);
    const auto dirtNoise = d.AddNode(NodeKind::Noise, {-900,1100}); d.FindNode(dirtNoise)->value={2,4,.55f,19}; d.FindNode(dirtNoise)->settings[0]=1;
    const auto dirtNoiseMask = d.AddNode(NodeKind::MaskCombine, {-300,700}); connect(recess,2,dirtNoiseMask,0); connect(dirtNoise,2,dirtNoiseMask,1);
    const auto dirtContrast = parameter("DirtContrast",1,{-300,1100});
    const auto contrast = d.AddNode(NodeKind::MaskContrast, {0,700}); connect(dirtNoiseMask,2,contrast,0); connect(dirtContrast,0,contrast,1);
    const auto dirtAmount = parameter("DirtAmount",1,{0,1100});
    const auto dirtMask = d.AddNode(NodeKind::Multiply, {300,900}); connect(contrast,2,dirtMask,0); connect(dirtAmount,0,dirtMask,1);
    d.SetMaterialLayerMask(dirt, PinId(dirtMask,2), error);

    const auto slope = d.AddNode(NodeKind::SlopeMask, {-1200,1500}); d.FindNode(slope)->value={25,65,0,0};
    const auto threshold = parameter("MossSlopeThreshold",25,{-1500,1750}); connect(threshold,0,slope,1);
    const auto up = d.AddNode(NodeKind::MaskInvert, {-900,1500}); connect(slope,3,up,0);
    const auto mossScale = parameter("MossScale",2,{-1500,2050}), mossSeed = parameter("MossSeed",23,{-1500,2300});
    const auto noise = d.AddNode(NodeKind::Noise, {-1200,2050}); d.FindNode(noise)->value={2,4,.55f,23}; d.FindNode(noise)->settings[0]=1;
    connect(mossScale,0,noise,1); connect(mossSeed,0,noise,3);
    const auto growth = d.AddNode(NodeKind::MaskCombine, {-600,1550}); connect(up,1,growth,0); connect(noise,2,growth,1);
    const auto cavityGrowth = d.AddNode(NodeKind::Multiply, {-600,2000}); d.FindNode(cavityGrowth)->value[1]=.15f; connect(cavity,0,cavityGrowth,0);
    const auto coverage = d.AddNode(NodeKind::MaskCombine, {-300,1500}); d.FindNode(coverage)->settings[0]=1;
    connect(growth,2,coverage,0); connect(cavityGrowth,2,coverage,1);
    const auto amount = parameter("MossAmount",1,{-300,1900});
    const auto mask = d.AddNode(NodeKind::Multiply, {0,1500}); connect(coverage,2,mask,0); connect(amount,0,mask,1);
    d.SetMaterialLayerMask(moss,PinId(mask,2),error);
    const auto mossNormal = texture("MossNormal",{.5f,.5f,1,1},false,{-1200,2600}); connect(mossNormal,7,source(moss),4);
    return d;
}
}
