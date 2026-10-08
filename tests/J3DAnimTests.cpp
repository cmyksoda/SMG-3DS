#include <smg/AnimFast.hpp>
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DJointTree.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/JMath/JMath.hpp>
#include <revolution/mtx.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// The animation step must match the code it replaced bit for bit: each routine is compared on random data with a plain copy
// of its old self.
namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct Rng {
    std::uint32_t s;
    std::uint32_t next() { return s ^= s << 13, s ^= s >> 17, s ^= s << 5; }
    unsigned below(unsigned n) { return next() % n; }
    bool chance(unsigned percent) { return below(100) < percent; }
    float range(float lo, float hi) { return lo + (hi - lo) * ((next() >> 8) * (1.0f / 16777216.0f)); }
    float value() {
        switch (below(12)) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return 1.0f;
        case 3: return -1.0f;
        case 4: return range(-1000.0f, 1000.0f);
        default: return range(-3.0f, 3.0f);
        }
    }
};

bool sameBits(const void* a, const void* b, std::size_t bytes) { return std::memcmp(a, b, bytes) == 0; }
struct Mat { Mtx m; };
bool sameInfo(const J3DTransformInfo& a, const J3DTransformInfo& b) {
    return sameBits(&a.mScale, &b.mScale, sizeof a.mScale) && sameBits(&a.mRotation, &b.mRotation, sizeof a.mRotation) &&
           sameBits(&a.mTranslate, &b.mTranslate, sizeof a.mTranslate);
}

void randomMatrix(Rng& r, Mtx m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = r.value();
}

// PSMTXConcat as the port first wrote it.
void referenceConcat(const Mtx a, const Mtx b, Mtx out) {
    float p[3][4];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            p[row][col] = a[row][0] * b[0][col] + a[row][1] * b[1][col] + a[row][2] * b[2][col] + (col == 3 ? a[row][3] : 0);
    std::memcpy(out, p, sizeof(p));
}

void concatenation() {
    Rng r{12345};
    bool all = true;
    for (int i = 0; i < 2000; ++i) {
        Mtx a, b, expected, got, copy;
        randomMatrix(r, a), randomMatrix(r, b);
        referenceConcat(a, b, expected);
        PSMTXConcat(a, b, got);
        all &= sameBits(expected, got, sizeof(Mtx));
        smgMTXConcatTee(a, b, got, copy);
        all &= sameBits(expected, got, sizeof(Mtx)) && sameBits(expected, copy, sizeof(Mtx));
        // in place, as the joint traversal calls it: the product replaces a, and b becomes its copy
        Mtx inPlaceA, inPlaceB;
        std::memcpy(inPlaceA, a, sizeof(Mtx)), std::memcpy(inPlaceB, b, sizeof(Mtx));
        smgMTXConcatTee(inPlaceA, inPlaceB, inPlaceA, inPlaceB);
        all &= sameBits(expected, inPlaceA, sizeof(Mtx)) && sameBits(expected, inPlaceB, sizeof(Mtx));
        std::memcpy(inPlaceA, a, sizeof(Mtx)), std::memcpy(inPlaceB, b, sizeof(Mtx));
        PSMTXConcat(inPlaceA, inPlaceB, inPlaceB);
        all &= sameBits(expected, inPlaceB, sizeof(Mtx));
        std::memcpy(inPlaceA, a, sizeof(Mtx));
        PSMTXConcat(inPlaceA, inPlaceA, inPlaceA);
        referenceConcat(a, a, expected);
        all &= sameBits(expected, inPlaceA, sizeof(Mtx));
    }
    check(all, "the concatenations are PSMTXConcat's arithmetic, in and out of place");
}

// J3DMtxBuffer::calcWeightEnvelopeMtx as it was: a product for every mix, summed in a temporary.
void referenceEnvelopes(J3DMtxBuffer& buffer) {
    J3DJointTree* tree = buffer.mJointTree;
    const u16* indices = tree->getWEvlpMixMtxIndex();
    const f32* weights = tree->getWEvlpMixWeight();
    for (u16 i = 0; i < tree->getWEvlpMtxNum(); i++) {
        u8* pScale = &buffer.mpEvlpScaleFlagArr[i];
        *pScale = 1;
        f32 sum[3][4] = {};
        const u8 mixNum = tree->getWEvlpMixMtxNum(i);
        for (u8 j = 0; j < mixNum; j++) {
            const u16 idx = *indices++;
            const f32 weight = *weights++;
            Mtx product;
            referenceConcat(buffer.mpAnmMtx[idx], tree->getInvJointMtx(idx), product);
            for (int row = 0; row < 3; row++)
                for (int column = 0; column < 4; column++) sum[row][column] += product[row][column] * weight;
            *pScale &= buffer.mpScaleFlagArr[idx];
        }
        std::memcpy(buffer.mpWeightEvlpMtx[i], sum, sizeof(sum));
    }
}

