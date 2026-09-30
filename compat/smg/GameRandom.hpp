#pragma once
#include <JSystem/JMath/random.hpp>

// GameSystemObjHolder's generator, owned natively until the game's system objects run.
JMath::TRandom_fast_& smgGameRandom();
