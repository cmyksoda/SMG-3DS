#pragma once
// KCollisionServer::checkArrow and KCHitArrow exactly as Petari has them (src/Game/Map/KCollision.cpp), as free functions over a server:
// the oracle the port's faster ray cast (source/compat/KCollisionArrow.cpp) must match bit for bit.
#include <Game/Map/KCollision.hpp>
#include <Game/Util/MathUtil.hpp>

namespace {
namespace petari {
inline bool hitArrow(const KCollisionServer& self, KC_PrismData* pPrism, const TVec3f& rOrigin, const TVec3f& rDir, f32* pDist, u8* pFlag);

inline KC_PrismData* checkArrow(const KCollisionServer& self, const TVec3f& rOrigin, const TVec3f& rDir, f32* pDists, u8* pFlags, u32* pCount,
                                KC_PrismData** pOut, u32 maxCount) {
    if (rDir.x == 0.0f && rDir.y == 0.0f && rDir.z == 0.0f) {
        return nullptr;
    }

    f32 length;
    TVec3f dir(rDir);
    f32 startT = 0.0f;
    MR::separateScalarAndDirection(&length, &dir, dir);

    if (MR::isNearZero(dir)) {
        return nullptr;
    }

    TVec3f hitPoint;
    KCollisionServer::V3u cell;
    TVec3f start(rOrigin);
    start.x -= self.mFile->mMin.x;
    start.y -= self.mFile->mMin.y;
    start.z -= self.mFile->mMin.z;

    cell.setUsingCast(start);

    if (self.isInsideMinMaxInLocalSpace(cell)) {
        hitPoint.set(start);
    } else {
        TVec3f boxMax;
        boxMax.x = (f32)(u32)~self.mFile->mXMask;
        boxMax.y = (f32)(u32)~self.mFile->mYMask;
        boxMax.z = (f32)(u32)~self.mFile->mZMask;

        if (dir.x != 0.0f) {
            f32 edge = 0.0f < dir.x ? 0.0f : boxMax.x;
            startT = (edge - start.x) / dir.x;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (self.isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        if (dir.y != 0.0f) {
            f32 edge = 0.0f < dir.y ? 0.0f : boxMax.y;
            startT = (edge - start.y) / dir.y;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (self.isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        if (dir.z != 0.0f) {
            f32 edge = 0.0f < dir.z ? 0.0f : boxMax.z;
            startT = (edge - start.z) / dir.z;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (self.isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        return nullptr;
    }

searchStart:
    u16* prismList;
    u32 foundCount;
    foundCount = 0;

    s32 shift;
    s32 deltaPosX;
    s32 deltaPosY;
    s32 deltaPosZ;
    s32 deltaNegX;
    s32 deltaNegY;
    s32 deltaNegZ;
    s32* pDeltaX;
    s32* pDeltaY;
    s32* pDeltaZ;
    s32 stepX;
    s32 stepY;
    s32 stepZ;
    if (dir.x < 0.0f) {
        pDeltaX = &deltaNegX;
        stepX = -1;
    } else {
        pDeltaX = &deltaPosX;
        stepX = 1;
    }

    if (dir.y < 0.0f) {
        pDeltaY = &deltaNegY;
        stepY = -1;
    } else {
        pDeltaY = &deltaPosY;
        stepY = 1;
    }

    if (dir.z < 0.0f) {
        pDeltaZ = &deltaNegZ;
        stepZ = -1;
    } else {
        pDeltaZ = &deltaPosZ;
        stepZ = 1;
    }

    KC_PrismData* bestPrism;
    f32 accumT = startT;

    bestPrism = nullptr;

    do {
        prismList = reinterpret_cast< u16* >(self.searchBlock(&shift, cell.x, cell.y, cell.z));
        u32 blockSize = 1 << shift;
        u32 mask = blockSize - 1;

        deltaPosX = blockSize - (cell.x & mask);
        deltaPosY = blockSize - (cell.y & mask);
        deltaPosZ = blockSize - (cell.z & mask);
        deltaNegX = -(s32)(cell.x & mask);
        deltaNegY = -(s32)(cell.y & mask);
        deltaNegZ = -(s32)(cell.z & mask);

        if (*pDeltaX == 0) {
            *pDeltaX = stepX;
        }

        if (*pDeltaY == 0) {
            *pDeltaY = stepY;
        }

        if (*pDeltaZ == 0) {
            *pDeltaZ = stepZ;
        }

        f32 bestFraction = 1.0f;
        f32 dist = 1.0f;

        while (*++prismList != 0) {
            KC_PrismData* prism = &self.mFile->mPrisms[*prismList];

            if (prism->mHeight <= 0.0f) {
                continue;
            }

            u8 flag = 0;

            if (!hitArrow(self, prism, rOrigin, rDir, &dist, &flag)) {
                continue;
            }

            if (pOut != nullptr) {
                pDists[foundCount] = dist;
                pOut[foundCount] = prism;
                foundCount++;

                if (dist < bestFraction) {
                    bestFraction = dist;
                    bestPrism = prism;
                }

                if (foundCount == maxCount) {
                    if (pCount != nullptr) {
                        *pCount = foundCount;
                    }

                    return bestPrism;
                }
            } else {
                if (!(dist < bestFraction)) {
                    continue;
                }

                bestFraction = dist;
                *pDists = dist;
                bestPrism = prism;
                *pFlags = flag;
            }
        }

        if (pOut == nullptr && bestPrism != nullptr) {
            break;
        }

        f32 tX = MR::isNearZero(dir.x) ? 1.0e9f : (f32)*pDeltaX / dir.x;
        f32 tY = MR::isNearZero(dir.y) ? 1.0e9f : (f32)*pDeltaY / dir.y;
        f32 tZ = MR::isNearZero(dir.z) ? 1.0e9f : (f32)*pDeltaZ / dir.z;

        f32 tMin = tX;

        if (tY < tMin) {
            tMin = tY;
        }

        if (tZ < tMin) {
            tMin = tZ;
        }

        if (length - accumT <= tMin) {
            break;
        }

        TVec3f step(dir);
        step.scale(tMin);
        hitPoint += step;
        accumT += tMin;

        cell.setUsingCast(hitPoint);

        if (!self.isInsideMinMaxInLocalSpace(cell)) {
            break;
        }
    } while (accumT < length);

    if (pCount != nullptr) {
        *pCount = foundCount;
    }

    return bestPrism;
}


inline bool hitArrow(const KCollisionServer& self, KC_PrismData* pPrism, const TVec3f& rOrigin, const TVec3f& rDir, f32* pDist, u8* pFlag) {
    TVec3f* v0 = &self.mFile->mPos[pPrism->mPositionIndex];
    TVec3f* faceNormal = &self.mFile->mNorms[pPrism->mNormalIndex];

    TVec3f rel;
    PSVECSubtract(rOrigin, v0, rel);

    f32 t = PSVECDotProduct(rel, faceNormal);

    if (t <= 0.0f) {
        *pFlag = 0;
        return false;
    }

    f32 dirDotFace = PSVECDotProduct(faceNormal, rDir);

    if (0.0f < t + dirDotFace) {
        *pFlag = 0;
        return false;
    }

    t = t / -dirDotFace;

    TVec3f hit = rDir * t;
    hit += rel;

    bool onEdge0 = false;
    bool onEdge1 = false;
    bool onEdge2 = false;

    f32 e0 = PSVECDotProduct(hit, self.mFile->mNorms[pPrism->mEdgeIndices[0]]);

    if (0.01f < e0) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e0 && e0 <= 0.01f) {
        onEdge0 = true;
    }

    f32 e1 = PSVECDotProduct(hit, self.mFile->mNorms[pPrism->mEdgeIndices[1]]);

    if (0.01f < e1) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e1 && e1 <= 0.01f) {
        onEdge1 = true;
    }

    f32 e2 = PSVECDotProduct(hit, self.mFile->mNorms[pPrism->mEdgeIndices[2]]);

    if (0.01f + pPrism->mHeight < e2) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e2 && e2 <= 0.01f) {
        onEdge2 = true;
    }

    *pDist = t;

    if (onEdge0) {
        if (onEdge1) {
            if (onEdge2) {
                *pFlag = 1;
            } else {
                *pFlag = 5;
            }
        } else {
            if (onEdge2) {
                *pFlag = 7;
            } else {
                *pFlag = 2;
            }
        }
    } else {
        if (onEdge1) {
            if (onEdge2) {
                *pFlag = 6;
            } else {
                *pFlag = 3;
            }
        } else {
            if (onEdge2) {
                *pFlag = 4;
            } else {
                *pFlag = 1;
            }
        }
    }

    return true;
}
}  // namespace petari
}  // namespace
