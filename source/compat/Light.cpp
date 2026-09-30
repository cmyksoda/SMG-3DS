#include <smg/Scene.hpp>
#include <smg/Bcsv.hpp>
#include <Game/Map/LightDataHolder.hpp>
#include <Game/Map/LightFunction.hpp>
#include <Game/Map/LightZoneDataHolder.hpp>
#include <Game/Util/StringUtil.hpp>
#include <cstdio>

// LightDirector's part of the scene: it owns the light and zone data holders and the archive they read.
namespace smg {
void NativeScene::initLights(std::unique_ptr<Bcsv> data, std::vector<std::pair<std::string, std::unique_ptr<Bcsv>>> zones) {
    mLightTable = std::move(data);
    mZoneLightTables = std::move(zones);
    mLightData = new LightDataHolder();
    mLightData->initLightData();
    mLightZones = new LightZoneDataHolder();
    mLightZones->initZoneData();
}

// The holders have no destructors; their names point into the tables, which go after them.
void NativeScene::forgetLights() {
    if (mLightZones) {
        for (s32 i = 0; i < mLightZones->mCount; ++i) delete[] mLightZones->mZoneInfo[i].mAreaInfo;
        delete[] mLightZones->mZoneInfo;
        delete mLightZones;
    }
    if (mLightData) {
        delete[] mLightData->mLights;
        delete mLightData;
    }
    mLightZones = nullptr;
    mLightData = nullptr;
}

const JMapInfo* NativeScene::lightData() const {
    return mLightTable ? &mLightTable->info() : nullptr;
}

// "Light%s.bcsv", which the archive stores in lowercase.
const JMapInfo* NativeScene::lightTable(const char* zone) const {
    const std::string name = std::string("light") + zone + ".bcsv";
    for (const auto& [path, table] : mZoneLightTables)
        if (MR::isEqualStringCase(path.c_str(), name.c_str())) return &table->info();
    return nullptr;
}

const AreaLightInfo* NativeScene::defaultAreaLight() const {
    if (!mLightData) return nullptr;
    return mLightData->findAreaLight(mLightZones->getAreaLightNameInZoneData(ZoneLightID()));
}
} // namespace smg

// tryCreateCsvParser on the director's LightData.arc; the tables belong to the scene, not to each caller.
s32 LightFunction::createLightDataParser(JMapInfo** pOut) {
    const JMapInfo* info = smg::NativeScene::current().lightData();
    *pOut = const_cast<JMapInfo*>(info);
    return info && info->mData ? info->mData->mNumEntries : 0;
}

s32 LightFunction::createZoneDataParser(const char* pZone, JMapInfo** pOut) {
    const JMapInfo* info = smg::NativeScene::current().lightTable(pZone);
    *pOut = const_cast<JMapInfo*>(info);
    return info && info->mData ? info->mData->mNumEntries : 0;
}

const char* LightFunction::getDefaultAreaLightName() {
    LightDataHolder* holder = smg::NativeScene::current().lightDataHolder();
    if (!holder) {
        std::fputs("LightFunction: the scene's lights were asked for before they were loaded\n", stderr);
        std::abort();
    }
    return holder->getDefaultAreaLightName();
}
