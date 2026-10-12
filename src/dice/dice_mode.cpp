// dice_mode.cpp - 3D wireframe polyhedral dice roller (demoscene style).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include "dice_mode.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

struct Vec3 { float x, y, z; };
struct Edge { uint8_t a, b; };

// ---- generated geometry (unit-normalized polyhedra) ------------------------
static const Vec3 D4_V[] = {{0.5774f,0.5774f,0.5774f},{0.5774f,-0.5774f,-0.5774f},{-0.5774f,0.5774f,-0.5774f},{-0.5774f,-0.5774f,0.5774f}};
static const Edge D4_E[] = {{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}};

static const Vec3 D6_V[] = {{-0.5774f,-0.5774f,-0.5774f},{-0.5774f,-0.5774f,0.5774f},{-0.5774f,0.5774f,-0.5774f},{-0.5774f,0.5774f,0.5774f},{0.5774f,-0.5774f,-0.5774f},{0.5774f,-0.5774f,0.5774f},{0.5774f,0.5774f,-0.5774f},{0.5774f,0.5774f,0.5774f}};
static const Edge D6_E[] = {{0,1},{0,2},{0,4},{1,3},{1,5},{2,3},{2,6},{3,7},{4,5},{4,6},{5,7},{6,7}};

static const Vec3 D8_V[] = {{1.0000f,0.0000f,0.0000f},{-1.0000f,0.0000f,0.0000f},{0.0000f,1.0000f,0.0000f},{0.0000f,-1.0000f,0.0000f},{0.0000f,0.0000f,1.0000f},{0.0000f,0.0000f,-1.0000f}};
static const Edge D8_E[] = {{0,2},{0,3},{0,4},{0,5},{1,2},{1,3},{1,4},{1,5},{2,4},{2,5},{3,4},{3,5}};

static const Vec3 D10_V[] = {{0.9439f,0.3304f,0.0000f},{0.2917f,0.3304f,0.8977f},{-0.7636f,0.3304f,0.5548f},{-0.7636f,0.3304f,-0.5548f},{0.2917f,0.3304f,-0.8977f},{0.7636f,-0.3304f,0.5548f},{-0.2917f,-0.3304f,0.8977f},{-0.9439f,-0.3304f,0.0000f},{-0.2917f,-0.3304f,-0.8977f},{0.7636f,-0.3304f,-0.5548f},{0.0000f,1.0000f,0.0000f},{0.0000f,-1.0000f,0.0000f}};
static const Edge D10_E[] = {{0,5},{0,9},{10,0},{11,5},{1,6},{1,5},{10,1},{11,6},{2,7},{2,6},{10,2},{11,7},{3,8},{3,7},{10,3},{11,8},{4,9},{4,8},{10,4},{11,9}};

static const Vec3 D12_V[] = {{-0.5774f,-0.5774f,-0.5774f},{-0.5774f,-0.5774f,0.5774f},{-0.5774f,0.5774f,-0.5774f},{-0.5774f,0.5774f,0.5774f},{0.5774f,-0.5774f,-0.5774f},{0.5774f,-0.5774f,0.5774f},{0.5774f,0.5774f,-0.5774f},{0.5774f,0.5774f,0.5774f},{0.0000f,-0.3568f,-0.9342f},{-0.3568f,-0.9342f,0.0000f},{-0.9342f,0.0000f,-0.3568f},{0.0000f,-0.3568f,0.9342f},{-0.3568f,0.9342f,0.0000f},{0.9342f,0.0000f,-0.3568f},{0.0000f,0.3568f,-0.9342f},{0.3568f,-0.9342f,0.0000f},{-0.9342f,0.0000f,0.3568f},{0.0000f,0.3568f,0.9342f},{0.3568f,0.9342f,0.0000f},{0.9342f,0.0000f,0.3568f}};
static const Edge D12_E[] = {{0,8},{0,9},{0,10},{1,9},{1,11},{1,16},{2,10},{2,12},{2,14},{3,12},{3,16},{3,17},{4,8},{4,13},{4,15},{5,11},{5,15},{5,19},{6,13},{6,14},{6,18},{7,17},{7,18},{7,19},{8,14},{9,15},{10,16},{11,17},{12,18},{13,19}};