void envelopes() {
    Rng r{777};
    bool all = true;
    for (int scene = 0; scene < 60; ++scene) {
        const unsigned joints = scene % 3 == 0 ? 70 + r.below(30) : 1 + r.below(20);  // past 64 joints, the cached ones run out
        const unsigned count = 1 + r.below(30);
        std::vector<u8> mixNum(count);
        unsigned total = 0;
        for (unsigned i = 0; i < count; ++i) total += mixNum[i] = r.chance(8) ? 0 : 1 + r.below(4);
        std::vector<u16> index(total + 1);
        std::vector<f32> weight(total + 1);
        const unsigned pool = r.chance(50) ? 1 + r.below(4) : joints;
        for (unsigned i = 0; i < total; ++i) {
            index[i] = r.below(pool < joints ? pool : joints);
            weight[i] = r.chance(15) ? (r.chance(50) ? 0.0f : 1.0f) : r.range(-0.5f, 1.0f);
        }
        std::vector<Mat> inverse(joints), anm(joints), envelope(count), expected(count);
        std::vector<u8> scale(joints), envScale(count), expectedScale(count);
        for (unsigned i = 0; i < joints; ++i) randomMatrix(r, inverse[i].m), randomMatrix(r, anm[i].m), scale[i] = r.below(2);
        J3DJointTree tree;
        tree.mWEvlpMtxNum = count;
        tree.mWEvlpMixMtxNum = mixNum.data();
        tree.mWEvlpMixMtxIndex = index.data();
        tree.mWEvlpMixWeight = weight.data();
        tree.mInvJointMtx = &inverse.data()->m;
        J3DMtxBuffer buffer;
        buffer.mJointTree = &tree;
        buffer.mpAnmMtx = &anm.data()->m;
        buffer.mpScaleFlagArr = scale.data();
        buffer.mpWeightEvlpMtx = &envelope.data()->m;
        buffer.mpEvlpScaleFlagArr = envScale.data();
        std::memset(envelope.data(), 0xA5, count * sizeof(Mat));
        buffer.calcWeightEnvelopeMtx();
        buffer.mpWeightEvlpMtx = &expected.data()->m;
        buffer.mpEvlpScaleFlagArr = expectedScale.data();
        referenceEnvelopes(buffer);
        all &= sameBits(envelope.data(), expected.data(), count * sizeof(Mat)) && sameBits(envScale.data(), expectedScale.data(), count);
    }
    check(all, "the envelope matrices are the weighted sums of each mix's product, with every joint's product made once");
}

// J3DGetKeyFrameInterpolation as it was: a search over the whole table every time.
template < typename T >
f32 referenceKey(f32 frame, const J3DAnmKeyTableBase* table, const T* data) {
    if (frame < data[0]) return data[1];
    const unsigned stride = table->mType == 0 ? 3 : 4;
    const unsigned last = (table->mMaxFrame - 1) * stride;
    if (data[last] <= frame) return data[last + 1];
    unsigned remaining = table->mMaxFrame;
    while (remaining > 1) {
        const unsigned half = remaining >> 1;
        if (frame >= data[half * stride]) {
            data += half * stride;
            remaining -= half;
        } else {
            remaining = half;
        }
    }
    if (stride == 3) return JMAHermiteInterpolation(frame, data[0], data[1], data[2], data[3], data[4], data[5]);
    return JMAHermiteInterpolation(frame, data[0], data[1], data[3], data[4], data[5], data[6]);
}

void referenceTransform(const J3DAnmTransformKey& anm, f32 frame, u16 joint, J3DTransformInfo* out) {
    const J3DAnmTransformKeyTable* e[3] = {&anm.mAnmTable[joint * 3], &anm.mAnmTable[joint * 3 + 1], &anm.mAnmTable[joint * 3 + 2]};
    f32* scale[3] = {&out->mScale.x, &out->mScale.y, &out->mScale.z};
    s16* rotation[3] = {&out->mRotation.x, &out->mRotation.y, &out->mRotation.z};
    f32* translate[3] = {&out->mTranslate.x, &out->mTranslate.y, &out->mTranslate.z};
    for (int a = 0; a < 3; ++a) {
        const J3DAnmKeyTableBase& s = e[a]->mScaleInfo;
        *scale[a] = s.mMaxFrame == 0 ? 1.0f : s.mMaxFrame == 1 ? anm.mScaleData[s.mOffset] : referenceKey(frame, &s, &anm.mScaleData[s.mOffset]);
        const J3DAnmKeyTableBase& q = e[a]->mRotationInfo;
        *rotation[a] = q.mMaxFrame == 0 ? 0 : q.mMaxFrame == 1 ? anm.mRotData[q.mOffset] * (1 << anm.mDecShift)
                                                               : (int)referenceKey(frame, &q, &anm.mRotData[q.mOffset]) * (1 << anm.mDecShift);
        const J3DAnmKeyTableBase& t = e[a]->mTranslateInfo;
        *translate[a] = t.mMaxFrame == 0 ? 0.0f : t.mMaxFrame == 1 ? anm.mTransData[t.mOffset] : referenceKey(frame, &t, &anm.mTransData[t.mOffset]);
    }
}

