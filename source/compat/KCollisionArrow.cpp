// KCollisionServer's ray cast: Petari's checkArrow and KCHitArrow (left out of the adapted KCollision.cpp) with the same arithmetic in
// the same order, so every result and every write to the caller's arrays is the same to the bit. Only the work around it differs.
#include "Game/Map/KCollision.hpp"
#include "Game/Util/MathUtil.hpp"
#include <cstdint>
#include <cstring>

namespace {
// KCHitArrow for one prism. A cast that collects hits never reads the classification, so only a first-hit cast works it out.
template < bool Classify >
inline __attribute__((always_inline)) bool hitArrow(const KCLFile* file, const KC_PrismData* pPrism, f32 ox, f32 oy, f32 oz, f32 dx, f32 dy,
                                                    f32 dz, f32* pDist, u8* pFlag) {
    const TVec3f* v0 = &file->mPos[pPrism->mPositionIndex];
    const TVec3f* faceNormal = &file->mNorms[pPrism->mNormalIndex];

    const f32 rx = ox - v0->x, ry = oy - v0->y, rz = oz - v0->z;
    f32 t = rx * faceNormal->x + ry * faceNormal->y + rz * faceNormal->z;

    if (t <= 0.0f) {
        if (Classify) {
            *pFlag = 0;
        }
        return false;
    }

    const f32 dirDotFace = faceNormal->x * dx + faceNormal->y * dy + faceNormal->z * dz;

    if (0.0f < t + dirDotFace) {
        if (Classify) {
            *pFlag = 0;
        }
        return false;
    }

    t = t / -dirDotFace;

    const f32 hx = dx * t + rx, hy = dy * t + ry, hz = dz * t + rz;

    const TVec3f* edge0 = &file->mNorms[pPrism->mEdgeIndices[0]];
    const f32 e0 = hx * edge0->x + hy * edge0->y + hz * edge0->z;

    if (0.01f < e0) {
        if (Classify) {
            *pFlag = 0;
        }
        return false;
    }

    const TVec3f* edge1 = &file->mNorms[pPrism->mEdgeIndices[1]];
    const f32 e1 = hx * edge1->x + hy * edge1->y + hz * edge1->z;

    if (0.01f < e1) {
        if (Classify) {
            *pFlag = 0;
        }
        return false;
    }

    const TVec3f* edge2 = &file->mNorms[pPrism->mEdgeIndices[2]];
    const f32 e2 = hx * edge2->x + hy * edge2->y + hz * edge2->z;

    if (0.01f + pPrism->mHeight < e2) {
        if (Classify) {
            *pFlag = 0;
        }
        return false;
    }

    *pDist = t;

    if (Classify) {
        const bool onEdge0 = 0.0f <= e0 && e0 <= 0.01f;
        const bool onEdge1 = 0.0f <= e1 && e1 <= 0.01f;
        const bool onEdge2 = 0.0f <= e2 && e2 <= 0.01f;

        if (onEdge0) {
            *pFlag = onEdge1 ? (onEdge2 ? 1 : 5) : (onEdge2 ? 7 : 2);
        } else {
            *pFlag = onEdge1 ? (onEdge2 ? 6 : 3) : (onEdge2 ? 4 : 1);
        }
    }

    return true;
}

// The block of the octree that holds a cell: searchBlock, with the file's fields read once per cast.
struct Octree {
    const u8* root;
    s32 widthShift, xyShift, xShift;
    bool single;
    s32 xMask, yMask, zMask;

    explicit Octree(const KCLFile* file)
        : root(static_cast< const u8* >(file->mOctree)), widthShift(file->mBlockWidthShift), xyShift(file->mBlockXYShift),
          xShift(file->mBlockXShift), single(xyShift == -1 && xShift == -1), xMask(file->mXMask), yMask(file->mYMask), zMask(file->mZMask) {
    }

    bool contains(u32 x, u32 y, u32 z) const {
        return (x & xMask) == 0 && (y & yMask) == 0 && (z & zMask) == 0;
    }