static const Vec3 D20_V[] = {{0.0000f,-0.5257f,-0.8507f},{-0.5257f,-0.8507f,0.0000f},{-0.8507f,0.0000f,-0.5257f},{0.0000f,-0.5257f,0.8507f},{-0.5257f,0.8507f,0.0000f},{0.8507f,0.0000f,-0.5257f},{0.0000f,0.5257f,-0.8507f},{0.5257f,-0.8507f,0.0000f},{-0.8507f,0.0000f,0.5257f},{0.0000f,0.5257f,0.8507f},{0.5257f,0.8507f,0.0000f},{0.8507f,0.0000f,0.5257f}};
static const Edge D20_E[] = {{0,1},{0,2},{0,5},{0,6},{0,7},{1,2},{1,3},{1,7},{1,8},{2,4},{2,6},{2,8},{3,7},{3,8},{3,9},{3,11},{4,6},{4,8},{4,9},{4,10},{5,6},{5,7},{5,10},{5,11},{6,10},{7,11},{8,9},{9,10},{9,11},{10,11}};

// ---- die table -------------------------------------------------------------
struct DieDef {
    const char* name;
    int sides;
    const Vec3* v; int vn;
    const Edge* e; int en;
};
static const DieDef DICE[] = {
    { "d4",  4,  D4_V,  4,  D4_E,  6 },
    { "d6",  6,  D6_V,  8,  D6_E,  12 },
    { "d8",  8,  D8_V,  6,  D8_E,  12 },
    { "d10", 10, D10_V, 12, D10_E, 20 },
    { "d12", 12, D12_V, 20, D12_E, 30 },
    { "d20", 20, D20_V, 12, D20_E, 30 },
};
static const int DIE_COUNT = sizeof(DICE)/sizeof(DICE[0]);

// ---- state -----------------------------------------------------------------
static int   diceSel = 1;           // current die index (default d6)
static float angX = 0.3f, angY = 0.5f, angZ = 0.0f;   // current rotation
static float spinX = 0, spinY = 0, spinZ = 0;          // angular velocity
static int   diceResult = 0;        // last rolled value (0 = none yet)
static bool  rolling = false;
static uint32_t rollStart = 0;
static const uint32_t ROLL_MS = 1500;
static uint32_t resultFlash = 0;

// trail of previous projected positions for a subtle motion-blur feel
static int diceCenterY = 150;

void diceEnter() {
    fb.setRotation(0);
    rolling = false;
    diceResult = 0;
    spinX = spinY = spinZ = 0;
    angX = 0.3f; angY = 0.5f; angZ = 0.0f;
}
void diceShutdown() {}

static void startRoll() {
    rolling = true;
    rollStart = millis();
    // random tumble velocities
    spinX = 0.25f + (esp_random() % 100) / 300.0f;
    spinY = 0.30f + (esp_random() % 100) / 250.0f;
    spinZ = 0.10f + (esp_random() % 100) / 400.0f;
    if (esp_random() & 1) spinX = -spinX;
    if (esp_random() & 1) spinZ = -spinZ;
    diceResult = 0;
}

void diceTick() {
    bool lEdge = btnL && !btnL_prev;
    bool rEdge = btnR && !btnR_prev;

    if (!rolling) {
        if (lEdge) { diceSel = (diceSel + 1) % DIE_COUNT; diceResult = 0; }
        if (rEdge) startRoll();
        // gentle idle rotation so the die always looks alive
        angY += 0.012f;
        angX += 0.005f;
    } else {
        uint32_t t = millis() - rollStart;
        float p = (float)t / ROLL_MS;
        if (p >= 1.0f) {
            rolling = false;
            diceResult = 1 + (int)(esp_random() % DICE[diceSel].sides);
            resultFlash = millis();
            spinX = spinY = spinZ = 0;
        } else {
            // ease-out: spin fast then slow toward a stop
            float decay = 1.0f - p;
            float sp = decay * decay;
            angX += spinX * sp * 1.6f;
            angY += spinY * sp * 1.6f;
            angZ += spinZ * sp * 1.6f;
        }
    }
}

// rotate a point by the current Euler angles
static Vec3 rotate(const Vec3& v) {
    float cx = cosf(angX), sx = sinf(angX);
    float cy = cosf(angY), sy = sinf(angY);
    float cz = cosf(angZ), sz = sinf(angZ);
    // X
    float y1 = v.y * cx - v.z * sx;
    float z1 = v.y * sx + v.z * cx;
    float x1 = v.x;
    // Y
    float x2 = x1 * cy + z1 * sy;
    float z2 = -x1 * sy + z1 * cy;
    float y2 = y1;
    // Z
    float x3 = x2 * cz - y2 * sz;
    float y3 = x2 * sz + y2 * cz;
    float z3 = z2;
    return { x3, y3, z3 };
}

