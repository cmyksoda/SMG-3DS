#include <smg/Stage.hpp>
#include <cstdio>
#include <cstdlib>
#include <map>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: stage_tool EXTRACTED_FILES_DIRECTORY GALAXY SCENARIO\n");
        return 2;
    }
    smg::Stage stage;
    std::string error;
    if (!smg::loadStage(argv[1], argv[2], std::atoi(argv[3]), stage, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (stage.entrance)
        std::printf("Start (%g, %g, %g) facing (%g, %g, %g)\n", stage.start[0], stage.start[1], stage.start[2],
            stage.startRotation[0], stage.startRotation[1], stage.startRotation[2]);
    else
        std::printf("No MarioNo 0 entrance in this scenario's layers\n");
    std::map<std::string, int> zones;
    for (const auto& object : stage.objects) {
        ++zones[object.zone];
        std::printf("%-24s %-28s (%9.1f, %9.1f, %9.1f)\n", object.zone.c_str(), object.name.c_str(),
            object.transform[3], object.transform[7], object.transform[11]);
    }
    for (const auto& [zone, count] : zones) std::printf("%4d objects in %s\n", count, zone.c_str());
    return 0;
}
