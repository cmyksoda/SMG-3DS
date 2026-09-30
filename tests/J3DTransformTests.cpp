#include <JSystem/J3DGraphBase/J3DTransform.hpp>
#include <revolution/mtx.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A turn about Y and X, a non-uniform scale and a translation.
void sample(Mtx m) {
    const float c = std::cos(0.7f), s = std::sin(0.7f), c2 = std::cos(-0.3f), s2 = std::sin(-0.3f);
    const float rows[3][4]{{c * 2, s * s2 * 3, s * c2 * 0.5f, 10}, {0, c2 * 3, -s2 * 0.5f, -4}, {-s * 2, c * s2 * 3, c * c2 * 0.5f, 7}};
    std::memcpy(m, rows, sizeof(rows));
}

// J3D's paired-single routines in their portable form, against the direct formulas.
void transforms() {
    Mtx m, inverse;
    sample(m);
    Mtx33 inverseTranspose;
    J3DPSCalcInverseTranspose(m, inverseTranspose);
    check(PSMTXInverse(m, inverse), "the sample inverts");
    bool same = true;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) same &= near(inverseTranspose[row][column], inverse[column][row]);
    check(same, "the inverse transpose is the inverse, transposed");

    Mtx flat{{1, 2, 3, 0}, {2, 4, 6, 0}, {0, 0, 1, 0}};
    Mtx33 untouched{{9, 9, 9}, {9, 9, 9}, {9, 9, 9}};
    J3DPSCalcInverseTranspose(flat, untouched);
    check(untouched[1][1] == 9, "a singular matrix leaves the destination alone");

    Mtx scaled;
    sample(scaled);
    const Vec scale{2, 3, 4};
    J3DScaleNrmMtx(scaled, scale);
    check(near(scaled[2][0], m[2][0] * 2) && near(scaled[1][2], m[1][2] * 4) && scaled[0][3] == m[0][3],
          "a normal matrix's columns scale, its translation does not");

    const float projection[4][4]{{1, 0, 0, 5}, {0, 2, 0, 0}, {0, 0, -1, -2}, {0, 0, -1, 0}};
    Mtx product;
    J3DMtxProjConcat(m, const_cast<float(*)[4]>(projection), product);
    check(near(product[1][3], m[1][0] * 5 + m[1][2] * -2) && near(product[2][2], -m[2][2] - m[2][3]),
          "a projection concatenates with all four columns");

    Mtx many[3], out[3], expected;
    for (int i = 0; i < 3; ++i) { sample(many[i]); many[i][0][3] += i; }
    J3DPSMtxArrayConcat(m, many[0], out[0], 3);
    PSMTXConcat(m, many[2], expected);
    same = true;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column) same &= near(out[2][row][column], expected[row][column]);
    check(same, "an array concatenates each matrix as PSMTXConcat does");
}
} // namespace

int runJ3DTransformTests() {
    transforms();
    std::printf("%u J3D transform checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int runJ3DSkeletonTests();
int main() {
    runJ3DTransformTests();
    return runJ3DSkeletonTests();
}
#endif
