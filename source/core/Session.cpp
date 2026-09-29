#include <smg/Session.hpp>
#include <Game/LiveActor/Nerve.hpp>
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <Game/Util/StringUtil.hpp>

namespace smg {
namespace SessionNerves {
NEW_NERVE(Boot, Session, Boot);
NEW_NERVE(Waiting, Session, Waiting);
}
Session::Session(const char* dataRoot) : NerveExecutor("SMG-3DS"), dataRoot_(dataRoot ? dataRoot : "") {
    initNerve(&SessionNerves::Boot::sInstance);
}
void Session::exeBoot() {
    if (!dataRoot_.empty()) {
        std::vector<std::uint8_t> storage;
        std::vector<ArchiveEntry> entries;
        std::string error;
        const std::string path = dataRoot_ + "/LayoutData/StarPieceCounter.arc";
        // Bootstrap scratch space, released before rendering; not a whole-game memory estimate.
        constexpr std::size_t bootstrapArchiveBudget = 16u * 1024 * 1024;
        if (readArchive(path.c_str(), storage, entries, bootstrapArchiveBudget, error))
            assetStatus_ = "Star-bit archive: " + std::to_string(entries.size()) + " resources";
        else assetStatus_ = error;

        const std::string scenarioPath = dataRoot_ + "/StageData/GoodEggGalaxy/GoodEggGalaxyScenario.arc";
        if (readArchive(scenarioPath.c_str(), storage, entries, bootstrapArchiveBudget, error)) {
            for (const auto& entry : entries) {
                if (!MR::isEqualStringCase(entry.path.c_str(), "ScenarioData.bcsv")) continue;
                Bcsv scenarios;
                if (scenarios.load({storage.data()+entry.offset, entry.size},error)) {
                    const auto& info = scenarios.info();
                    assetStatus_ = "Good Egg: " + std::to_string(info.getNumEntries()) + " scenario records";
                    s32 firstScenario = 0;
                    if (info.getNumEntries() > 0 && info.getValue(0, "ScenarioNo", &firstScenario))
                        assetStatus_ += " / first " + std::to_string(firstScenario);
                } else assetStatus_ = error;
                break;
            }
        }
    }
    booted_ = true;
    setNerve(&SessionNerves::Waiting::sInstance);
}
void Session::exeWaiting() {}
} // namespace smg