    // The zero-terminated prism list one u16 before its first entry, and the block's shift.
    const u16* find(u32 x, u32 y, u32 z, s32& shift) const {
        const u8* octree = root;
        shift = widthShift;
        s32 offset = 0;

        if (!single) {
            offset = (((z >> widthShift) << xyShift) | ((y >> widthShift) << xShift) | (x >> widthShift)) * 4;
        }

        while ((offset = *reinterpret_cast< const s32* >(octree + offset)) >= 0) {
            octree += offset;
            shift--;
            offset = ((((z >> shift) & 1) << 2) | (((y >> shift) & 1) << 1) | ((x >> shift) & 1)) * 4;
        }

        return reinterpret_cast< const u16* >(octree + (offset & 0x7FFFFFFF));
    }
};

// A prism is listed in every block it touches, so the ray meets it again in the next block, always with the same answer. A slot holds
// the last prism asked about and where the cast recorded its hit (position plus one), or zero for a miss.
class Met {
public:
    static constexpr u32 Slots = 128;

    Met() {
        std::memset(mPrism, 0, sizeof mPrism);
    }

    u32 slot(u16 prism) const {
        return prism & (Slots - 1);
    }

    bool knows(u32 slot, u16 prism) const {
        return mPrism[slot] == prism;
    }

    u16 recorded(u32 slot) const {
        return mHit[slot];
    }

    void miss(u32 slot, u16 prism) {
        mPrism[slot] = prism;
        mHit[slot] = 0;
    }

    void hit(u32 slot, u16 prism, u32 position) {
        if (position < 0xFFFF) {
            mPrism[slot] = prism;
            mHit[slot] = static_cast< u16 >(position + 1);
        }
    }

private:
    u16 mPrism[Slots];
    u16 mHit[Slots];
};

// The walk from the starting block to the end of the ray's length. A cast that collects (pOut) goes through every block; the first-hit
// one stops after the first block with a hit.
template < bool Collect >
KC_PrismData* walk(const KCollisionServer& server, const TVec3f& rOrigin, const TVec3f& rDir, const TVec3f& dir, f32 length, f32 startT,
                   const TVec3f& hitPoint, const KCollisionServer::V3u& startCell, f32* pDists, u8* pFlags, u32* pCount, KC_PrismData** pOut,
                   u32 maxCount) {
    const KCLFile* file = server.mFile;
    const Octree octree(file);
    KC_PrismData* prisms = file->mPrisms;
    const f32 ox = rOrigin.x, oy = rOrigin.y, oz = rOrigin.z;
    const f32 rdx = rDir.x, rdy = rDir.y, rdz = rDir.z;
    const f32 dirX = dir.x, dirY = dir.y, dirZ = dir.z;

    const bool nearX = MR::isNearZero(dirX);
    const bool nearY = MR::isNearZero(dirY);
    const bool nearZ = MR::isNearZero(dirZ);
    const bool backX = dirX < 0.0f;
    const bool backY = dirY < 0.0f;
    const bool backZ = dirZ < 0.0f;
    const s32 stepX = backX ? -1 : 1;
    const s32 stepY = backY ? -1 : 1;
    const s32 stepZ = backZ ? -1 : 1;

    f32 hx = hitPoint.x, hy = hitPoint.y, hz = hitPoint.z;
    u32 cx = startCell.x, cy = startCell.y, cz = startCell.z;
    f32 accumT = startT;
    KC_PrismData* bestPrism = nullptr;
    u32 foundCount = 0;
    Met met;

    do {
        s32 shift;
        const u16* list = octree.find(cx, cy, cz, shift);
        const u32 blockSize = 1u << shift;
        const u32 mask = blockSize - 1;

        s32 deltaX = backX ? -static_cast< s32 >(cx & mask) : static_cast< s32 >(blockSize - (cx & mask));
        s32 deltaY = backY ? -static_cast< s32 >(cy & mask) : static_cast< s32 >(blockSize - (cy & mask));
        s32 deltaZ = backZ ? -static_cast< s32 >(cz & mask) : static_cast< s32 >(blockSize - (cz & mask));

        if (deltaX == 0) {
            deltaX = stepX;
        }

        if (deltaY == 0) {
            deltaY = stepY;
        }

        if (deltaZ == 0) {
            deltaZ = stepZ;
        }

        f32 bestFraction = 1.0f;
        f32 dist = 1.0f;

        while (*++list != 0) {
            const u16 index = *list;
            KC_PrismData* prism = &prisms[index];

            if (prism->mHeight <= 0.0f) {
                continue;
            }

            const u32 slot = met.slot(index);

            if (Collect) {
                if (met.knows(slot, index)) {
                    const u16 recorded = met.recorded(slot);

                    if (recorded == 0) {
                        continue;
                    }

                    dist = pDists[recorded - 1];
                } else {
                    if (!hitArrow< false >(file, prism, ox, oy, oz, rdx, rdy, rdz, &dist, nullptr)) {
                        met.miss(slot, index);
                        continue;
                    }

                    met.hit(slot, index, foundCount);
                }

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
                if (met.knows(slot, index)) {
                    continue;
                }

                u8 flag = 0;

                if (!hitArrow< true >(file, prism, ox, oy, oz, rdx, rdy, rdz, &dist, &flag)) {
                    met.miss(slot, index);
                    continue;
                }

                if (!(dist < bestFraction)) {
                    continue;
                }

                bestFraction = dist;
                *pDists = dist;
                bestPrism = prism;
                *pFlags = flag;
            }
        }

        if (!Collect && bestPrism != nullptr) {
            break;
        }

        const f32 tX = nearX ? 1.0e9f : static_cast< f32 >(deltaX) / dirX;
        const f32 tY = nearY ? 1.0e9f : static_cast< f32 >(deltaY) / dirY;
        const f32 tZ = nearZ ? 1.0e9f : static_cast< f32 >(deltaZ) / dirZ;

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

        hx = hx + dirX * tMin;
        hy = hy + dirY * tMin;
        hz = hz + dirZ * tMin;
        accumT += tMin;

        cx = static_cast< s32 >(hx);
        cy = static_cast< s32 >(hy);
        cz = static_cast< s32 >(hz);

        if (!octree.contains(cx, cy, cz)) {
            break;
        }
    } while (accumT < length);

    if (pCount != nullptr) {
        *pCount = foundCount;
    }

    return bestPrism;
}
}  // namespace