void keys() {
    Rng r{4242};
    bool all = true;
    for (int scene = 0; scene < 40; ++scene) {
        const unsigned joints = 1 + r.below(4);
        std::vector<J3DAnmTransformKeyTable> table(joints * 3);
        std::vector<f32> scaleData, transData;
        std::vector<s16> rotData;
        for (auto& t : table) {
            J3DAnmKeyTableBase* b[3] = {&t.mScaleInfo, &t.mRotationInfo, &t.mTranslateInfo};
            for (int c = 0; c < 3; ++c) {
                const unsigned count = r.chance(15) ? r.below(2) : 2 + r.below(r.chance(30) ? 40 : 6);
                const unsigned type = r.chance(85) ? 0 : 1;
                b[c]->mMaxFrame = count, b[c]->mType = type;
                b[c]->mOffset = c == 0 ? scaleData.size() : c == 1 ? rotData.size() : transData.size();
                int time = r.chance(70) ? 0 : (int)r.below(8) - 3;
                const bool onHalf = r.chance(10);
                for (unsigned k = 0; k < count; ++k) {
                    for (unsigned part = 0; part < (type == 0 ? 3u : 4u); ++part) {
                        if (c == 1) rotData.push_back((s16)(part == 0 ? time : r.below(65536)));
                        else (c == 0 ? scaleData : transData).push_back(part == 0 ? time + (onHalf ? 0.5f : 0.0f) : r.value());
                    }
                    time += r.chance(7) ? 0 : 1 + r.below(r.chance(20) ? 9 : 3);  // equal neighbors occur on the disc
                }
            }
        }
        scaleData.push_back(0), rotData.push_back(0), transData.push_back(0);
        J3DAnmTransformKey anm;
        anm.mAnmTable = table.data();
        anm.mScaleData = scaleData.data(), anm.mRotData = rotData.data(), anm.mTransData = transData.data();
        anm.mDecShift = r.below(4);
        std::vector<u16> hints(joints * 9);  // kept between calls, as a player keeps them, and left stale when the frames jump
        f32 frame = r.chance(20) ? -2.0f : (f32)r.below(20);
        const f32 rates[] = {1.0f, 1.0f, 0.5f, 2.0f, -1.0f, 0.3f};
        f32 rate = rates[r.below(6)];
        for (int step = 0; step < 80; ++step) {
            if (r.chance(5)) frame = (f32)r.below(120) - (r.chance(10) ? 8.0f : 0.0f);
            if (r.chance(5)) rate = rates[r.below(6)];
            if (r.chance(4)) for (auto& hint : hints) hint = (u16)r.next();  // another animation's, as when a player swaps them
            frame += rate;
            for (unsigned j = 0; j < joints; ++j) {
                J3DTransformInfo expected, hinted, plain;
                std::memset(&expected, 0, sizeof expected), std::memset(&hinted, 0, sizeof hinted), std::memset(&plain, 0, sizeof plain);
                referenceTransform(anm, frame, j, &expected);
                anm.calcTransformHinted(frame, j, &hinted, &hints[j * 9]);
                anm.calcTransform(frame, j, &plain);
                all &= sameInfo(expected, hinted) && sameInfo(expected, plain);
            }
        }
    }
    check(all, "a key animation's channels are the same whether found by hint, by the plain call or by the old search");
}

// J3DJoint::recursiveCalc as it was, on the same joints.
void referenceTraversal(J3DJoint* joint) {
    Mtx currentMtx;
    Vec currentScale;
    Vec parentScale;
    J3DJointCallBack callback;
    J3DMtxCalc* previousCalc = NULL;
    PSMTXCopy(J3DSys::mCurrentMtx, currentMtx);
    currentScale = J3DSys::mCurrentS;
    parentScale = J3DSys::mParentS;
    J3DMtxCalc* calc = joint->mMtxCalc;
    if (calc != NULL) {
        previousCalc = J3DJoint::mCurrentMtxCalc;
        J3DMtxCalc::setJoint(joint);
        J3DJoint::mCurrentMtxCalc = calc;
        calc->calc();
    } else if (J3DJoint::mCurrentMtxCalc != NULL) {
        J3DMtxCalc::setJoint(joint);
        J3DJoint::mCurrentMtxCalc->calc();
    }
    callback = joint->mCallBack;
    if (callback != NULL) callback(joint, 0);
    if (joint->mChild != NULL) referenceTraversal(joint->mChild);
    PSMTXCopy(currentMtx, J3DSys::mCurrentMtx);
    J3DSys::mCurrentS = currentScale;
    J3DSys::mParentS = parentScale;
    if (previousCalc != NULL) J3DJoint::mCurrentMtxCalc = previousCalc;
    if (callback != NULL) callback(joint, 1);
    if (joint->mYounger != NULL) referenceTraversal(joint->mYounger);
}

