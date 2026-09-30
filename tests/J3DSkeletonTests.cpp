#include "BmdFixture.hpp"
#include <smg/J3DImage.hpp>
#include <smg/Model.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

bool same(const float* a, const std::array<float, 12>& b) {
    for (int i = 0; i < 12; ++i)
        if (std::fabs(a[i] - b[i]) > 1e-4f) return false;
    return true;
}

// The fixture's two joints (the child moved 10 along X and turned a quarter about Y) and one envelope mixing
// both evenly, through JSystem's own loader and J3DModel, against the port's own skinning.
void skeleton() {
    Options options;
    options.envelope = true;
    const Fixture f = fixture(options);
    smg::J3DSkeletonImage image;
    std::string error;
    check(image.load({f.data.data(), f.data.size()}, error) && image.joints() == 2, "the skeleton image builds");

    J3DModelLoader_v26 loader;
    J3DModelData* data = loader.load(image.data(), 0);
    check(data && data->getJointNum() == 2, "JSystem's loader reads it");
    check(data->getJointNodePointer(1)->getYounger() == nullptr && data->getJointNodePointer(0)->getChild() == data->getJointNodePointer(1),
          "the child joint hangs from the root, as INF1 nests it");
    auto* model = new J3DModel(data, 0, 1);
    model->calcAnmMtx();
    model->mMtxBuffer->calcWeightEnvelopeMtx();

    smg::Model port;
    check(smg::loadModel({f.data.data(), f.data.size()}, port, 4096, error, true), "the port's loader reads it too");
    std::vector<smg::JointTransform> pose(port.joints.size());
    for (std::size_t j = 0; j < pose.size(); ++j) {
        std::copy(port.joints[j].scale, port.joints[j].scale + 3, pose[j].scale);
        std::copy(port.joints[j].rotation, port.joints[j].rotation + 3, pose[j].rotation);
        std::copy(port.joints[j].translation, port.joints[j].translation + 3, pose[j].translation);
    }
    std::vector<std::array<float, 12>> world, draws;
    smg::poseSkeleton(port, pose, world);
    smg::skinMatrices(port, world, draws);
    check(same(&model->getAnmMtx(0)[0][0], world[0]) && same(&model->getAnmMtx(1)[0][0], world[1]),
          "each joint's matrix is the port's");
    check(same(&model->getWeightAnmMtx(0)[0][0], draws[0]), "the envelope's matrix is the port's weighted skin");

    smg::forgetJ3DSkeleton(model);
}
} // namespace

int runJ3DSkeletonTests() {
    skeleton();
    std::printf("%u J3D skeleton checks passed\n", checks);
    return 0;
}
