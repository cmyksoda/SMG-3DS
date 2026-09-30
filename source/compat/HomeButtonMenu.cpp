// The Wii's HOME Button Menu, which the game loads as a module of PowerPC code (HomeButtonMenuWrapperRSO.rso) and
// never could run here. On the 3DS the HOME button belongs to the system, which suspends the game, and the port's pad
// never reports the Remote's HOME press, so the game's menu stays closed and there is nothing to load.
#include <Game/System/HomeButtonMenuWrapper.hpp>
#include <revolution/os.h>

void RSO::setupRsoHomeButtonMenu() {}
void RSO::HBMCreate(const HBMDataInfo*) {}
void RSO::HBMInit() {}
void RSO::HBMSetAdjustFlag(int) {}
void RSO::HBMStartBlackOut() {}

// Only an open menu runs these, which would mean the game found a HOME press the port never gives it.
void RSO::HBMCalc(const HBMControllerData*) {
    OSPanic(__FILE__, __LINE__, "the Wii's HOME Button Menu opened, which the 3DS's HOME button replaces");
}
void RSO::HBMDraw() {
    OSPanic(__FILE__, __LINE__, "the Wii's HOME Button Menu drew, which the 3DS's HOME button replaces");
}
HBMSelectBtnNum RSO::HBMGetSelectBtnNum() {
    OSPanic(__FILE__, __LINE__, "the Wii's HOME Button Menu was asked for a choice, which the 3DS's HOME button replaces");
    return HBM_SELECT_NULL;
}