KC_PrismData* KCollisionServer::checkArrow(const TVec3f& rOrigin, const TVec3f& rDir, f32* pDists, u8* pFlags, u32* pCount, KC_PrismData** pOut,
                                           u32 maxCount) const {
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
    V3u cell;
    TVec3f start(rOrigin);
    start.x -= mFile->mMin.x;
    start.y -= mFile->mMin.y;
    start.z -= mFile->mMin.z;

    cell.setUsingCast(start);

    if (isInsideMinMaxInLocalSpace(cell)) {
        hitPoint.set(start);
    } else {
        TVec3f boxMax;
        boxMax.x = (f32)(u32)~mFile->mXMask;
        boxMax.y = (f32)(u32)~mFile->mYMask;
        boxMax.z = (f32)(u32)~mFile->mZMask;

        bool entered = false;

        if (dir.x != 0.0f) {
            f32 edge = 0.0f < dir.x ? 0.0f : boxMax.x;
            startT = (edge - start.x) / dir.x;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                entered = isInsideMinMaxInLocalSpace(cell);
            }
        }

        if (!entered && dir.y != 0.0f) {
            f32 edge = 0.0f < dir.y ? 0.0f : boxMax.y;
            startT = (edge - start.y) / dir.y;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                entered = isInsideMinMaxInLocalSpace(cell);
            }
        }

        if (!entered && dir.z != 0.0f) {
            f32 edge = 0.0f < dir.z ? 0.0f : boxMax.z;
            startT = (edge - start.z) / dir.z;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                entered = isInsideMinMaxInLocalSpace(cell);
            }
        }

        if (!entered) {
            return nullptr;
        }
    }

    if (pOut != nullptr) {
        return walk< true >(*this, rOrigin, rDir, dir, length, startT, hitPoint, cell, pDists, pFlags, pCount, pOut, maxCount);
    }

    return walk< false >(*this, rOrigin, rDir, dir, length, startT, hitPoint, cell, pDists, pFlags, pCount, pOut, maxCount);
}

bool KCollisionServer::KCHitArrow(KC_PrismData* pPrism, const TVec3f& rOrigin, const TVec3f& rDir, f32* pDist, u8* pFlag) const {
    return hitArrow< true >(mFile, pPrism, rOrigin.x, rOrigin.y, rOrigin.z, rDir.x, rDir.y, rDir.z, pDist, pFlag);
}
