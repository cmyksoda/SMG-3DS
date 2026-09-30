#include <smg/GameRandom.hpp>

JMath::TRandom_fast_& smgGameRandom() {
    static JMath::TRandom_fast_ random(0); // GameSystemObjHolder seeds it with zero
    return random;
}
