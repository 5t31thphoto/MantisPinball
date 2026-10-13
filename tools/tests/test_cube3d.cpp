// The drawing geometry must agree with the solver: turning facelets with
// cube3d.h's layer + rotation must give EXACTLY what cube_core's mcApplyMove()
// gives, for every one of the 18 moves, from many scrambled states.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "cube_core.h"
#include "cube3d.h"
static int whereIs(C3 c, C3 nrm) {          // facelet index at a 3D sticker centre + normal
    for (int i = 0; i < 54; i++) {
        C3 q[4]; mcFaceletQuad(i, 0, q);
        C3 m = {(q[0].x+q[2].x)/2, (q[0].y+q[2].y)/2, (q[0].z+q[2].z)/2};
        const int8_t* n = MC_FACE_FRAME[i/9][0];
        if (fabsf(m.x-c.x)<0.01f && fabsf(m.y-c.y)<0.01f && fabsf(m.z-c.z)<0.01f &&
            fabsf(n[0]-nrm.x)<0.01f && fabsf(n[1]-nrm.y)<0.01f && fabsf(n[2]-nrm.z)<0.01f) return i;
    }
    return -1;
}
static void geomMove(const uint8_t in[54], int move, uint8_t out[54]) {
    memcpy(out, in, 54);
    for (int i = 0; i < 54; i++) if (mcInTurnLayer(i, move)) {
        C3 q[4]; mcFaceletQuad(i, 0, q);
        C3 c = {(q[0].x+q[2].x)/2, (q[0].y+q[2].y)/2, (q[0].z+q[2].z)/2};
        const int8_t* n = MC_FACE_FRAME[i/9][0]; C3 nv = {(float)n[0], (float)n[1], (float)n[2]};
        C3 c2 = mcRotAboutFace(c, move/3, mcTurnAngle(move)), n2 = mcRotAboutFace(nv, move/3, mcTurnAngle(move));
        int j = whereIs(c2, n2); if (j < 0) { printf("lost facelet %d\n", i); exit(1); }
        out[j] = in[i];
    }
}
int main() {
    int bad = 0, checked = 0; srand(5);
    McCube c; mcSolved(c);
    for (int trial = 0; trial < 300; trial++) {
        for (int m = 0; m < 18; m++) {
            uint8_t before[54], expect[54], got[54];
            mcCubeToFacelets(c, before);
            McCube d = c; mcApplyMove(d, m); mcCubeToFacelets(d, expect);
            geomMove(before, m, got);
            checked++;
            if (memcmp(expect, got, 54)) { if (bad++ < 4) printf("  MISMATCH on move %d (face %c, kind %d)\n", m, "URFDLB"[m/3], m%3); }
        }
        mcApplyMove(c, rand() % 18);              // walk to a new scrambled state
    }
    printf(bad ? "  %d of %d move checks FAILED\n" : "  cube3d turns == solver's mcApplyMove on all %d checks (18 moves x 300 states)\n", bad ? bad : checked, checked);
    return bad != 0;
}