// brightness ramp for depth (z): nearer edges glow brighter
static uint16_t depthColor(float z) {
    // z in roughly [-1,1]; map to a teal->lime glow
    if (z >  0.45f) return LIME;
    if (z >  0.05f) return LIME_PALE;
    if (z > -0.35f) return TEAL_LIGHT;
    if (z > -0.7f)  return TEAL_BRIGHT;
    return TEAL_DARK;
}

static void drawDie(int cx, int cy, float scale) {
    const DieDef& d = DICE[diceSel];
    // project all vertices
    static float px[24], py[24], pz[24];
    for (int i = 0; i < d.vn; i++) {
        Vec3 r = rotate(d.v[i]);
        // simple perspective
        float zoff = 3.0f;
        float persp = zoff / (zoff - r.z * 0.9f);
        px[i] = cx + r.x * scale * persp;
        py[i] = cy + r.y * scale * persp;
        pz[i] = r.z;
    }
    // draw edges back-to-front-ish: draw dark first by sorting on midpoint z.
    // simple insertion order: two passes (far dim, near bright) for glow feel.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < d.en; i++) {
            int a = d.e[i].a, b = d.e[i].b;
            float mz = (pz[a] + pz[b]) * 0.5f;
            bool near = mz > -0.1f;
            if ((pass == 0 && near) || (pass == 1 && !near)) continue;
            uint16_t col = depthColor(mz);
            // pass 0 draws far edges (dim); pass 1 draws near edges (bright) with
            // a thin halo for the demoscene glow
            if (pass == 1) {
                // halo
                fb.drawLine((int)px[a], (int)py[a]+1, (int)px[b], (int)py[b]+1, TEAL_DARK);
            }
            fb.drawLine((int)px[a], (int)py[a], (int)px[b], (int)py[b], col);
        }
    }
    // glowing vertices on the near side
    for (int i = 0; i < d.vn; i++) {
        if (pz[i] > 0.2f) {
            fb.fillCircle((int)px[i], (int)py[i], 2, LIME_PALE);
        } else if (pz[i] > -0.3f) {
            fb.drawPixel((int)px[i], (int)py[i], TEAL_LIGHT);
        }
    }
}

// a faint starfield backdrop for the demoscene vibe (static, cheap)
static void drawStars() {
    // deterministic pseudo-stars so they don't flicker
    static const uint16_t SX[] = {12, 40, 78, 110, 150, 26, 64, 96, 134, 158, 8, 52, 88, 120, 145};
    static const uint16_t SY[] = {40, 70, 55, 90, 48, 110, 130, 100, 125, 80, 160, 175, 150, 185, 168};
    for (int i = 0; i < 15; i++) {
        uint16_t c = (i & 1) ? TEAL_DARK : TEAL_BRIGHT;
        fb.drawPixel(SX[i], SY[i], c);
    }
}

void diceDraw() {
    fb.fillSprite(TEAL_DEEP);
    drawStars();

    // header
    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("DICE", 4, 9, 1);
    fb.setTextDatum(MR_DATUM); fb.setTextColor(TEAL_LIGHT);
    fb.drawString(DICE[diceSel].name, W - 4, 9, 1);

    // the die
    float scale = rolling ? 52.0f : 56.0f;
    drawDie(W / 2, diceCenterY, scale);

    // result readout
    uint32_t now = millis();
    if (!rolling && diceResult > 0) {
        bool fresh = (now - resultFlash) < 250;
        fb.setTextDatum(MC_DATUM);
        fb.setTextColor(fresh ? LIME_PALE : LIME);
        char b[8]; snprintf(b, sizeof(b), "%d", diceResult);
        fb.drawString(b, W / 2, 250, 7);
    } else if (rolling) {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("rolling", W / 2, 250, 2);
    } else {
        fb.setTextDatum(MC_DATUM); fb.setTextColor(TEAL_DARK);
        fb.drawString("roll me", W / 2, 250, 2);
    }

    // footer
    fb.fillRect(0, H - 14, W, 14, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("L: die", 4, H - 7, 1);
    fb.setTextDatum(MR_DATUM); fb.drawString("R: roll", W - 4, H - 7, 1);

    fb.pushSprite(0, 0);
}