// What a joint's calc does to the traversal: its matrix times the parent's, kept in the buffer and carried to its children.
struct StubCalc : J3DMtxCalc {
    std::vector<Mat>* local;
    J3DMtxBuffer* buffer;
    void init(const Vec&, const Mtx&) override {}
    void calc() override {
        const int j = getJoint()->getJntNo();
        J3DSys::mCurrentS.x *= 1.0f + 0.01f * j;
        J3DSys::mParentS.y += 0.5f;
        PSMTXConcat(J3DSys::mCurrentMtx, (*local)[j].m, J3DSys::mCurrentMtx);
        PSMTXCopy(J3DSys::mCurrentMtx, buffer->mpAnmMtx[j]);
    }
};

std::vector<unsigned> trace;
J3DMtxBuffer* callbackBuffer = nullptr;
int tracing(J3DJoint* joint, int timing) {
    trace.push_back(joint->mJntNo * 2 + timing);
    for (int i = 0; i < 3; ++i) trace.push_back(*reinterpret_cast< const unsigned* >(&J3DSys::mCurrentMtx[i][3]));
    if (joint->mJntNo % 2 == 1) J3DSys::mCurrentMtx[1][3] += 0.75f;
    callbackBuffer->mpAnmMtx[joint->mJntNo][2][3] += J3DSys::mCurrentMtx[0][3];
    return 1;
}

void traversal() {
    Rng r{99};
    bool all = true;
    for (int scene = 0; scene < 80; ++scene) {
        const unsigned count = 1 + r.below(scene % 4 == 0 ? 40 : 10);
        std::vector<J3DJoint*> joint(count);
        for (auto& j : joint) j = new J3DJoint();
        const unsigned shape = r.below(3);  // a chain, a mix, or anything
        for (unsigned i = 1; i < count; ++i)
            joint[shape == 0 ? i - 1 : shape == 1 && r.chance(60) ? i - 1 : r.below(i)]->appendChild(joint[i]);
        std::vector<Mat> local(count), anmA(count), anmB(count);
        for (unsigned i = 0; i < count; ++i) {
            randomMatrix(r, local[i].m);
            joint[i]->mJntNo = i;
            if (r.chance(15)) joint[i]->mCallBack = tracing;
        }
        J3DMtxBuffer buffer;
        StubCalc calc;
        calc.local = &local, calc.buffer = &buffer;
        joint[0]->mMtxCalc = &calc;
        Mtx base;
        randomMatrix(r, base);
        Mtx finalMtx[2];
        Vec finalScale[2], finalParent[2];
        std::vector<unsigned> traces[2];
        for (int version = 0; version < 2; ++version) {
            buffer.mpAnmMtx = version == 0 ? &anmA.data()->m : &anmB.data()->m;
            callbackBuffer = &buffer;
            std::memset(buffer.mpAnmMtx, 0, count * sizeof(Mat));
            std::memcpy(J3DSys::mCurrentMtx, base, sizeof(Mtx));
            J3DSys::mCurrentS = {1.0f, 1.0f, 1.0f};
            J3DSys::mParentS = {1.0f, 1.0f, 1.0f};
            J3DMtxCalc::setMtxBuffer(&buffer);
            J3DJoint::mCurrentMtxCalc = &calc;
            trace.clear();
            if (version == 0) joint[0]->recursiveCalc();
            else referenceTraversal(joint[0]);
            traces[version] = trace;
            std::memcpy(finalMtx[version], J3DSys::mCurrentMtx, sizeof(Mtx));
            finalScale[version] = J3DSys::mCurrentS, finalParent[version] = J3DSys::mParentS;
        }
        all &= sameBits(anmA.data(), anmB.data(), count * sizeof(Mat)) && traces[0] == traces[1] && sameBits(finalMtx[0], finalMtx[1], sizeof(Mtx)) &&
               sameBits(&finalScale[0], &finalScale[1], sizeof(Vec)) && sameBits(&finalParent[0], &finalParent[1], sizeof(Vec));
        for (auto* j : joint) delete j;
    }
    check(all, "the traversal leaves the joints' matrices, the callbacks' view and J3DSys as the old one did");
}
} // namespace

int runJ3DAnimTests() {
    concatenation();
    envelopes();
    keys();
    traversal();
    std::printf("%u J3D animation checks passed\n", checks);
    return 0;
}
