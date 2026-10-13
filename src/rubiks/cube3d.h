// cube3d.h - geometry for drawing the cube exactly as the solver sees it.
//
// Facelets are the solver's Kociemba layout (cube_core.h): URFDLB face order,
// each face read row-major in its standard orientation:
//   U seen from above with B at the top      R seen from the right, U on top
//   F seen from the front, U on top          D seen from below with F at the top
//   L seen from the left, U on top           B seen from the back, U on top
// Model axes: +x = R, +y = U, +z = F (toward you when GREEN faces you).
//
// mcFacelet3D() places every facelet in 3D; mcTurnLayer() says which facelets
// a move turns and about which axis. tools/tests/test_cube3d.cpp proves that
// rotating facelets with this geometry reproduces cube_core's mcApplyMove()
// for all 18 moves - so the animation always matches the real move.
#pragma once
#include <stdint.h>
#include <math.h>

struct C3 { float x, y, z; };

// per face: outward normal, "right" and "down" directions of its reading order
static const int8_t MC_FACE_FRAME[6][3][3] = {
    {{0, 1, 0}, { 1, 0, 0}, {0, 0,  1}},   // U
    {{1, 0, 0}, { 0, 0,-1}, {0,-1,  0}},   // R
    {{0, 0, 1}, { 1, 0, 0}, {0,-1,  0}},   // F
    {{0,-1, 0}, { 1, 0, 0}, {0, 0, -1}},   // D
    {{-1,0, 0}, { 0, 0, 1}, {0,-1,  0}},   // L
    {{0, 0,-1}, {-1, 0, 0}, {0,-1,  0}},   // B
};

// cubie position (each coordinate -1, 0 or +1) a facelet sits on
static inline void mcFaceletCubie(int idx, int& x, int& y, int& z) {
    const int f = idx / 9, s = idx % 9, row = s / 3, col = s % 3;
    const int8_t (*F)[3] = MC_FACE_FRAME[f];
    x = F[0][0] + (col - 1) * F[1][0] + (row - 1) * F[2][0];
    y = F[0][1] + (col - 1) * F[1][1] + (row - 1) * F[2][1];
    z = F[0][2] + (col - 1) * F[1][2] + (row - 1) * F[2][2];
}

// the four corners of a facelet's square, in model units (cube spans +-1.5)
static inline void mcFaceletQuad(int idx, float inset, C3 out[4]) {
    const int f = idx / 9, s = idx % 9, row = s / 3, col = s % 3;
    const int8_t (*F)[3] = MC_FACE_FRAME[f];
    const float h = 0.5f - inset;
    const float cx = 1.5f * F[0][0] + (col - 1) * F[1][0] + (row - 1) * F[2][0];
    const float cy = 1.5f * F[0][1] + (col - 1) * F[1][1] + (row - 1) * F[2][1];
    const float cz = 1.5f * F[0][2] + (col - 1) * F[1][2] + (row - 1) * F[2][2];
    const float su[4] = {-h, h, h, -h}, sv[4] = {-h, -h, h, h};
    for (int k = 0; k < 4; k++) {
        out[k].x = cx + su[k] * F[1][0] + sv[k] * F[2][0];
        out[k].y = cy + su[k] * F[1][1] + sv[k] * F[2][1];
        out[k].z = cz + su[k] * F[1][2] + sv[k] * F[2][2];
    }
}

// a move turns the layer on face (move/3); clockwise as seen from outside that
// face is -90 degrees about its outward normal (right-hand rule)
static inline bool mcInTurnLayer(int idx, int move) {
    int x, y, z; mcFaceletCubie(idx, x, y, z);
    const int8_t* n = MC_FACE_FRAME[move / 3][0];
    return x * n[0] + y * n[1] + z * n[2] == 1;
}
static inline float mcTurnAngle(int move) {            // full turn, radians
    const int kind = move % 3;                         // 0 = cw, 1 = ccw ('), 2 = half
    return kind == 0 ? -1.5707963f : kind == 1 ? 1.5707963f : -3.1415927f;
}

// rotate p about the axis of face f's normal by angle a
static inline C3 mcRotAboutFace(C3 p, int f, float a) {
    const int8_t* n = MC_FACE_FRAME[f][0];
    const float c = cosf(a), s = sinf(a);
    // Rodrigues with a unit axis along +-x/y/z
    const float kx = n[0], ky = n[1], kz = n[2];
    const float dot = kx * p.x + ky * p.y + kz * p.z;
    const float cx = ky * p.z - kz * p.y, cy = kz * p.x - kx * p.z, cz = kx * p.y - ky * p.x;
    return { p.x * c + cx * s + kx * dot * (1 - c),
             p.y * c + cy * s + ky * dot * (1 - c),
             p.z * c + cz * s + kz * dot * (1 - c) };
}
