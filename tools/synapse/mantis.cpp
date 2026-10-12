// ============================================================
//  SYNAPSE — the mantis puppet
//  Skeletal rig built from the separated sprite sheet: every part is a
//  sprite rotated about its real joint (forward kinematics), legs are
//  2-bone IK with planted feet, and a beat-locked choreographer blends
//  dance moves. Sing mode zooms in and lip-syncs an anchored mouth.
// ============================================================
#include "app.h"
#include "audio.h"
#include "fx.h"
#include "mantis_rig.h"
#include <string.h>

static const float DEG = 0.01745329f;

// ---------- sprites ----------
struct Bone {
  M5Canvas spr;
  float pvx, pvy;
  int w, h;
  bool mirror;
  bool fast = false;                   // small parts: non-AA rotate (cheaper)
};
enum { B_TORSO, B_HEAD, B_ABD, B_ARM_L, B_ARM_R, B_SCY_L, B_SCY_R, B_THI_L, B_THI_R, B_SHI_L, B_SHI_R, B_CLAW_L, B_CLAW_R, B_COUNT };
static Bone s_b[B_COUNT];
static bool s_ready = false;
static uint16_t s_skin = 0;

static void build(Bone &b, const RigPart &p, bool mirror) {
  b.w = p.w; b.h = p.h; b.mirror = mirror;
  b.pvx = mirror ? p.w - p.pvx : p.pvx;
  b.pvy = p.pvy;
  b.spr.setColorDepth(16);
  b.spr.setPsram(true);
  if (!b.spr.createSprite(p.w, p.h)) return;
  for (int y = 0; y < p.h; y++)
    for (int x = 0; x < p.w; x++) {
      int sx = mirror ? (p.w - 1 - x) : x;
      b.spr.drawPixel(x, y, p.px[y * p.w + sx]);
    }
  b.spr.setPivot(b.pvx, b.pvy);
}

void mantisBegin() {
  build(s_b[B_TORSO], RIG_TORSO, false);
  build(s_b[B_HEAD], RIG_HEAD, false);
  build(s_b[B_ABD], RIG_ABDOMEN, false);
  build(s_b[B_ARM_L], RIG_ARM, false);
  build(s_b[B_ARM_R], RIG_ARM, true);
  build(s_b[B_SCY_L], RIG_SCYTHE, false);
  build(s_b[B_SCY_R], RIG_SCYTHE, true);
  build(s_b[B_CLAW_L], RIG_CLAW, false);
  build(s_b[B_CLAW_R], RIG_CLAW, true);
  for (int k : {B_THI_L, B_THI_R, B_SHI_L, B_SHI_R, B_CLAW_L, B_CLAW_R}) s_b[k].fast = true;
  build(s_b[B_THI_L], RIG_THIGH, false);
  build(s_b[B_THI_R], RIG_THIGH, true);
  build(s_b[B_SHI_L], RIG_SHIN, false);
  build(s_b[B_SHI_R], RIG_SHIN, true);
  s_skin = RIG_HEAD.px[(int)(RIG_HEAD_MOUTH_Y - 16) * RIG_HEAD.w + (int)RIG_HEAD_MOUTH_X];
  if (s_skin == RIG_KEY) s_skin = rgb565(150, 205, 120);
  s_ready = true;
}

// ---------- kinematics ----------
static float s_Z = 0.72f;          // rig units -> screen px
static float s_rx = 160, s_ry = 163;

struct Xf { float x, y, a, zx, zy; };
// point (lx,ly) in bone's sprite space -> rig space
static inline void fk(const Xf &f, const Bone &b, float lx, float ly, float &ox, float &oy, bool mir = false) {
  if (mir) lx = b.w - lx;
  float dx = (lx - b.pvx) * f.zx, dy = (ly - b.pvy) * f.zy;
  float c = cosf(f.a * DEG), s = sinf(f.a * DEG);
  ox = f.x + c * dx - s * dy;
  oy = f.y + s * dx + c * dy;
}
static inline void toScr(float x, float y, float &sx, float &sy) { sx = s_rx + x * s_Z; sy = s_ry + y * s_Z; }
static void blit(Bone &b, const Xf &f) {
  if (!b.spr.width()) return;
  float sx, sy; toScr(f.x, f.y, sx, sy);
  if (b.fast) b.spr.pushRotateZoom(&canvas, sx, sy, f.a, f.zx * s_Z, f.zy * s_Z, (uint16_t)RIG_KEY);
  else b.spr.pushRotateZoomWithAA(&canvas, sx, sy, f.a, f.zx * s_Z, f.zy * s_Z, (uint16_t)RIG_KEY);
}
static float boneAngle(const Bone &b, float ex, float ey, bool mir) {
  float px = b.pvx, ex2 = mir ? b.w - ex : ex;
  return atan2f(ey - b.pvy, ex2 - px) / DEG;
}

// ---------- pose ----------
struct Pose {
  float x, y, torso, head, hx, hy, abd;
  float au[2], as[2];
  float fx[4], fy[4];
  float sq, mouth;
  float claw[2];                       // pincers: 0 shut .. 0.4 resting .. 1 wide open
  float strain;                        // 1 = straining overhead like a hype man: spines may face up
};
static const int NPF = sizeof(Pose) / sizeof(float);
static void lerpPose(Pose &o, const Pose &a, const Pose &b, float t) {
  const float *pa = (const float *)&a, *pb = (const float *)&b;
  float *po = (float *)&o;
  for (int i = 0; i < NPF; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

enum Move { M_IDLE, M_BOB, M_SWAY, M_WAVE_L, M_WAVE_R, M_CHEER, M_BANG, M_SPREAD, M_SHUFFLE, M_SING, M_TALK, M_LEAN, M_LISTEN,
            // styles
            M_POP, M_ROBOT, M_RUNMAN, M_TSTEP, M_MOON, M_BODYWAVE, M_HYPE,
            // emotes (one beat, on accents)
            M_E_CLAP, M_E_SNAP, M_E_POINT, M_E_FLEX, M_E_GUITAR, M_E_PEACE, M_E_BOW,
            // ---- V25: the big vocabulary ----
            M_NOD, M_TWOSTEP, M_CABBAGE, M_TUT, M_SHLEAN, M_TOPROCK,          // hip-hop
            M_WINDMILL, M_STOMP, M_MOSH,                                     // metal
            M_HEELTOE, M_GRAPEVINE, M_HOEDOWN, M_SWAGGER,                    // country
            M_PUMP, M_JUMPUP, M_ARMWAVE, M_SHUFFLE2, M_SWAYUP,               // pop / EDM
            M_WOBBLE, M_GLITCH, M_SLOWMO, M_LIQUID,                          // dubstep
            M_TWIG, M_PRAY, M_STALK,                                         // soft / pure mantis
            M_TRILL, M_FREEZE,                                               // flourish, freeze
            M_E_HORNS, M_E_LASSO, M_E_OVERCLAP, M_E_STRIKE, M_E_GROOM, M_E_SWIVEL, M_E_THREAT, M_E_SHRUG, M_E_DROP,
            M_COUNT };
#include "dance_net.h"
// Synapse move -> MantisNow DANCE move id (MANTISNOW.md decision 6):
// 0 bob 1 sway 2 arms-up 3 spin 4 jump 5 wave 6 robot 7 shuffle 8 head-bang 9 pose
static uint8_t mnMoveOf(int m) {
  switch (m) {
    case M_IDLE: case M_BOB: case M_NOD: case M_TWOSTEP: case M_SING: case M_TALK: case M_LISTEN: return 0;
    case M_SWAY: case M_SWAYUP: case M_LEAN: case M_BODYWAVE: case M_LIQUID: case M_SLOWMO: case M_SWAGGER: case M_SHLEAN: return 1;
    case M_CHEER: case M_SPREAD: case M_HYPE: case M_PUMP: case M_ARMWAVE: case M_E_PEACE: case M_E_CLAP: case M_E_OVERCLAP: return 2;
    case M_WINDMILL: case M_TRILL: case M_E_SWIVEL: case M_TOPROCK: return 3;
    case M_JUMPUP: case M_STOMP: case M_HOEDOWN: case M_E_DROP: return 4;
    case M_WAVE_L: case M_WAVE_R: case M_E_POINT: case M_E_LASSO: case M_E_SNAP: return 5;
    case M_ROBOT: case M_POP: case M_TUT: case M_GLITCH: case M_FREEZE: return 6;
    case M_SHUFFLE: case M_SHUFFLE2: case M_RUNMAN: case M_TSTEP: case M_MOON: case M_HEELTOE: case M_GRAPEVINE: case M_CABBAGE: return 7;
    case M_BANG: case M_MOSH: case M_WOBBLE: case M_E_HORNS: case M_E_GUITAR: return 8;
    default: return 9;
  }
}
static int s_freezeVar = 0;
static bool s_cave = false;                 // the echo cave scene
#ifdef HOST
__attribute__((weak)) int g_dbgMove = -1;
__attribute__((weak)) float g_dbgClaw = -1.f, g_dbgBeat = 0.f;
#endif
static float s_mouthOpen = 0, s_mouthWide = 0;


static inline float dip(float f) { return 0.5f + 0.5f * cosf(6.2831853f * f); }
static inline float hit(float f) { float u = 1.f - f; return u * u * u; }

static float s_t = 0;
static float s_look = 0, s_lookY = 0;      // touch/tilt gaze
static float s_sustain = 0;               // sing: held-note energy

static void evalMove(int m, float b, float e, Pose &p) {
  memset(&p, 0, sizeof(p));
  p.claw[0] = p.claw[1] = 0.4f;
  float f = b - floorf(b);
  int bi = ((int)floorf(b)) & 3;
  float sgn = (bi & 1) ? 1.f : -1.f;
  float t = s_t;
  switch (m) {
    case M_IDLE:
      p.y = sinf(t * 1.6f) * 1.4f;
      p.sq = sinf(t * 1.6f) * 0.25f;
      p.head = s_look * 16.f + sinf(t * 0.7f) * 4.f;
      p.hx = s_look * 3.f;
      p.torso = s_look * 4.f + sinf(t * 0.5f) * 1.5f;
      p.abd = sinf(t * 1.1f) * 5.f;
      p.as[0] = p.as[1] = sinf(t * 2.7f) * 5.f;
      p.au[0] = p.au[1] = 3.f + 2.f * sinf(t * 1.3f);
      p.claw[0] = 0.3f + 0.1f * sinf(t * 0.9f); p.claw[1] = 0.3f + 0.1f * sinf(t * 0.8f + 1.f);
      break;
    case M_BOB:
      p.y = 8.f * e * dip(f) + 1.f;
      p.sq = 0.9f * hit(f) * e;
      p.head = 7.f * sgn * (1.f - f) + s_look * 8.f;
      p.hy = 2.f * dip(f);
      p.au[0] = p.au[1] = 16.f + 24.f * dip(f);
      p.as[0] = p.as[1] = -8.f + 16.f * dip(f);
      p.abd = 8.f * sinf(3.14159f * b);
      p.claw[0] = p.claw[1] = 0.25f + 0.35f * dip(f);            // pinch on the beat
      break;
    case M_SWAY: {
      float s = sinf(3.14159f * b);
      p.x = 13.f * s; p.torso = -9.f * s; p.head = 13.f * s; p.abd = -16.f * s;
      p.au[0] = 12.f + 60.f * fmaxf(0.f, s);
      p.au[1] = 12.f + 60.f * fmaxf(0.f, -s);
      p.as[0] = 25.f * fmaxf(0.f, s); p.as[1] = 25.f * fmaxf(0.f, -s);
      p.y = 5.f * dip(f) * e;
      p.fy[0] = p.fy[1] = -7.f * fmaxf(0.f, s);
      p.fy[2] = p.fy[3] = -7.f * fmaxf(0.f, -s);
      p.fx[0] = p.fx[1] = p.fx[2] = p.fx[3] = 4.f * s;
      break;
    }
    case M_WAVE_L: case M_WAVE_R: {
      int a = (m == M_WAVE_L) ? 0 : 1; float d = a ? 1.f : -1.f;
      p.au[a] = 150.f + 6.f * sinf(6.2831853f * b); p.strain = 1.f;
      p.as[a] = -15.f + 40.f * sinf(6.2831853f * b);
      p.au[1 - a] = 12.f; p.as[1 - a] = 6.f * dip(f);
      p.torso = 6.f * d; p.head = 10.f * d + 5.f * sinf(3.14159f * b); p.x = 5.f * d;
      p.y = 3.5f * dip(f); p.mouth = 0.25f;
      p.abd = -10.f * d;
      break;
    }
    case M_CHEER: {
      float air = sinf(3.14159f * f);
      p.au[0] = p.au[1] = 150.f + 12.f * cosf(6.2831853f * f);
      p.as[0] = p.as[1] = -30.f + 35.f * cosf(6.2831853f * f);
      p.y = -13.f * e * air;
      for (int i = 0; i < 4; i++) p.fy[i] = p.y * 0.9f;
      p.sq = 1.2f * hit(f) - 0.5f * air;
      p.head = 7.f * sinf(3.14159f * b); p.mouth = 0.55f + 0.3f * air;
      p.abd = 12.f * sinf(6.2831853f * b);
      p.claw[0] = p.claw[1] = 0.9f;                            // hands up, claws wide
      p.strain = 1.f;
      break;
    }
    case M_BANG: {
      float h = hit(f);
      p.hy = 6.f * h; p.head = 18.f * sgn * h; p.torso = 6.f * sgn * h;
      p.au[0] = p.au[1] = 32.f;
      p.as[0] = (bi & 1) ? 55.f * h : 0.f; p.as[1] = (bi & 1) ? 0.f : 55.f * h;
      p.y = 6.f * h; p.sq = 1.0f * h; p.abd = -14.f * sgn * h;
      p.claw[0] = (bi & 1) ? 0.05f + 0.8f * (1.f - h) : 0.4f; p.claw[1] = (bi & 1) ? 0.4f : 0.05f + 0.8f * (1.f - h);   // SNAP
      break;
    }
    case M_SPREAD:
      p.au[0] = p.au[1] = 95.f;
      p.as[0] = p.as[1] = 55.f + 18.f * sinf(6.2831853f * b);
      p.abd = 22.f * sinf(12.566f * b);
      p.y = -2.f - 3.f * dip(f); p.sq = -0.5f; p.head = 8.f * sinf(3.14159f * b); p.mouth = 0.4f;
      p.claw[0] = p.claw[1] = 0.7f + 0.3f * sinf(6.2831853f * b * 2.f);
      break;
    case M_SHUFFLE: {
      float s = sinf(3.14159f * b), up = sinf(3.14159f * f);
      if (bi & 1) { p.fy[0] = p.fy[2] = -11.f * up; } else { p.fy[1] = p.fy[3] = -11.f * up; }
      p.x = 7.f * s; p.y = 4.f * dip(f);
      p.au[0] = 25.f + 35.f * fmaxf(0.f, s); p.au[1] = 25.f + 35.f * fmaxf(0.f, -s);
      p.as[0] = 30.f * fmaxf(0.f, -s); p.as[1] = 30.f * fmaxf(0.f, s);
      p.head = -9.f * s; p.torso = 4.f * s; p.abd = 12.f * s;
      break;
    }
    case M_SING: {
      float lv = clampf(aud::level * 1.6f, 0.f, 1.f);
      p.torso = sinf(t * 1.1f) * 4.f - lv * 3.f;
      p.head = sinf(t * 0.8f) * 6.f + (aud::zcr - 0.4f) * 14.f + s_look * 10.f;
      p.hy = -lv * 3.f;
      p.x = sinf(t * 0.55f) * 4.f;
      // left scythe cradles the microphone near the mouth
      p.au[0] = 118.f + lv * 6.f; p.as[0] = 118.f + sinf(t * 1.3f) * 3.f;
      // right arm: diva reach grows with held notes, flutters with vibrato
      p.au[1] = 14.f + 115.f * s_sustain + sinf(t * 7.f) * 5.f * s_sustain;
      p.as[1] = 10.f + 55.f * s_sustain + sinf(t * 9.f) * 8.f * lv;
      p.abd = sinf(t * 0.9f) * 8.f;
      p.y = sinf(t * 1.1f) * 1.2f + lv * 2.f;
      p.claw[0] = 0.02f;                                        // gripping the mic
      p.claw[1] = 0.4f + 0.5f * s_sustain;
      break;
    }
    // ---------------- POP & LOCK: snap to a pose on each half-beat, freeze, snap again ----------------
    case M_POP: {
      int k = ((int)floorf(b * 2.f)) & 3; float h = hit(fmodf(b * 2.f, 1.f)), ov = 1.f + 0.25f * h;   // tiny overshoot on the hit
      static const float PA[4][6] = {{95, 20, 30, 95, -8, 0}, {30, 95, 95, 30, 8, 0}, {140, 140, 60, 60, 0, -3}, {20, 20, 110, 110, 0, 2}};
      p.au[0] = PA[k][0] * ov; p.au[1] = PA[k][1] * ov; p.as[0] = PA[k][2]; p.as[1] = PA[k][3];
      p.head = PA[k][4] * ov + (k & 1 ? 6.f : -6.f); p.torso = PA[k][5] * 2.f; p.hx = (k & 1) ? 2.f : -2.f;
      p.y = 2.f * h; p.sq = 0.6f * h;
      p.claw[0] = (k == 2) ? 0.95f : 0.1f; p.claw[1] = (k == 3) ? 0.95f : 0.1f;
      p.strain = 1.f;                                              // elbow pops: spines up is the look
      break;
    }
    case M_ROBOT: {                                               // a wave travelling arm -> scythe -> claw, stiff torso
      float w = 6.2831853f * b;
      p.au[0] = 60.f + 50.f * sinf(w); p.as[0] = 40.f + 50.f * sinf(w - 1.2f); p.claw[0] = 0.4f + 0.5f * sinf(w - 2.4f);
      p.au[1] = 60.f + 50.f * sinf(w + 3.14f); p.as[1] = 40.f + 50.f * sinf(w + 1.94f); p.claw[1] = 0.4f + 0.5f * sinf(w + 0.74f);
      p.head = 8.f * ((((int)floorf(b * 4.f)) & 1) ? 1.f : -1.f);     // head ticks in quarter steps
      p.torso = 0; p.y = 1.5f * dip(f); p.strain = 1.f;
      break;
    }
    // ---------------- FOOTWORK: fast feet (2x the beat), cool upper body ----------------
    case M_RUNMAN: {
      float q = b * 2.f, qf = q - floorf(q); int qi = ((int)floorf(q)) & 1;
      float lift = sinf(3.14159f * qf);
      if (qi) { p.fy[0] = p.fy[1] = -12.f * lift; p.fx[0] = p.fx[1] = -6.f * lift; p.fx[2] = p.fx[3] = 5.f * lift; }
      else { p.fy[2] = p.fy[3] = -12.f * lift; p.fx[2] = p.fx[3] = 6.f * lift; p.fx[0] = p.fx[1] = -5.f * lift; }
      p.y = 3.f * lift; p.au[0] = 40.f + 30.f * (qi ? lift : 0.f); p.au[1] = 40.f + 30.f * (qi ? 0.f : lift);
      p.as[0] = p.as[1] = 60.f; p.claw[0] = p.claw[1] = 0.2f; p.head = 3.f * sinf(6.2831853f * b);
      break;
    }
    case M_TSTEP: {                                               // heel-toe shuffle, sliding sideways
      float q = b * 4.f, qf = q - floorf(q); int qi = ((int)floorf(q)) & 3;
      float tap = sinf(3.14159f * qf);
      p.x = 10.f * sinf(3.14159f * b * 0.5f);
      p.fy[qi] = -9.f * tap; p.fx[qi] = ((qi & 1) ? 6.f : -6.f) * tap;
      p.au[0] = p.au[1] = 22.f; p.as[0] = p.as[1] = 25.f + 10.f * tap; p.claw[0] = p.claw[1] = 0.25f;
      p.torso = -3.f * sinf(3.14159f * b); p.y = 2.f * tap;
      break;
    }
    // ---------------- SMOOTH ----------------
    case M_MOON: {                                                // moonwalk glide: body slides, feet slide back
      float s = fmodf(b * 0.5f, 2.f); float dir = s < 1.f ? 1.f : -1.f; float g = s < 1.f ? s : 2.f - s;
      p.x = -18.f + 36.f * g;
      int ff = ((int)floorf(b)) & 1; float slide = f;
      p.fx[ff * 2] = p.fx[ff * 2 + 1] = -10.f * dir * slide; p.fy[(1 - ff) * 2] = -4.f;
      p.au[0] = p.au[1] = 18.f; p.as[0] = p.as[1] = 14.f; p.torso = -4.f * dir; p.head = 6.f * dir;
      p.claw[0] = p.claw[1] = 0.3f;
      break;
    }
    case M_BODYWAVE: {                                            // a wave rolling torso -> abdomen -> head
      float w = 6.2831853f * b * 0.5f;
      p.torso = 9.f * sinf(w); p.abd = 22.f * sinf(w - 1.1f); p.head = 12.f * sinf(w - 2.2f); p.hy = 3.f * sinf(w - 2.2f);
      p.y = 3.f * sinf(w + 1.f); p.au[0] = p.au[1] = 50.f + 25.f * sinf(w - 0.6f); p.as[0] = p.as[1] = 60.f + 30.f * sinf(w - 1.6f);
      p.claw[0] = p.claw[1] = 0.5f + 0.4f * sinf(w - 2.6f);
      break;
    }
    // ---------------- HYPE ----------------
    case M_HYPE: {
      float air = sinf(3.14159f * f), h = hit(f);
      p.y = -16.f * e * air; for (int i = 0; i < 4; i++) p.fy[i] = p.y * 0.95f;
      p.au[0] = p.au[1] = 120.f + 40.f * h; p.as[0] = p.as[1] = 20.f + 60.f * air;
      p.claw[0] = p.claw[1] = 0.1f + 0.8f * h;                    // claw pump on the kick
      p.strain = 1.f;
      p.head = -10.f * h; p.hy = 5.f * h; p.sq = 1.1f * h - 0.4f * air; p.mouth = 0.6f * air;
      break;
    }
    // ---------------- EMOTES (one beat each) ----------------
    case M_E_CLAP: {
      float c = fmodf(b * 2.f, 1.f), cl = hit(c);
      p.au[0] = p.au[1] = 75.f; p.as[0] = p.as[1] = 110.f - 40.f * (1.f - cl); p.claw[0] = p.claw[1] = 0.1f;
      p.head = 4.f * cl; p.mouth = 0.5f; p.y = 2.f * cl;
      break;
    }
    case M_E_SNAP: {                                              // pinch pinch!
      float c = fmodf(b * 4.f, 1.f);
      p.au[0] = p.au[1] = 115.f; p.as[0] = p.as[1] = 70.f; p.claw[0] = p.claw[1] = c < 0.5f ? 1.f : 0.f;
      p.head = 6.f * sinf(6.2831853f * b); p.mouth = 0.3f;
      break;
    }
    case M_E_POINT:                                               // right at you
      p.au[1] = 90.f; p.as[1] = -10.f; p.claw[1] = 0.f; p.au[0] = 20.f; p.as[0] = 30.f;
      p.head = -6.f; p.hx = -2.f; p.torso = -5.f; p.mouth = 0.45f;
      break;
    case M_E_FLEX: {
      float h = hit(f);
      p.au[0] = p.au[1] = 100.f; p.as[0] = p.as[1] = 140.f; p.claw[0] = p.claw[1] = 0.05f; p.strain = 1.f;
      p.y = 2.f + 2.f * h; p.sq = 0.8f * h; p.head = 3.f * sinf(12.f * b); p.mouth = 0.2f;
      break;
    }
    case M_E_GUITAR: {                                            // air guitar strum
      float s = sinf(6.2831853f * b * 2.f);
      p.au[0] = 70.f; p.as[0] = 40.f; p.claw[0] = 0.6f;
      p.au[1] = 35.f + 18.f * s; p.as[1] = 80.f; p.claw[1] = 0.2f;
      p.torso = 6.f; p.head = 10.f + 6.f * s; p.hy = 2.f; p.mouth = 0.6f;
      break;
    }
    case M_E_PEACE:
      p.au[0] = p.au[1] = 140.f; p.as[0] = p.as[1] = 30.f; p.claw[0] = p.claw[1] = 1.f; p.strain = 1.f;
      p.head = 5.f * sinf(6.2831853f * b); p.mouth = 0.5f; p.y = -2.f;
      break;
    case M_E_BOW:                                                 // the mantis prayer bow
      p.au[0] = p.au[1] = 70.f; p.as[0] = p.as[1] = 120.f; p.claw[0] = p.claw[1] = 0.05f;
      p.torso = 0; p.head = 0; p.hy = 7.f * sinf(3.14159f * f); p.y = 3.f * sinf(3.14159f * f); p.mouth = 0.f;
      break;

    // ======================= V25 moves =======================
    // ---- hip-hop ----
    case M_NOD: {                                                 // head nod + knee bounce, arms loose
      float d = dip(f);
      p.head = 9.f * (1.f - d) - 3.f; p.hy = 3.f * (1.f - d); p.y = 5.f * (1.f - d); p.sq = 0.5f * hit(f);
      p.au[0] = 22.f + 6.f * sgn; p.au[1] = 22.f - 6.f * sgn; p.as[0] = p.as[1] = 35.f; p.claw[0] = p.claw[1] = 0.25f;
      p.torso = 3.f * sgn; p.abd = 6.f * sgn;
      break;
    }
    case M_TWOSTEP: {                                             // step out, step back, shoulders ride it
      float s = sinf(3.14159f * b), up = sinf(3.14159f * f);
      p.x = 9.f * s; p.torso = -5.f * s; p.head = 6.f * s + 4.f * up;
      int side = (bi & 1); p.fx[side * 2] = p.fx[side * 2 + 1] = 7.f * (side ? 1.f : -1.f) * up; p.fy[side * 2] = -6.f * up;
      p.au[0] = 30.f + 20.f * fmaxf(0.f, s); p.au[1] = 30.f + 20.f * fmaxf(0.f, -s); p.as[0] = p.as[1] = 50.f; p.y = 3.f * dip(f);
      break;
    }
    case M_CABBAGE: {                                             // cabbage patch: claws churn in circles at chest height
      float w = 6.2831853f * b * 0.5f;
      p.au[0] = 70.f + 25.f * cosf(w); p.as[0] = 80.f + 30.f * sinf(w);
      p.au[1] = 70.f + 25.f * cosf(w + 3.14f); p.as[1] = 80.f + 30.f * sinf(w + 3.14f);
      p.claw[0] = p.claw[1] = 0.2f; p.torso = 5.f * sinf(w); p.x = 6.f * sinf(w); p.y = 3.f * dip(f); p.head = -4.f * sinf(w);
      break;
    }
    case M_TUT: {                                                 // King Tut: crisp 90-degree geometry, a new shape every half beat
      static const float T[6][4] = {{90, 0, 90, 90}, {0, 90, 90, 90}, {90, 90, 0, 90}, {150, 60, 30, 120}, {30, 120, 150, 60}, {90, 180, 90, 0}};
      int k = ((int)floorf(b * 2.f)) % 6; float h = hit(fmodf(b * 2.f, 1.f));
      p.au[0] = T[k][0]; p.as[0] = T[k][1]; p.au[1] = T[k][2]; p.as[1] = T[k][3];
      p.claw[0] = p.claw[1] = 0.f; p.head = (k & 1 ? 8.f : -8.f); p.hx = (k & 1 ? 3.f : -3.f); p.y = 1.5f * h; p.strain = 1.f;
      break;
    }
    case M_SHLEAN: {                                              // shoulder lean: lean, hold, lean back
      float l = sgn * (0.6f + 0.4f * (1.f - hit(f)));
      p.torso = 11.f * l; p.x = 8.f * l; p.head = -6.f * l; p.hx = -2.f * l; p.abd = -10.f * l;
      p.au[0] = 25.f; p.au[1] = 25.f; p.as[0] = p.as[1] = 40.f; p.claw[0] = p.claw[1] = 0.2f; p.y = 2.f * dip(f);
      break;
    }
    case M_TOPROCK: {                                             // b-boy toprock: cross-step in front, arms open-close
      float q = fmodf(b, 2.f) / 2.f; int ph = (int)(q * 4.f);
      p.fx[ph & 3] = (ph & 1 ? 8.f : -8.f) * sinf(3.14159f * fmodf(q * 4.f, 1.f)); p.fy[ph & 3] = -8.f * sinf(3.14159f * fmodf(q * 4.f, 1.f));
      float open = sinf(3.14159f * q * 2.f);
      p.au[0] = p.au[1] = 40.f + 40.f * open; p.as[0] = p.as[1] = 70.f - 40.f * open; p.claw[0] = p.claw[1] = 0.3f + 0.5f * open;
      p.torso = 6.f * sinf(6.2831853f * q); p.y = 3.f * dip(f); p.head = 5.f * sgn;
      break;
    }
    // ---- metal ----
    case M_WINDMILL: {                                            // windmill headbang: the whole top half circles
      float w = 6.2831853f * b * 0.5f;
      p.head = 22.f * sinf(w); p.hy = 6.f + 5.f * cosf(w); p.torso = 10.f * sinf(w); p.abd = -12.f * sinf(w);
      p.au[0] = p.au[1] = 30.f; p.as[0] = p.as[1] = 20.f; p.claw[0] = p.claw[1] = 0.1f; p.y = 4.f + 2.f * dip(f); p.sq = 0.4f;
      break;
    }
    case M_STOMP: {                                               // power stance: wide, stomp on the kick, claws pumping low
      float h = hit(f);
      p.fx[0] = p.fx[1] = -6.f; p.fx[2] = p.fx[3] = 6.f; p.fy[(bi & 1) * 2] = -9.f * (1.f - h) * (f < 0.5f ? 1.f : 0.f);
      p.y = 5.f + 3.f * h; p.sq = 1.f * h; p.head = 8.f * h; p.hy = 4.f * h;
      p.au[0] = p.au[1] = 55.f + 20.f * h; p.as[0] = p.as[1] = 60.f - 30.f * h; p.claw[0] = p.claw[1] = 0.05f + 0.6f * (1.f - h);
      break;
    }
    case M_MOSH: {                                                // chaotic jump, limbs everywhere
      float air = sinf(3.14159f * f);
      p.y = -12.f * air * e; for (int i = 0; i < 4; i++) p.fy[i] = p.y * (i & 1 ? 0.8f : 1.f) - 3.f * air * ((bi + i) & 1);
      p.au[0] = 60.f + 70.f * air * ((bi & 1) ? 1.f : 0.3f); p.au[1] = 60.f + 70.f * air * ((bi & 1) ? 0.3f : 1.f);
      p.as[0] = p.as[1] = 40.f + 40.f * air; p.head = 14.f * sgn * air; p.torso = 8.f * sgn * air; p.claw[0] = p.claw[1] = 0.8f * air;
      p.mouth = 0.5f * air; p.strain = air;
      break;
    }
    // ---- country ----
    case M_HEELTOE: {                                             // heel, toe, heel, toe
      int q = ((int)floorf(b * 2.f)) & 3; float u = sinf(3.14159f * fmodf(b * 2.f, 1.f));
      int foot = (q < 2) ? 0 : 2; p.fx[foot] = (q & 1 ? 5.f : -5.f) * u; p.fy[foot] = -5.f * u;
      p.au[0] = p.au[1] = 18.f; p.as[0] = p.as[1] = 95.f; p.claw[0] = p.claw[1] = 0.05f;      // claws hooked in the "belt"
      p.head = 5.f * sgn; p.torso = 3.f * sgn; p.y = 2.f * dip(f);
      break;
    }
    case M_GRAPEVINE: {                                           // side, behind, side, touch - travelling
      float s = sinf(3.14159f * b * 0.5f); p.x = 14.f * s;
      int q = ((int)floorf(b)) & 3; float u = sinf(3.14159f * f);
      p.fx[q] = (q & 1 ? -7.f : 7.f) * u; p.fy[q] = -6.f * u;
      p.au[0] = 25.f + 25.f * fmaxf(0.f, s); p.au[1] = 25.f + 25.f * fmaxf(0.f, -s); p.as[0] = p.as[1] = 35.f;
      p.head = 7.f * s; p.torso = -4.f * s; p.claw[0] = p.claw[1] = 0.35f;
      break;
    }
    case M_HOEDOWN: {                                             // arms swinging like a fiddle tune, hips going
      float s = sinf(6.2831853f * b);
      p.au[0] = 45.f + 45.f * s; p.au[1] = 45.f - 45.f * s; p.as[0] = 40.f - 20.f * s; p.as[1] = 40.f + 20.f * s;
      p.torso = 4.f * s; p.abd = 14.f * s; p.x = 3.f * s; p.y = 3.f * dip(f); p.head = 6.f * s;
      p.fy[(bi & 1) * 2] = -7.f * sinf(3.14159f * f); p.claw[0] = p.claw[1] = 0.45f;
      break;
    }
    case M_SWAGGER: {                                             // thumbs-in-belt strut: slow, cocky side-to-side
      float s = sinf(3.14159f * b);
      p.x = 7.f * s; p.torso = -6.f * s; p.head = 8.f * s + 3.f; p.hx = 2.f * s;
      p.au[0] = p.au[1] = 16.f; p.as[0] = p.as[1] = 100.f; p.claw[0] = p.claw[1] = 0.05f;
      p.fy[s > 0 ? 0 : 2] = -4.f * fabsf(s); p.y = 2.f * dip(f);
      break;
    }
    // ---- pop / EDM ----
    case M_PUMP: {                                                // fist (claw) pump on every beat
      float h = hit(f); int a = bi & 1;
      p.au[a] = 130.f + 25.f * h; p.as[a] = 20.f; p.claw[a] = 0.05f;
      p.au[1 - a] = 30.f; p.as[1 - a] = 50.f; p.claw[1 - a] = 0.3f;
      p.y = 4.f * h; p.sq = 0.6f * h; p.head = -6.f * h; p.mouth = 0.4f * h; p.strain = 1.f;
      break;
    }
    case M_JUMPUP: {                                              // jump on every beat, claws up
      float air = sinf(3.14159f * f);
      p.y = -10.f * air * (0.6f + 0.4f * e); for (int i = 0; i < 4; i++) p.fy[i] = p.y;
      p.au[0] = p.au[1] = 120.f + 30.f * air; p.as[0] = p.as[1] = 10.f; p.claw[0] = p.claw[1] = 0.6f + 0.4f * air;
      p.sq = 0.8f * hit(f) - 0.4f * air; p.strain = 1.f;
      break;
    }
    case M_ARMWAVE: {                                             // the wave: rolls from one claw tip, across, to the other
      float w = 6.2831853f * b * 0.5f;
      p.as[0] = 40.f + 50.f * sinf(w); p.au[0] = 70.f + 30.f * sinf(w - 0.8f);
      p.au[1] = 70.f + 30.f * sinf(w - 2.0f); p.as[1] = 40.f + 50.f * sinf(w - 2.8f);
      p.head = 8.f * sinf(w - 1.4f); p.torso = 4.f * sinf(w - 1.4f); p.claw[0] = 0.4f + 0.4f * sinf(w + 0.8f); p.claw[1] = 0.4f + 0.4f * sinf(w - 3.6f);
      break;
    }
    case M_SHUFFLE2: {                                            // Melbourne shuffle: running man + T-step at double time
      float q = b * 2.f, qf = q - floorf(q); int qi = ((int)floorf(q)) & 3; float u = sinf(3.14159f * qf);
      if (qi < 2) { p.fy[qi * 2] = -9.f * u; p.fx[qi * 2] = (qi ? 6.f : -6.f) * u; } else { p.fx[(qi - 2) * 2 + 1] = (qi & 1 ? 8.f : -8.f) * u; }
      p.x = 6.f * sinf(3.14159f * b * 0.5f); p.y = 2.f * u;
      p.au[0] = 45.f + 25.f * u; p.au[1] = 45.f + 25.f * (1.f - u); p.as[0] = p.as[1] = 70.f; p.claw[0] = p.claw[1] = 0.2f;
      break;
    }
    case M_SWAYUP: {                                              // hands in the air, swaying
      float s = sinf(3.14159f * b * 0.5f);
      p.au[0] = p.au[1] = 150.f; p.as[0] = p.as[1] = -10.f + 15.f * s; p.claw[0] = p.claw[1] = 0.7f;
      p.torso = 8.f * s; p.x = 8.f * s; p.head = 10.f * s; p.y = 2.f * dip(f); p.strain = 1.f;
      break;
    }
    // ---- dubstep ----
    case M_WOBBLE: {                                              // the whole body wobbles with the sub, half-time
      float wob = sinf(6.2831853f * b * 2.f) * (0.5f + aud::bass);
      p.x = 5.f * wob; p.torso = 7.f * wob; p.abd = -14.f * wob; p.head = -9.f * wob; p.y = 5.f + 2.f * fabsf(wob); p.sq = 0.5f;
      p.au[0] = p.au[1] = 55.f; p.as[0] = p.as[1] = 90.f + 20.f * wob; p.claw[0] = p.claw[1] = 0.3f + 0.3f * wob;
      p.fx[0] = p.fx[1] = -5.f; p.fx[2] = p.fx[3] = 5.f;
      break;
    }
    case M_GLITCH: {                                              // stutter: snaps between micro-poses on 1/8ths, freezes on 1/16ths
      int k = ((int)floorf(b * 4.f)) & 7; uint32_t hsh = (uint32_t)(k * 2654435761u + ((int)floorf(b)) * 97u);
      p.au[0] = 30.f + (hsh % 110); p.au[1] = 30.f + ((hsh >> 8) % 110); p.as[0] = 20.f + ((hsh >> 16) % 100); p.as[1] = 20.f + ((hsh >> 4) % 100);
      p.head = (float)((int)(hsh % 30) - 15); p.torso = (float)((int)((hsh >> 12) % 12) - 6);
      p.claw[0] = (hsh & 1) ? 1.f : 0.f; p.claw[1] = (hsh & 2) ? 1.f : 0.f; p.strain = 1.f;
      break;
    }
    case M_SLOWMO: {                                              // everything in slow motion (it takes 4 beats to do one thing)
      float w = 6.2831853f * b * 0.25f;
      p.au[0] = 60.f + 50.f * sinf(w); p.au[1] = 60.f + 50.f * sinf(w + 1.5f); p.as[0] = 60.f + 40.f * sinf(w + 0.7f); p.as[1] = 60.f + 40.f * sinf(w + 2.2f);
      p.head = 10.f * sinf(w + 0.4f); p.torso = 6.f * sinf(w); p.x = 6.f * sinf(w); p.y = 2.f * sinf(w * 2.f); p.claw[0] = p.claw[1] = 0.5f + 0.4f * sinf(w);
      break;
    }
    case M_LIQUID: {                                              // liquid arms: waves travelling through both arms, opposite phase
      float w = 6.2831853f * b * 0.5f;
      p.au[0] = 50.f + 35.f * sinf(w); p.as[0] = 60.f + 45.f * sinf(w - 1.2f); p.claw[0] = 0.4f + 0.5f * sinf(w - 2.4f);
      p.au[1] = 50.f - 35.f * sinf(w); p.as[1] = 60.f - 45.f * sinf(w - 1.2f); p.claw[1] = 0.4f - 0.5f * sinf(w - 2.4f);
      p.torso = 4.f * sinf(w - 0.6f); p.head = 7.f * sinf(w - 1.8f); p.abd = -8.f * sinf(w - 0.6f);
      break;
    }
    // ---- soft / pure mantis ----
    case M_TWIG: {                                                // a real mantis trick: rocking like a twig in the breeze
      float s = sinf(6.2831853f * b * 0.25f) + 0.3f * sinf(6.2831853f * b * 0.61f);
      p.x = 7.f * s; p.torso = 8.f * s; p.head = 4.f * s; p.abd = 6.f * s; p.y = -1.f;
      p.au[0] = p.au[1] = 12.f + 3.f * s; p.as[0] = p.as[1] = 6.f; p.claw[0] = p.claw[1] = 0.05f;   // raptorial legs folded, poised
      break;
    }
    case M_PRAY: {                                                // the praying pose, bowing gently on the beat
      float d = dip(f);
      p.au[0] = p.au[1] = 10.f + 4.f * d; p.as[0] = p.as[1] = 4.f; p.claw[0] = p.claw[1] = 0.05f;
      p.hy = 3.f * (1.f - d); p.head = 3.f * sinf(3.14159f * b * 0.5f); p.y = 1.f * (1.f - d); p.torso = 2.f * sinf(3.14159f * b * 0.5f);
      break;
    }
    case M_STALK: {                                               // stalking: slow, careful steps, head locked on something
      float q = fmodf(b, 4.f) / 4.f; int step = ((int)floorf(b)) & 3; float u = sinf(3.14159f * fmodf(b, 1.f));
      p.fy[step] = -5.f * u; p.fx[step] = 3.f * u; p.x = -10.f + 20.f * q;
      p.au[0] = p.au[1] = 26.f; p.as[0] = p.as[1] = 18.f; p.claw[0] = p.claw[1] = 0.3f;              // poised, a little forward
      p.head = -p.x * 0.6f; p.torso = 2.f; p.y = 3.f;
      break;
    }
    // ---- flourish + freeze ----
    case M_TRILL: {                                               // a ridiculously fast little shuffle (hi-hat trills)
      float q = b * 8.f, qf = q - floorf(q); int qi = ((int)floorf(q)) & 3; float u = sinf(3.14159f * qf);
      p.fy[qi] = -6.f * u; p.fx[qi] = (qi & 1 ? 4.f : -4.f) * u; p.x = 3.f * sinf(6.2831853f * b);
      p.au[0] = p.au[1] = 35.f; p.as[0] = p.as[1] = 60.f; p.claw[0] = p.claw[1] = 0.3f; p.y = 1.5f * u; p.head = 2.f * sinf(6.2831853f * b * 4.f);
      break;
    }
    case M_FREEZE: {                                              // held pose on a silence break
      static const float FZ[5][7] = {                             // au0 au1 as0 as1 claw torso head
        {150, 60, 20, 120, 1.f, 8, -10}, {90, 90, 0, 0, 0.f, 0, 0}, {140, 140, 40, 40, 1.f, 0, 12}, {12, 12, 5, 5, 0.05f, 0, 0}, {160, 20, -20, 30, 0.f, -10, 8}};
      const float *z = FZ[s_freezeVar % 5];
      p.au[0] = z[0]; p.au[1] = z[1]; p.as[0] = z[2]; p.as[1] = z[3]; p.claw[0] = p.claw[1] = z[4]; p.torso = z[5]; p.head = z[6];
      p.y = 3.f; p.sq = 0.3f; p.strain = 1.f;
      break;
    }
    // ---- new emotes ----
    case M_E_HORNS:                                               // devil horns, both claws open overhead
      p.au[0] = p.au[1] = 145.f; p.as[0] = p.as[1] = 10.f; p.claw[0] = p.claw[1] = 1.f; p.head = 10.f * sinf(12.f * b); p.hy = 3.f; p.mouth = 0.7f; p.strain = 1.f;
      break;
    case M_E_LASSO: {                                             // one claw twirls a lasso overhead
      float w = 6.2831853f * b * 2.f;
      p.au[1] = 150.f + 10.f * cosf(w); p.as[1] = 20.f + 40.f * sinf(w); p.claw[1] = 0.8f;
      p.au[0] = 18.f; p.as[0] = 100.f; p.claw[0] = 0.05f; p.head = 6.f; p.mouth = 0.5f; p.x = 3.f * sinf(w * 0.5f); p.strain = 1.f;
      break;
    }
    case M_E_OVERCLAP: {                                          // clap overhead, twice
      float c = fmodf(b * 2.f, 1.f), cl = hit(c);
      p.au[0] = p.au[1] = 150.f; p.as[0] = p.as[1] = 60.f - 30.f * (1.f - cl); p.claw[0] = p.claw[1] = 0.2f; p.y = 2.f * cl; p.mouth = 0.4f; p.strain = 1.f;
      break;
    }
    case M_E_STRIKE: {                                            // lightning-fast mantis strike and snap back
      float h = f < 0.15f ? f / 0.15f : fmaxf(0.f, 1.f - (f - 0.15f) * 2.f);
      int a = bi & 1;
      p.au[a] = 12.f + 78.f * h; p.as[a] = 5.f - 15.f * h; p.claw[a] = h > 0.8f ? 1.f : 0.f;
      p.au[1 - a] = 12.f; p.as[1 - a] = 5.f; p.claw[1 - a] = 0.05f; p.head = (a ? 10.f : -10.f) * h; p.torso = (a ? 6.f : -6.f) * h;
      break;
    }
    case M_E_GROOM: {                                             // grooming: a claw wipes across the big eyes
      float w = sinf(6.2831853f * b * 2.f);
      p.au[0] = 115.f + 10.f * w; p.as[0] = 125.f + 15.f * w; p.claw[0] = 0.3f;
      p.au[1] = 30.f; p.as[1] = 70.f; p.claw[1] = 0.2f; p.head = -8.f + 6.f * w; p.hx = -2.f;
      break;
    }
    case M_E_SWIVEL: {                                            // that iconic mantis head swivel: snap, hold, snap
      int k = ((int)floorf(b * 3.f)) % 3; static const float SV[3] = {-24.f, 24.f, 0.f};
      p.head = SV[k]; p.hx = SV[k] * 0.15f; p.au[0] = p.au[1] = 12.f; p.as[0] = p.as[1] = 5.f; p.claw[0] = p.claw[1] = 0.05f;
      break;
    }
    case M_E_THREAT: {                                            // threat display: rears up, arms high and wide, claws open
      float r2 = sinf(3.14159f * fminf(1.f, f * 2.f));
      p.y = -4.f * r2; p.au[0] = p.au[1] = 120.f + 20.f * r2; p.as[0] = p.as[1] = 30.f; p.claw[0] = p.claw[1] = 1.f;
      p.abd = 20.f * r2; p.head = 0; p.mouth = 0.8f * r2; p.strain = 1.f;
      break;
    }
    case M_E_SHRUG:
      p.au[0] = p.au[1] = 50.f; p.as[0] = p.as[1] = -10.f; p.claw[0] = p.claw[1] = 0.8f; p.hy = -3.f * sinf(3.14159f * f); p.head = 10.f; p.mouth = 0.2f;
      break;
    case M_E_DROP: {                                              // the drop hits: explode outward
      float h = hit(f);
      p.au[0] = p.au[1] = 100.f + 50.f * (1.f - h); p.as[0] = p.as[1] = 20.f; p.claw[0] = p.claw[1] = 1.f;
      p.y = -8.f * (1.f - h); for (int i = 0; i < 4; i++) p.fy[i] = p.y; p.fx[0] = p.fx[1] = -8.f; p.fx[2] = p.fx[3] = 8.f;
      p.mouth = 0.9f; p.sq = -0.5f; p.strain = 1.f;
      break;
    }
    case M_TALK:                                                  // chatting at the cave mouth
      p.head = sinf(t * 1.7f) * 5.f + s_look * 8.f; p.hy = -1.5f * s_mouthOpen;
      p.torso = sinf(t * 0.6f) * 2.f; p.abd = sinf(t * 1.1f) * 5.f;
      p.au[0] = 20.f + 8.f * sinf(t * 1.3f); p.au[1] = 22.f + 30.f * s_mouthOpen;
      p.as[0] = 12.f; p.as[1] = 20.f + 25.f * s_mouthOpen;
      p.claw[0] = 0.3f; p.claw[1] = 0.35f + 0.4f * s_mouthOpen;
      p.y = sinf(t * 1.4f) * 1.2f;
      break;
    case M_LEAN:                                                  // leaning into the cave, claws cupped round the mouth
      p.torso = 13.f; p.x = 12.f; p.head = 14.f; p.hx = 3.f; p.abd = -10.f;
      p.au[0] = p.au[1] = 108.f; p.as[0] = p.as[1] = 98.f;
      p.claw[0] = p.claw[1] = 0.55f;
      p.fy[0] = p.fy[1] = -3.f; p.y = 1.f + sinf(t * 3.f) * 0.6f;
      break;
    case M_LISTEN:                                                // head cocked, a claw cupped at the ear
      p.head = 17.f; p.torso = 6.f; p.x = 6.f; p.abd = -6.f;
      p.au[1] = 138.f; p.as[1] = 118.f; p.claw[1] = 0.7f;
      p.au[0] = 14.f; p.as[0] = 10.f; p.claw[0] = 0.3f;
      p.y = sinf(t * 1.2f) * 0.8f;
      break;
  }
}

// ---------- behaviour state ----------
static int s_move = M_IDLE, s_prevMove = M_IDLE;
static float s_blend = 1.f;
static int s_lastBar = -1;
static float s_acc = 0;
static float s_blinkT = 2.f, s_blink = 0;
static float s_happy = 0;                 // after petting: ^^ eyes + blush
static float s_idleBeat = 0;
static float s_idleWaveT = 6.f;
static float s_dizzy = 0;
static uint32_t s_touchUntil = 0;
static bool s_lastSing = false;

struct Fx { float x, y, vx, vy, life; uint8_t kind; float hue; };
static Fx s_fx[24];
static void spawnFx(float x, float y, uint8_t kind, float hue) {
  for (auto &p : s_fx) if (p.life <= 0) {
    p.x = x; p.y = y; p.kind = kind; p.hue = hue; p.life = 1.f;
    p.vx = (float)((int)(esp_random() % 61) - 30) * 0.9f;
    p.vy = -30.f - (float)(esp_random() % 30);
    return;
  }
}

// ---------------- the choreographer ----------------
enum Style : uint8_t { ST_GROOVE = 0, ST_POPLOCK, ST_FOOTWORK, ST_HYPE, ST_SMOOTH };
static uint8_t s_style = ST_GROOVE;
static float s_onsetT[12] = {-99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99}; static int s_onsetI = 0;   // recent bright transients
static int s_emoteCool = 0;
static int s_strong = 0, s_lastBeatI = -1, s_emoteUntilBeat = -1, s_emote = -1, s_lastEmote = -1;
static float s_beatAcc = 0, s_footUntil = 0;
static int styleMove(uint8_t st) {
  static const uint8_t G[] = {M_BOB, M_SWAY, M_SHUFFLE, M_WAVE_L, M_WAVE_R, M_BOB};
  static const uint8_t P[] = {M_POP, M_ROBOT, M_POP, M_BANG};
  static const uint8_t F[] = {M_RUNMAN, M_TSTEP, M_RUNMAN, M_SHUFFLE};
  static const uint8_t Hh[] = {M_HYPE, M_CHEER, M_BANG, M_HYPE, M_SPREAD};
  static const uint8_t S[] = {M_MOON, M_BODYWAVE, M_SWAY, M_BODYWAVE, M_SPREAD};
  const uint8_t *L; int n;
  switch (st) { case ST_POPLOCK: L = P; n = 4; break; case ST_FOOTWORK: L = F; n = 4; break; case ST_HYPE: L = Hh; n = 5; break; case ST_SMOOTH: L = S; n = 5; break; default: L = G; n = 6; }
  for (int k = 0; k < 6; k++) { int m = L[esp_random() % n]; if (m != s_move) return m; }
  return L[0];
}
static uint8_t chooseStyle(float e) {
  float bright = aud::centroid, hits = aud::beatConf;
  if (e > 0.75f && hits > 0.5f) return ST_HYPE;
  if (e > 0.45f && bright > 0.45f) return (esp_random() & 1) ? ST_POPLOCK : ST_HYPE;
  if (e < 0.3f) return (esp_random() % 3 == 0) ? ST_GROOVE : ST_SMOOTH;
  static const uint8_t mix[] = {ST_GROOVE, ST_POPLOCK, ST_SMOOTH, ST_GROOVE};
  return mix[esp_random() % 4];
}
// ---------------- V25: the feel model. Not a genre classifier - abstractions that moves can hang on ----------------
enum Fam : uint8_t { F_GROOVE = 0, F_HIPHOP, F_METAL, F_COUNTRY, F_EDM, F_DUBSTEP, F_SOFT, F_COUNT };
struct Feel {
  float four = 0, back = 0, off = 0, hatRate = 0, dist = 0, wob = 0, eSlow = 0, eFast = 0, eBar = 0, ePrevBar = 0;
  float dens = 0, sus = 0.5f, tonal = 0, beatMin = 9, beatMax = 0, drops = 0;
  bool kickEarly = false, snareBeat = false, offHit = false; float bassPrev = 0, wobAcc = 0;
};
static Feel s_feel;
static uint8_t s_fam = F_GROOVE, s_famCand = F_GROOVE; static int s_famVotes = 0;
static float s_quietT = 0;
static bool s_frozen = false; static float s_dropT = -9, s_build = 0, s_breakdown = 0, s_singAlong = 0, s_singCool = 0, s_vocalT = 0, s_trillT = -9;
static inline float gauss(float x, float mu, float sd) { float z = (x - mu) / sd; return expf(-0.5f * z * z); }
static void feelFrame(float dt, float b) {
  Feel &F = s_feel;
  float fr = b - floorf(b);
  if (aud::kick > 0 && (fr < 0.22f || fr > 0.85f)) F.kickEarly = true;
  if (aud::snare > 0.2f) F.snareBeat = true;
  if (aud::onset > 0.2f && fr > 0.35f && fr < 0.65f) F.offHit = true;
  F.hatRate += ((aud::hat > 0 ? 1.f / fmaxf(dt, 0.01f) : 0.f) - F.hatRate) * clampf(dt * 1.2f, 0.f, 1.f);
  F.dist += (clampf((aud::flat - 0.35f) * 2.2f, 0.f, 1.f) * clampf(aud::level * 2.f, 0.f, 1.f) - F.dist) * clampf(dt * 0.3f, 0.f, 1.f);
  float db = fabsf(aud::bass - F.bassPrev) / fmaxf(dt, 0.01f); F.bassPrev = aud::bass;   // sub wobble: bass that keeps moving
  F.wob += (clampf(db * 0.25f, 0.f, 1.f) * clampf(aud::bass * 2.f, 0.f, 1.f) - F.wob) * clampf(dt * 0.4f, 0.f, 1.f);
  F.eSlow += (aud::level - F.eSlow) * clampf(dt * 0.25f, 0.f, 1.f);
  F.eFast += (aud::level - F.eFast) * clampf(dt * 5.f, 0.f, 1.f);
  F.eBar = fmaxf(F.eBar, F.eFast);
  F.dens += ((aud::onset > 0 ? 1.f / fmaxf(dt, 0.01f) : 0.f) - F.dens) * clampf(dt * 0.5f, 0.f, 1.f);   // hits per second
  F.tonal += ((1.f - aud::flat) - F.tonal) * clampf(dt * 0.3f, 0.f, 1.f);
  F.beatMin = fminf(F.beatMin, aud::level); F.beatMax = fmaxf(F.beatMax, aud::level);
}
static void feelBeat(int bi) {                                  // once per beat
  Feel &F = s_feel;
  F.four += ((F.kickEarly ? 1.f : 0.f) - F.four) * 0.12f;
  F.drops *= 0.995f;
  bool twoFour = (bi & 1) == 1;
  if (F.snareBeat) F.back += ((twoFour ? 1.f : -0.6f) - F.back) * 0.12f; else F.back *= 0.97f;
  F.off += ((F.offHit ? 1.f : 0.f) - F.off) * 0.1f;
  F.kickEarly = F.snareBeat = F.offHit = false;
  if (F.beatMax > 0.05f) F.sus += (F.beatMin / F.beatMax - F.sus) * 0.1f;     // sustain: how full it stays between hits
  F.beatMin = 9; F.beatMax = 0;
  if ((bi & 3) == 0) { s_build = clampf(s_build + (F.eBar > F.ePrevBar * 1.08f && F.eBar > 0.25f ? 0.25f : -0.35f), 0.f, 1.f); F.ePrevBar = F.eBar; F.eBar = 0; }
}
static uint8_t scoreFamily() {
  // Abstractions that generalise: hit density, sustain (wall of sound vs punchy), tonality, loudness,
  // kick-on-every-beat, backbeat, syncopation, brightness. No single cue decides; each family is a blend.
  Feel &F = s_feel;
  float loud = clampf(F.eSlow * 1.6f, 0.f, 1.f), dense = clampf((F.dens - 2.f) / 4.f, 0.f, 1.f), sparse = 1.f - clampf((F.dens - 1.f) / 3.f, 0.f, 1.f);
  float wall = clampf((F.sus - 0.25f) / 0.45f, 0.f, 1.f), punchy = 1.f - wall, tonal = clampf((F.tonal - 0.3f) / 0.4f, 0.f, 1.f);
  float dark = clampf((0.3f - aud::centroid) / 0.2f, 0.f, 1.f), back = clampf(F.back * 2.f, 0.f, 1.f);
  float sc[F_COUNT];
  sc[F_GROOVE] = 0.3f;
  sc[F_METAL] = wall * loud * (1.f - tonal) * (0.6f + 0.4f * dense) * (1.f - 0.6f * dark) * 1.6f;
  sc[F_DUBSTEP] = wall * loud * (0.5f + 0.5f * sparse) * (0.3f + 0.7f * dark) * (1.f - tonal) * 1.3f + 0.2f * F.wob + 0.8f * F.drops * (0.3f + 0.7f * dark);
  sc[F_EDM] = F.four * (0.4f + 0.6f * punchy) * (1.f - dense * 0.5f) * (1.f - tonal * 0.5f) * 1.1f;
  sc[F_HIPHOP] = punchy * (1.f - F.four) * (0.4f + back) * (1.f - tonal * 0.6f) * (0.5f + 0.5f * loud) * 1.2f;
  sc[F_COUNTRY] = tonal * (0.3f + back + 0.4f * F.off) * (1.f - dense) * (1.f - dark * 0.5f) * 1.1f;
  sc[F_SOFT] = clampf(0.45f - F.eSlow, 0.f, 0.3f) * 3.2f * (0.5f + 0.5f * tonal);
  int best = 0; for (int k = 1; k < F_COUNT; k++) if (sc[k] > sc[best]) best = k;
  return (uint8_t)best;
}
static const uint8_t FAM_MOVES[F_COUNT][10] = {
  {M_BOB, M_SWAY, M_SHUFFLE, M_NOD, M_TWOSTEP, M_ARMWAVE, M_WAVE_L, M_WAVE_R, M_BODYWAVE, M_POP},
  {M_NOD, M_TWOSTEP, M_CABBAGE, M_TUT, M_SHLEAN, M_TOPROCK, M_POP, M_ROBOT, M_RUNMAN, M_NOD},
  {M_BANG, M_WINDMILL, M_STOMP, M_MOSH, M_HYPE, M_BANG, M_STOMP, M_WINDMILL, M_MOSH, M_CHEER},
  {M_HEELTOE, M_GRAPEVINE, M_HOEDOWN, M_SWAGGER, M_SHUFFLE, M_TSTEP, M_HEELTOE, M_HOEDOWN, M_SWAY, M_BOB},
  {M_PUMP, M_JUMPUP, M_ARMWAVE, M_SHUFFLE2, M_SWAYUP, M_HYPE, M_CHEER, M_RUNMAN, M_TSTEP, M_PUMP},
  {M_WOBBLE, M_GLITCH, M_SLOWMO, M_LIQUID, M_POP, M_ROBOT, M_BODYWAVE, M_WOBBLE, M_GLITCH, M_TUT},
  {M_TWIG, M_PRAY, M_SWAY, M_BODYWAVE, M_MOON, M_STALK, M_SPREAD, M_TWIG, M_LIQUID, M_SLOWMO},
};
static const uint8_t FAM_EMOTES[F_COUNT][6] = {
  {M_E_CLAP, M_E_SNAP, M_E_POINT, M_E_PEACE, M_E_SWIVEL, M_E_GROOM},
  {M_E_POINT, M_E_FLEX, M_E_SNAP, M_E_SHRUG, M_E_SWIVEL, M_E_STRIKE},
  {M_E_HORNS, M_E_GUITAR, M_E_THREAT, M_E_STRIKE, M_E_HORNS, M_E_FLEX},
  {M_E_LASSO, M_E_CLAP, M_E_OVERCLAP, M_E_SHRUG, M_E_LASSO, M_E_POINT},
  {M_E_OVERCLAP, M_E_PEACE, M_E_POINT, M_E_CLAP, M_E_DROP, M_E_SNAP},
  {M_E_DROP, M_E_STRIKE, M_E_SWIVEL, M_E_THREAT, M_E_SNAP, M_E_GROOM},
  {M_E_BOW, M_E_GROOM, M_E_SWIVEL, M_E_BOW, M_E_GROOM, M_E_SWIVEL},
};
static int famMove(uint8_t fam) {
  // syncopated grooves lean toward isolations; build-ups toward the hype end of the list
  if (s_feel.off > 0.55f && (esp_random() % 3) == 0) { static const uint8_t ISO[] = {M_TUT, M_GLITCH, M_POP, M_ROBOT}; int mv = ISO[esp_random() % 4]; if (mv != s_move) return mv; }
  if ((esp_random() % 100) < 55) {                                // the spine: groove steps (the body layers make them dance)
    static const uint8_t CORE[F_COUNT][4] = {{M_BOB, M_SWAY, M_NOD, M_TWOSTEP}, {M_NOD, M_TWOSTEP, M_SHLEAN, M_BOB}, {M_BANG, M_STOMP, M_NOD, M_BANG},
      {M_SWAGGER, M_HEELTOE, M_SWAY, M_TWOSTEP}, {M_BOB, M_TWOSTEP, M_SWAY, M_NOD}, {M_BODYWAVE, M_NOD, M_SWAY, M_WOBBLE}, {M_SWAY, M_TWIG, M_BODYWAVE, M_PRAY}};
    for (int k = 0; k < 6; k++) { int mv = CORE[fam][esp_random() % 4]; if (mv != s_move) return mv; }
  }
  for (int k = 0; k < 8; k++) { int mv = FAM_MOVES[fam][esp_random() % 10]; if (mv != s_move) return mv; }
  return FAM_MOVES[fam][0];
}
static int pickMove(float e) {
  static const uint8_t low[] = {M_BOB, M_SWAY, M_BOB, M_SHUFFLE};
  static const uint8_t mid[] = {M_SWAY, M_WAVE_L, M_SHUFFLE, M_WAVE_R, M_BANG, M_BOB};
  static const uint8_t high[] = {M_CHEER, M_BANG, M_SPREAD, M_SHUFFLE, M_CHEER, M_SWAY};
  for (int tries = 0; tries < 6; tries++) {
    int m;
    if (e < 0.35f) m = low[esp_random() % sizeof(low)];
    else if (e < 0.7f) m = mid[esp_random() % sizeof(mid)];
    else m = high[esp_random() % sizeof(high)];
    if (m != s_move) return m;
  }
  return M_BOB;
}
static void setMove(int m) {
#ifdef CHOREO_LOG
  printf("t=%5.1f style=%d move=%d%s\n", s_t, s_style, m, m >= M_E_CLAP ? "  <- EMOTE" : "");
#endif
  if (m == s_move) return;
  s_prevMove = s_move; s_move = m; s_blend = 0.f;
  danceNetMove(mnMoveOf(m), 200);
}

void mantisDrawCave() { s_cave = true; mantisDraw(true); s_cave = false; }

void mantisTap(int x, int y) {
  // head region in screen space (approx; uses last frame's layout)
  float hx = s_rx, hy = s_ry - 150.f * s_Z;
  float dx = x - hx, dy = y - hy;
  if (dx * dx + dy * dy < (60.f * s_Z) * (60.f * s_Z) + 400.f) {
    s_happy = 1.6f; s_acc = 1.f;
    hap(70, 60);
    for (int i = 0; i < 4; i++) spawnFx((float)x, (float)y, 1, 330.f);
    if ((esp_random() & 3) == 0) aud::sfx(1.7f);
  } else {
    s_acc = 0.7f;
    for (int i = 0; i < 2; i++) spawnFx((float)x, (float)y, 2, g_hue);
    hap(40, 20);
  }
}

// ---------- stage (fx buffer) ----------
static void drawStage(bool sing, float beatF, float e) {
  static float rot = 0;
  rot += g_dt * (0.25f + e * 1.2f) * 40.f;
  int cx = 80 + (int)(g_lookX * -10.f), cy = sing ? 34 : 52;
  int floorY = sing ? 200 : (int)((s_ry + 85.f * s_Z) * 0.5f);
  uint8_t r8 = (uint8_t)rot;
  int pulseR = (int)(beatF * 90.f);
  int glow = 40 + (int)(aud::bass * 60.f + s_acc * 30.f);
  auto rows_ = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
    uint8_t *row = fx::buf + y * fx::LW;
    if (y >= floorY) {
      int d = y - floorY + 1;
      int z = 900 / (d + 2);
      for (int x = 0; x < fx::LW; x++) {
        int u = ((x - 80) * z) >> 6;
        int chk = ((u >> 3) ^ ((z + (int)(s_t * 30.f)) >> 3)) & 1;
        int v = (chk ? 70 : 28) + (glow >> 2) - (d >> 1);
        row[x] = (uint8_t)(v < 8 ? 8 : (v > 200 ? 200 : v));
      }
      continue;
    }
    for (int x = 0; x < fx::LW; x++) {
      uint16_t t = fx::tunAt(x - cx, y - cy);
      int a = t >> 8;
      int r = fx::radAt(x - cx, y - cy);
      int ray = fx::sn[(uint8_t)(a * 6 + r8)] > 40 ? 26 : 12;
      int ringd = abs(r - pulseR);
      int ring = ringd < 4 ? (4 - ringd) * 14 : 0;
      int g = glow - r / 2; if (g < 0) g = 0;
      int v = ray + ring + g;
      row[x] = (uint8_t)(v > 180 ? 180 : v);
    }
  }
  };
  fx::parallel(rows_);
  float hsh = g_hue / 360.f;
  fx::palCosine(0.35f, 0.25f, 0.45f, 0.35f, 0.3f, 0.35f, 1.f, 1.f, 1.f, 0.f + hsh, 0.25f + hsh, 0.6f + hsh, 0.f);
  fx::present(canvas);
}

// ---------- face details ----------
static void drawHeart(int x, int y, int r, uint16_t c) {
  canvas.fillCircle(x - r / 2, y, r / 2 + 1, c);
  canvas.fillCircle(x + r / 2, y, r / 2 + 1, c);
  canvas.fillTriangle(x - r - 1, y + 1, x + r + 1, y + 1, x, y + r + 2, c);
}
static void drawNote(int x, int y, uint16_t c) {
  canvas.fillEllipse(x, y, 3, 2, c);
  canvas.drawFastVLine(x + 2, y - 9, 9, c);
  canvas.drawLine(x + 2, y - 9, x + 6, y - 6, c);
}

static void drawFace(const Xf &hf, bool sing, float lid) {
  Bone &hb = s_b[B_HEAD];
  float zs = hf.zx * s_Z;
  // eyes / blink / happy
  float ex[2], ey[2];
  fk(hf, hb, RIG_HEAD_EYEL_X, RIG_HEAD_EYEL_Y, ex[0], ey[0]);
  fk(hf, hb, RIG_HEAD_EYER_X, RIG_HEAD_EYER_Y, ex[1], ey[1]);
  bool happy = s_happy > 0.2f;
  for (int i = 0; i < 2; i++) {
    float sx, sy; toScr(ex[i], ey[i], sx, sy);
    int rx = (int)(13.5f * zs), ry = (int)(15.5f * zs);
    if (happy) {
      canvas.fillEllipse((int)sx, (int)sy, rx + 1, ry + 1, s_skin);
      int w = rx - 2;
      for (int k = -1; k <= 1; k++) {
        canvas.drawLine((int)sx - w, (int)sy + 3 + k, (int)sx, (int)sy - ry / 3 + k, rgb565(20, 50, 25));
        canvas.drawLine((int)sx, (int)sy - ry / 3 + k, (int)sx + w, (int)sy + 3 + k, rgb565(20, 50, 25));
      }
    } else if (s_dizzy > 0.05f) {
      canvas.fillEllipse((int)sx, (int)sy, rx, ry, rgb565(245, 245, 250));
      float a0 = s_t * 14.f * (i ? -1.f : 1.f);
      int px = (int)sx, py = (int)sy;
      for (int k = 1; k < 18; k++) {
        float a = a0 + k * 0.7f, rr = k * rx / 18.f;
        int nx = (int)(sx + cosf(a) * rr), ny = (int)(sy + sinf(a) * rr);
        canvas.drawLine(px, py, nx, ny, rgb565(30, 20, 60));
        px = nx; py = ny;
      }
    } else if (lid > 0.02f) {
      // lid slides down the round eye: per-column, follows the eye's curve
      int cut = (int)(sy - ry - 1 + (2 * ry + 2) * lid);
      for (int x = -rx - 1; x <= rx + 1; x++) {
        float u = (float)x / (rx + 1);
        int e = (int)((ry + 1) * sqrtf(fmaxf(0.f, 1.f - u * u)));
        int top = (int)sy - e, bot = (int)sy + e;
        int c = cut + (int)((ry * 0.25f) * (1.f - u * u) * (1.f - lid));
        if (c > bot) c = bot;
        if (c > top) canvas.drawFastVLine((int)sx + x, top, c - top, s_skin);
        if (c > top && c < bot) canvas.drawPixel((int)sx + x, c, rgb565(20, 45, 25));
      }
    }
  }
  // blush
  if (happy || sing || s_acc > 0.6f) {
    float a = happy ? 1.f : (sing ? clampf(aud::level * 2.f, 0.f, 0.8f) : 0.5f);
    uint16_t bc = rgb565(255, (uint8_t)(170 - a * 40), (uint8_t)(190 - a * 30));
    for (int i = 0; i < 2; i++) {
      float cx, cy; fk(hf, hb, i ? 84.f : 38.f, 79.f, cx, cy);
      float sx, sy; toScr(cx, cy, sx, sy);
      if (a > 0.2f) canvas.fillEllipse((int)sx, (int)sy, (int)(5.f * zs), (int)(2.5f * zs), bc);
    }
  }
  // mouth — anchored to the sprite's own mouth point, scaled with the head
  float mo = s_mouthOpen, wi = s_mouthWide;
  if (mo > 0.06f) {
    float mx, my; fk(hf, hb, RIG_HEAD_MOUTH_X, RIG_HEAD_MOUTH_Y - 1.f, mx, my);
    float sx, sy; toScr(mx, my, sx, sy);
    int mw = (int)((7.f + 6.f * wi - 2.f * mo * (1.f - wi)) * zs);
    int mh = (int)((1.5f + 12.f * mo * (1.f - 0.45f * wi)) * zs);
    if (mw < 2) mw = 2;
    if (mh < 1) mh = 1;
    int X = (int)sx, Y = (int)sy + mh / 3;
    canvas.fillEllipse(X, Y, mw + 2, mh + 2, rgb565(30, 110, 50));
    canvas.fillEllipse(X, Y, mw, mh, rgb565(40, 8, 22));
    if (mh > 4) canvas.fillEllipse(X, Y + mh / 2, mw * 2 / 3, mh / 3 + 1, rgb565(210, 70, 100));
    if (mo > 0.3f && mh > 5) {
      canvas.fillRect(X - mw * 2 / 3, Y - mh + 1, mw * 4 / 3, 1 + mh / 5, rgb565(245, 245, 235));
      for (int k = -1; k <= 1; k++) canvas.drawFastVLine(X + k * mw / 3, Y - mh + 1, 1 + mh / 5, rgb565(180, 180, 170));
    }
  }
}

// ---------- the frame ----------
// ---- the echo cave: a rock face on the right, a deep mouth, rings of sound in and out ----
static void drawCave() {
  int cs = aud::caveState();
  float echo = aud::caveEcho();
  for (int y = 14; y < H - 14; y += 3) {
    float u = (float)(y - 14) / (H - 28);
    canvas.fillRect(0, y, W, 3, rgb565((uint8_t)(20 + 30 * u), (uint8_t)(24 + 20 * (1 - u)), (uint8_t)(46 + 20 * (1 - u))));
  }
  canvas.fillRect(0, 200, W, 26, rgb565(40, 34, 30));
  canvas.fillRoundRect(170, 20, 160, 190, 30, rgb565(70, 62, 58));                         // the rock
  for (int k = 0; k < 14; k++) { int x = 176 + (k * 37) % 150, y = 30 + (k * 53) % 170; canvas.fillCircle(x, y, 6 + k % 5, rgb565(60, 54, 50)); }
  int cx = 252, cy = 124;
  for (int k = 6; k >= 0; k--) canvas.fillEllipse(cx, cy, 44 + k * 4, 70 + k * 5, rgb565((uint8_t)(10 + k * 6), (uint8_t)(8 + k * 5), (uint8_t)(10 + k * 5)));
  canvas.fillEllipse(cx, cy, 40, 66, rgb565(4, 3, 6));
  static float glowHold = 0; if (cs == 3) glowHold = fmaxf(glowHold, 0.6f + echo); glowHold = fmaxf(0.f, glowHold - g_dt * 1.5f); echo = fmaxf(echo, glowHold);
  if (echo > 0.02f) {                                                                       // the cave answers: it glows
    uint16_t gc = echo > 0.3f ? 0x8FE5 : 0x0396;
    for (int k = 0; k < 4; k++) canvas.drawEllipse(cx, cy, 40 + k * 2, 66 + k * 2, k < 2 ? gc : rgb565(0, 160, 160));
    for (int k = 0; k < 3; k++) { int r = (int)fmodf(s_t * 90.f + k * 26.f, 80.f); canvas.drawEllipse(cx - 30 - r / 2, cy, 10 + r / 3, 20 + r / 2, k == 1 ? 0x8FE5 : 0x0396); }
    canvas.fillCircle(cx - 9, cy - 20, 3, 0x8FE5); canvas.fillCircle(cx + 9, cy - 20, 3, 0x8FE5);   // something's eyes in there
  }
  if (cs == 2) for (int k = 0; k < 3; k++) { int r = (int)fmodf(s_t * 80.f + k * 22.f, 60.f); canvas.drawEllipse(190 + r, 116, 6 + r / 4, 12 + r / 3, rgb565(200, 220, 230)); }   // words going in
  for (int k = 0; k < 7; k++) { int x = cx - 34 + k * 11; canvas.fillTriangle(x - 4, cy - 62, x + 4, cy - 62, x, cy - 50 + (k % 3) * 5, rgb565(90, 82, 76)); }   // stalactites
  static float dripY = 0; dripY += g_dt * 60.f; if (dripY > 120) dripY = 0;
  canvas.fillCircle(cx + 12, (int)(cy - 48 + dripY), 1, rgb565(150, 200, 230));
  if (cs == 1) { canvas.fillCircle(24, 26, 4, rgb565(255, 70, 70)); }                     // recording
}

void mantisDraw(bool sing) {
  if (!s_ready) return;
  float dt = g_dt;
  s_t += dt;

  // layout: dance shows the whole bug; sing moves in close on the face
  float zT = s_cave ? 0.66f : (sing ? 1.12f : 0.72f);
  float ryT = s_cave ? 170.f : (sing ? 236.f : 163.f);
  if (sing != s_lastSing) { s_lastSing = sing; s_blend = 0.f; s_prevMove = s_move; }
  s_Z += (zT - s_Z) * clampf(dt * 5.f, 0.f, 1.f);
  s_ry += (ryT - s_ry) * clampf(dt * 5.f, 0.f, 1.f);
  s_rx = s_cave ? 104.f : 160.f;

  // gaze: tilt, or finger while touching
  auto td = M5.Touch.getDetail();
  float lookT = clampf(g_lookX, -1.f, 1.f);
  if (td.isPressed() && td.y > 14 && td.y < H - 14) { lookT = clampf((td.x - 160) / 120.f, -1.f, 1.f); s_touchUntil = millis() + 1200; }
  else if (millis() < s_touchUntil) lookT = s_look;
  s_look += (lookT - s_look) * clampf(dt * 6.f, 0.f, 1.f);

  // energy & beat
  float e = clampf(aud::level * 1.5f + aud::beatConf * 0.25f, 0.f, 1.f);
  if (aud::onset > 0.f) s_acc = fmaxf(s_acc, aud::onset);
  s_acc *= expf(-dt * 7.f);
  static float s_songOn = 0;                                      // a song has been going: keep dancing through its gaps
  if (aud::beatConf > 0.3f || aud::level > 0.22f) s_songOn = 3.f; else s_songOn -= dt;
  bool grooving = s_songOn > 0.f;
  float b = grooving ? aud::beatPos : (s_idleBeat += dt * 0.8f);

  if (s_cave) {
    int cs = aud::caveState();
    int want = cs == 1 ? M_LEAN : (cs == 3 ? M_LISTEN : (cs == 2 ? M_LEAN : M_TALK));
    if (s_move != want) setMove(want);
  } else if (sing) {
    float lv = clampf(aud::level * 1.8f, 0.f, 1.f);
    s_sustain += ((lv > 0.35f ? 1.f : 0.f) - s_sustain) * clampf(dt * (lv > 0.35f ? 0.9f : 2.5f), 0.f, 1.f);
    if (s_move != M_SING) setMove(M_SING);
  } else if (s_frozen || (s_quietT > 0.18f && s_emote < 0)) {
    // silence break: freeze in a pose (even though the groove has stopped)... explode when it comes back
    Feel &F = s_feel;
    F.eFast += (aud::level - F.eFast) * clampf(dt * 5.f, 0.f, 1.f);
    static float frozeAt = 0;
    if (!s_frozen) { s_frozen = true; static const int FV[4] = {0, 1, 2, 4}; s_freezeVar = FV[esp_random() % 4]; setMove(M_FREEZE); frozeAt = s_t; }
    if (s_t - frozeAt > 0.8f * 60.f / fmaxf(aud::bpm(), 60.f)) {                // hold under a beat... then hit an emote into the gap
      s_frozen = false; s_quietT = -1.5f;
      int em = FAM_EMOTES[s_fam][esp_random() % 6];
      s_emote = s_lastEmote = em; s_emoteUntilBeat = (int)floorf(b) + 2; setMove(em); s_emoteCool = 4;
    }
    if (s_t - frozeAt > 1.2f) F.eSlow += (aud::level - F.eSlow) * clampf(dt * 0.8f, 0.f, 1.f);   // a quieter song: re-normalise
    if (F.eFast > F.eSlow * 0.55f) {
      bool realDrop = s_t - frozeAt < 3.f && F.eFast > 0.3f;                         // it came back loud, fast: a drop
      s_frozen = false; s_quietT = 0;
      if (realDrop) {
        s_dropT = s_t; F.drops += (1.f - F.drops) * 0.3f;
        s_emote = M_E_DROP; s_emoteUntilBeat = (int)floorf(b) + 1; setMove(M_E_DROP); hapGesture(HG_CRACK);
      } else setMove(famMove(s_fam));
    }
    if (s_t - s_dropT > 12.f && F.eSlow < 0.12f) s_frozen = false;                  // the song really ended
  } else if (grooving) {
    feelFrame(dt, b);
    // hi-hat trills -> the ridiculously fast shuffle flourish
    static float hatT[10] = {-9, -9, -9, -9, -9, -9, -9, -9, -9, -9}; static int hatI = 0;
    if (aud::hat > 0.15f) { hatT[hatI] = s_t; hatI = (hatI + 1) % 10; }
    int hats = 0; for (float ht : hatT) if (s_t - ht < 0.6f) hats++;
    // bright fast transients repeating -> footwork (as before)
    static float lastOn = -9;
    if (aud::onset > 0.15f && (aud::treble > 0.2f || aud::zcr > 0.3f) && s_t - lastOn > 0.06f) { lastOn = s_t; s_onsetT[s_onsetI] = s_t; s_onsetI = (s_onsetI + 1) % 12; }
    int recent = 0; for (float ot : s_onsetT) if (s_t - ot < 1.f) recent++;
    if (recent >= 4) s_footUntil = s_t + 2.6f;
    Feel &F = s_feel;
    // breakdown: quieter than the song has been, but not silent -> half-time, liquid
    s_breakdown += ((F.eSlow > 0.3f && F.eFast < F.eSlow * 0.6f && F.eFast > F.eSlow * 0.22f ? 1.f : 0.f) - s_breakdown) * clampf(dt * 0.8f, 0.f, 1.f);
    // a clear vocal line -> sometimes it sings along
    s_vocalT = aud::vocal > 0.45f ? s_vocalT + dt : 0.f;
    s_singCool -= dt;
    if (s_vocalT > 1.2f && s_singCool <= 0 && s_singAlong <= 0 && (esp_random() % 3) == 0) { s_singAlong = 6.f; s_singCool = 16.f; }
    s_singAlong -= dt;
    s_beatAcc = fmaxf(s_beatAcc, aud::onset);
    int bi = (int)floorf(b);
    if (bi != s_lastBeatI && !s_frozen) {
      if (grooving) danceNetBeat((uint16_t)aud::bpm(), (uint8_t)(bi & 3));   // real music beats only, never the idle counter
      feelBeat(bi);
      s_strong = s_beatAcc > 0.45f ? s_strong + 1 : 0;
      s_beatAcc = 0; s_lastBeatI = bi;
      int bar = bi / 4, inPhrase = bi % 8;
      bool foot = s_t < s_footUntil;
      if (s_emote >= 0 && bi >= s_emoteUntilBeat) { s_emote = -1; setMove(famMove(s_fam)); }
      if (s_emoteCool > 0) s_emoteCool--;
      if (s_emote < 0 && hats >= 5 && s_t - s_trillT > 3.f) {                       // the trill flourish (one beat)
        s_trillT = s_t; s_emote = M_TRILL; s_emoteUntilBeat = bi + 1; setMove(M_TRILL);
      } else if (s_strong >= 3 && (bi & 3) == 3 && s_emoteCool == 0 && s_emote < 0 && !foot && (esp_random() % 10) < 4) {
        int em; int tries = 0;                                                         // 3 strong beats -> the bar's 4th lands an emote
        do { em = FAM_EMOTES[s_fam][esp_random() % 6]; } while (em == s_lastEmote && ++tries < 6);
        s_emote = s_lastEmote = em; s_emoteUntilBeat = bi + 1; setMove(em); s_strong = 0; s_emoteCool = 8;
      } else if (s_emote < 0) {
        if (inPhrase == 0) {                                                         // phrase boundary: re-read the music
          uint8_t cand = scoreFamily();
          if (cand == s_famCand) s_famVotes++; else { s_famCand = cand; s_famVotes = 1; }
          if (s_famVotes >= 2 && cand != s_fam) s_fam = cand;                        // two phrases of agreement to switch
        }
        if (foot && s_fam != F_SOFT) { int fm[] = {M_RUNMAN, M_TSTEP, M_SHUFFLE2}; if (bar != s_lastBar || s_move < M_RUNMAN) { s_lastBar = bar; setMove(fm[esp_random() % 3]); } }
        else if (inPhrase == 0 || bar != s_lastBar || s_move == M_IDLE || s_move == M_SING || s_move == M_FREEZE || (s_move >= M_TALK && s_move <= M_LISTEN)) {
          s_lastBar = bar;
          if (s_breakdown > 0.6f) { int bd[] = {M_SLOWMO, M_LIQUID, M_TWIG, M_BODYWAVE}; setMove(bd[esp_random() % 4]); }
          else setMove(famMove(s_fam));
          if (inPhrase == 7 && (esp_random() % 6) == 0 && s_emoteCool == 0) {        // sometimes a mantis moment closes a phrase
            static const uint8_t MM[] = {M_E_BOW, M_E_SWIVEL, M_E_GROOM, M_E_STRIKE}; int em = MM[esp_random() % 4];
            s_emote = em; s_emoteUntilBeat = bi + 1; setMove(em); s_emoteCool = 6;
          }
        }
      }
    }
    } else {
    feelFrame(dt, b);
    static float reread = 0; reread += dt;
    if (reread > 3.f) { reread = 0; uint8_t c = scoreFamily(); if (c == s_famCand) s_famVotes++; else { s_famCand = c; s_famVotes = 1; } if (s_famVotes >= 2) s_fam = c; }
    s_idleWaveT -= dt;
    if (s_idleWaveT < 0 && s_move == M_IDLE) { setMove((esp_random() & 1) ? M_WAVE_L : M_WAVE_R); s_idleWaveT = 2.5f; }
    else if (s_idleWaveT < 0) { setMove(M_IDLE); s_idleWaveT = 7.f + (esp_random() % 6); }
    else if (s_move != M_IDLE && s_move != M_WAVE_L && s_move != M_WAVE_R) setMove(M_IDLE);
  }

  // the move clock: half-time for dubstep / soft / breakdowns (continuous, so switching never jumps)
  static float s_mb = 0, s_lastB = 0;
  float dB = b - s_lastB; if (dB < 0 || dB > 1.f) dB = 0; s_lastB = b;
  float speedK = (s_fam == F_DUBSTEP || s_fam == F_SOFT || s_breakdown > 0.6f) ? 0.5f : 1.f;
  s_mb += dB * speedK;
  auto beatFor = [&](int mv) { return (mv >= M_E_CLAP && mv <= M_E_BOW) || mv >= M_TRILL ? b : s_mb; };
  { // a real silence break: the song had been going, and it's nearly silent for a quarter second (not just a gap between notes)
    Feel &F = s_feel;
    if (!sing && !s_cave && F.eSlow > 0.28f && aud::level < F.eSlow * 0.2f) s_quietT += dt; else s_quietT = 0.f;
#ifdef FREEZE_LOG
    static float mn = 9; mn = fminf(mn, aud::level / fmaxf(F.eSlow, 0.01f)); static int fc = 0; if (++fc % 150 == 0) { printf("  [level/eSlow min %.2f eSlow %.2f quietT %.2f]\n", mn, F.eSlow, s_quietT); mn = 9; }
#endif
  }
  Pose pa, pb, p;
  evalMove(s_prevMove, beatFor(s_prevMove), e, pa);
  evalMove(s_move, beatFor(s_move), e, pb);
  s_blend = fminf(1.f, s_blend + dt * 3.2f);
  float k = s_blend * s_blend * (3.f - 2.f * s_blend);
  lerpPose(p, pa, pb, k);
#ifdef HOST
  { extern int g_dbgMove; extern float g_dbgClaw, g_dbgBeat;
    if (g_dbgMove >= 0) evalMove(g_dbgMove, g_dbgBeat, 1.f, p);
    if (g_dbgClaw >= 0) p.claw[0] = p.claw[1] = g_dbgClaw; }
#endif
  p.y += 3.f * s_acc + 4.f * s_build; p.sq += 0.7f * s_acc + 0.3f * s_build; p.hy += 1.5f * s_acc;
  if (s_singAlong > 0 && !sing && !s_cave) { p.head *= 0.6f; p.hy -= 2.f; }
  if (!sing && !s_cave && grooving && s_move != M_FREEZE) {
    // Body layers run all the time, concurrently, under whatever the move is doing:
    //   hips + head follow the bass/kick,  legs follow the hats/shakers,  arms rise when a lead line soars.
    static float kickEnv = 0, hatAct = 0, soar = 0;
    kickEnv = fmaxf(kickEnv * expf(-dt * 9.f), aud::kick);
    hatAct += ((aud::hat > 0.1f ? 1.f / fmaxf(dt, 0.01f) : 0.f) + aud::treble * 4.f - hatAct) * clampf(dt * 1.5f, 0.f, 1.f);
    float soarIn = clampf((aud::centroid - 0.28f) * 3.f, 0.f, 1.f) * clampf(s_feel.tonal * 1.6f - 0.4f, 0.f, 1.f) * clampf(aud::level * 2.f, 0.f, 1.f) * (aud::onset < 0.25f ? 1.f : 0.6f);
    soar += (soarIn - soar) * clampf(dt * (soarIn > soar ? 0.8f : 0.4f), 0.f, 1.f);        // slow: a line has to sustain
    bool emote = s_move >= M_E_CLAP && s_move <= M_E_BOW || s_move >= M_E_HORNS;
    float legBusy = 0; for (int i = 0; i < 4; i++) legBusy += fabsf(pb.fx[i]) + fabsf(pb.fy[i]);
    float hipW = (s_move == M_WINDMILL || s_move == M_MOSH || s_move == M_BODYWAVE) ? 0.4f : 1.f;
    float legW = legBusy > 3.f ? 0.25f : 1.f, armW = emote ? 0.25f : 1.f;
    float bf = b - floorf(b); int bi = (int)floorf(b); float side = (bi & 1) ? 1.f : -1.f;
    // hips: she sways side to side on the beat, pops on the kick; the head rides it
    float hipAmt = clampf(0.3f + aud::bass * 1.3f, 0.f, 1.f) * hipW, hs = sinf(3.14159f * bf);
    p.abd += side * (13.f * hs + 7.f * kickEnv) * hipAmt;
    p.x += side * 4.f * hs * hipAmt; p.torso -= side * 3.f * hs * hipAmt;
    p.hy += 2.5f * kickEnv * hipW; p.head += side * 3.f * hs * hipAmt;
    // legs: always stepping on the beat; hats push it to 8ths, then 16ths
    float rate = hatAct > 9.f ? 4.f : (hatAct > 4.f ? 2.f : 1.f);
    float q = b * rate, qf = q - floorf(q); int qi = (int)floorf(q);
    float lift = sinf(3.14159f * qf) * clampf(0.45f + hatAct * 0.06f, 0.f, 1.f) * legW;
    int f0 = (qi & 1) ? 2 : 0, f1 = f0 + (rate >= 2.f ? 1 : 0);
    p.fy[f0] -= 7.f * lift; p.fx[f0] += (f0 ? 3.f : -3.f) * lift;
    if (f1 != f0) { p.fy[f1] -= 4.f * lift; p.fx[f1] += (f0 ? 2.f : -2.f) * lift; }
    p.y += 1.2f * lift;
    // arms: a soaring lead lifts them and opens the claws
    for (int a = 0; a < 2; a++) { p.au[a] += soar * 45.f * armW; p.claw[a] = fminf(1.f, p.claw[a] + soar * 0.4f * armW); }
    if (soar > 0.5f) p.strain = fmaxf(p.strain, soar);
  }
  if (s_happy > 0) { s_happy -= dt; p.y -= 4.f * sinf(s_t * 18.f) * fminf(1.f, s_happy); p.mouth = fmaxf(p.mouth, 0.35f); }
  if (g_shakeKick) s_dizzy = 1.4f;
  if (s_dizzy > 0) { s_dizzy -= dt; p.head += sinf(s_t * 11.f) * 14.f * s_dizzy; p.torso += sinf(s_t * 7.f) * 5.f * s_dizzy; }

  // blink
  s_blinkT -= dt;
  if (s_blinkT < 0) { s_blink = 1.f; s_blinkT = 2.f + (esp_random() % 3000) / 1000.f; }
  s_blink = s_blink > 0 ? s_blink - dt * 7.f : 0.f;
  float lid = s_blink > 0.5f ? (1.f - s_blink) * 2.f : s_blink * 2.f;

  // mouth
  float mTarget = p.mouth;
  if (!sing && !s_cave && s_singAlong > 0) {                      // singing along with the vocal line it hears
    mTarget = fmaxf(mTarget, powf(aud::vocalEnv, 0.8f) * clampf(aud::vocal * 2.f, 0.f, 1.f));
    s_mouthWide += (clampf(aud::zcr * 1.4f, 0.f, 1.f) - s_mouthWide) * clampf(dt * 12.f, 0.f, 1.f);
  }
  if (sing || s_cave) {
    float wide = 0, env = aud::mouthNow(&wide);                  // 4 ms speech envelope: closes between syllables
    mTarget = fmaxf(s_cave ? 0.f : mTarget * 0.3f, powf(env, 0.8f));
    s_mouthWide += (wide - s_mouthWide) * clampf(dt * 18.f, 0.f, 1.f);
  } else s_mouthWide *= 0.9f;
  s_mouthOpen += (mTarget - s_mouthOpen) * clampf(dt * (mTarget > s_mouthOpen ? 45.f : 30.f), 0.f, 1.f);

  // ---------- stage ----------
  float beatF = b - floorf(b);
  if (s_cave) drawCave(); else drawStage(sing, beatF, e);

  // ---------- skeleton ----------
  Bone &T = s_b[B_TORSO];
  float sq = clampf(p.sq, -0.8f, 1.5f);
  Xf tf{p.x, p.y, p.torso, 1.f + sq * 0.05f, 1.f - sq * 0.075f};

  // hips / shoulders / neck / abdomen anchors
  float hipX[4], hipY[4];
  const float hipLx[4] = {RIG_TORSO_HIPBL_X, RIG_TORSO_HIPFL_X, RIG_TORSO_HIPFR_X, RIG_TORSO_HIPBR_X};
  const float hipLy[4] = {RIG_TORSO_HIPBL_Y, RIG_TORSO_HIPFL_Y, RIG_TORSO_HIPFR_Y, RIG_TORSO_HIPBR_Y};
  for (int i = 0; i < 4; i++) fk(tf, T, hipLx[i], hipLy[i], hipX[i], hipY[i]);
  float shX[2], shY[2];
  fk(tf, T, RIG_TORSO_SHL_X, RIG_TORSO_SHL_Y, shX[0], shY[0]);
  fk(tf, T, RIG_TORSO_SHR_X, RIG_TORSO_SHR_Y, shX[1], shY[1]);
  float nkX, nkY; fk(tf, T, RIG_TORSO_NECK_X, RIG_TORSO_NECK_Y, nkX, nkY);
  float abX, abY; fk(tf, T, RIG_TORSO_ABD_X, RIG_TORSO_ABD_Y, abX, abY);

  // abdomen (behind everything)
  Xf af{abX, abY, -48.f + p.abd + p.torso * 0.5f, 1.f, 1.f};
  blit(s_b[B_ABD], af);

  // legs: IK with planted feet
  const float footBx[4] = {-62.f, -34.f, 34.f, 62.f};
  const float floorY = 85.f;
  float L1 = hypotf(RIG_THIGH_END_X - RIG_THIGH.pvx, RIG_THIGH_END_Y - RIG_THIGH.pvy);
  float L2 = hypotf(RIG_SHIN_END_X - RIG_SHIN.pvx, RIG_SHIN_END_Y - RIG_SHIN.pvy);
  const int legOrder[4] = {0, 3, 1, 2};   // back pair first
  for (int oi = 0; oi < 4; oi++) {
    int i = legOrder[oi];
    bool right = i >= 2;
    float fX = footBx[i] + p.fx[i], fY = floorY + p.fy[i];
    float dx = fX - hipX[i], dy = fY - hipY[i];
    float d = hypotf(dx, dy);
    float reach = (L1 + L2) * 0.995f;
    if (d > reach) { fX = hipX[i] + dx / d * reach; fY = hipY[i] + dy / d * reach; d = reach; }
    if (d < fabsf(L1 - L2) + 1.f) d = fabsf(L1 - L2) + 1.f;
    float th = atan2f(dy, dx);
    float ca = clampf((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f);
    float al = acosf(ca);
    float ka = right ? th - al : th + al;
    float kX = hipX[i] + cosf(ka) * L1, kY = hipY[i] + sinf(ka) * L1;
    Bone &TH = s_b[right ? B_THI_R : B_THI_L];
    Bone &SH = s_b[right ? B_SHI_R : B_SHI_L];
    float thRest = boneAngle(TH, RIG_THIGH_END_X, RIG_THIGH_END_Y, right);
    float shRest = boneAngle(SH, RIG_SHIN_END_X, RIG_SHIN_END_Y, right);
    float shA = atan2f(fY - kY, fX - kX) / DEG;
    Xf sf{kX, kY, shA - shRest, 1.f, 1.f};
    Xf thf{hipX[i], hipY[i], ka / DEG - thRest, 1.f, 1.f};
    blit(TH, thf);
    blit(SH, sf);
  }

  blit(T, tf);

  // head
  Xf hf{nkX + p.hx, nkY + p.hy, p.torso + p.head, 1.f - sq * 0.03f, 1.f + sq * 0.02f};

  // arms (upper then scythe). index 0 = viewer-left, positive = outward/up
  float tipX[2], tipY[2], tipA[2];
  float micX = 0, micY = 0, mouthX = 0, mouthY = 0;
  fk(hf, s_b[B_HEAD], RIG_HEAD_MOUTH_X, RIG_HEAD_MOUTH_Y, mouthX, mouthY);
  micX = mouthX - 12.f; micY = mouthY + 14.f;
  for (int a = 0; a < 2; a++) {
    bool r = a == 1;
    float sgn = r ? -1.f : 1.f;
    Bone &AR = s_b[r ? B_ARM_R : B_ARM_L];
    Bone &SC = s_b[r ? B_SCY_R : B_SCY_L];
    Xf uf{shX[a], shY[a], p.torso + sgn * p.au[a], 1.f, 1.f};
    float eX, eY; fk(uf, AR, RIG_ARM_END_X, RIG_ARM_END_Y, eX, eY, r);
    Xf scf{eX, eY, uf.a + sgn * p.as[a], 1.f, 1.f};
    if (s_cave && (s_move == M_LEAN || (s_move == M_LISTEN && a == 1))) {
      // cupping: each claw reaches its side of the mouth (or the ear), elbows flared outward
      float gX = s_move == M_LEAN ? mouthX + (r ? 11.f : -11.f) : mouthX + 26.f, gY = s_move == M_LEAN ? mouthY + 5.f : mouthY - 28.f;
      Bone &ARl = s_b[B_ARM_L]; Bone &SCl = s_b[B_SCY_L];
      float L1 = hypotf(RIG_ARM_END_X - ARl.pvx, RIG_ARM_END_Y - ARl.pvy);
      float L2 = hypotf(RIG_SCYTHE_TIP_X - SCl.pvx, RIG_SCYTHE_TIP_Y - SCl.pvy);
      float dx = gX - shX[a], dy = gY - shY[a], d = clampf(hypotf(dx, dy), fabsf(L1 - L2) + 1.f, (L1 + L2) * 0.99f);
      float th = atan2f(dy, dx), al = acosf(clampf((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f));
      float e1 = th + al, e2 = th - al;
      float ea = (r ? cosf(e1) > cosf(e2) : cosf(e1) < cosf(e2)) ? e1 : e2;   // the elbow that flares out
      eX = shX[a] + cosf(ea) * L1; eY = shY[a] + sinf(ea) * L1;
      uf.a = ea / DEG - boneAngle(AR, RIG_ARM_END_X, RIG_ARM_END_Y, r);
      scf.x = eX; scf.y = eY;
      scf.a = atan2f(gY - eY, gX - eX) / DEG - boneAngle(SC, RIG_SCYTHE_TIP_X, RIG_SCYTHE_TIP_Y, r);
    }
    if (sing && !s_cave && a == 0) {
      // 2-bone IK: the hook grips the mic handle just under the mouth
      float L1 = hypotf(RIG_ARM_END_X - AR.pvx, RIG_ARM_END_Y - AR.pvy);
      float L2 = hypotf(RIG_SCYTHE_TIP_X - SC.pvx, RIG_SCYTHE_TIP_Y - SC.pvy);
      float gX = micX - 16.f, gY = micY + 30.f;
      float dx = gX - shX[0], dy = gY - shY[0], d = clampf(hypotf(dx, dy), fabsf(L1 - L2) + 1.f, (L1 + L2) * 0.99f);
      float th = atan2f(dy, dx), al = acosf(clampf((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f));
      float ea = th + al;                                   // natural hold: the elbow drops, tucked by the body
      if (sinf(th - al) < sinf(ea)) ea = th - al;         // (verified by render: this branch drops the elbow)
      eX = shX[0] + cosf(ea) * L1; eY = shY[0] + sinf(ea) * L1;
      uf.a = ea / DEG - boneAngle(AR, RIG_ARM_END_X, RIG_ARM_END_Y, false);
      scf.x = eX; scf.y = eY;
      scf.a = atan2f(gY - eY, gX - eX) / DEG - boneAngle(SC, RIG_SCYTHE_TIP_X, RIG_SCYTHE_TIP_Y, false);
    }
    // The spines run along one edge of the femur sprite. Rotating an arm up past ~90 deg would turn that edge
    // to the sky - upside down for a mantis. Flip the scythe across its length instead, so the spines keep facing
    // down/inward; only a straining overhead hype pose may show them upward. (Hysteresis: no flicker.)
    static bool flipped[2] = {false, false};
    float ny = cosf(scf.a * DEG);                                  // y of the spine-edge normal in the screen
    bool allowUp = p.strain > 0.5f || (sing && !s_cave && a == 0) || (s_cave && (s_move == M_LEAN || s_move == M_LISTEN));
    if (allowUp) flipped[a] = false;
    else if (!flipped[a] && ny < -0.45f) flipped[a] = true;             // only when the spines point strongly skyward
    else if (flipped[a] && ny > -0.1f) flipped[a] = false;
    float fz = flipped[a] ? -1.f : 1.f;
    scf.zy = fz;
    Bone &CL = s_b[r ? B_CLAW_R : B_CLAW_L];
    float hX, hY; fk(scf, SC, RIG_SCYTHE_HINGE_X, RIG_SCYTHE_HINGE_Y, hX, hY, r);
    Xf clf{hX, hY, scf.a + fz * sgn * (0.4f - clampf(p.claw[a], 0.f, 1.f)) * 112.f, 1.f, fz};   // closes toward the spines
    blit(AR, uf);
    blit(CL, clf);
    blit(SC, scf);
    fk(clf, CL, RIG_CLAW_TIP_X, RIG_CLAW_TIP_Y, tipX[a], tipY[a], r);
    tipA[a] = clf.a;
  }
  blit(s_b[B_HEAD], hf);
  drawFace(hf, sing, lid);

  // sing prop: a little mic, gripped by the left hook, head just under the mouth
  if (sing && !s_cave) {
    float hx, hy, gx, gy, ax, ay;
    toScr(micX, micY, hx, hy);
    toScr(micX - 16.f, micY + 30.f, gx, gy);
    toScr(mouthX, mouthY, ax, ay);
    for (int k = -2; k <= 2; k++)
      canvas.drawLine((int)gx + k, (int)gy + 6, (int)hx + k, (int)hy, k == -2 ? rgb565(30, 30, 38) : (k == 1 ? rgb565(150, 150, 165) : rgb565(80, 80, 92)));
    int mr = (int)(7.f * s_Z);
    canvas.fillCircle((int)hx, (int)hy, mr + 1, rgb565(30, 30, 40));
    canvas.fillCircle((int)hx, (int)hy, mr, rgb565(120, 120, 135));
    for (int k = -mr + 2; k < mr - 1; k += 3) canvas.drawFastHLine((int)hx - mr + 2, (int)hy + k, 2 * mr - 3, rgb565(70, 70, 85));
    canvas.fillCircle((int)hx - mr / 3, (int)hy - mr / 3, 2, rgb565(230, 230, 245));
    (void)tipA;
    static float noteT = 0;
    noteT -= dt;
    if (aud::level > 0.2f && noteT < 0) {
      noteT = 0.35f - aud::level * 0.15f;
      spawnFx(ax + 10.f, ay - 6.f, 3, g_hue + (esp_random() % 120));
    }
  }

  // floating hearts / sparks / notes
  for (auto &q : s_fx) {
    if (q.life <= 0) continue;
    q.life -= dt * (q.kind == 3 ? 0.5f : 0.8f);
    q.x += q.vx * dt * (q.kind == 3 ? 0.5f : 1.f) + (q.kind == 3 ? sinf(s_t * 5.f + q.hue) * 0.6f : 0.f);
    q.y += q.vy * dt;
    q.vy += (q.kind == 2 ? 60.f : -6.f) * dt;
    uint16_t c = hsv565(q.hue, q.kind == 1 ? 0.55f : 0.8f, 0.6f + 0.4f * q.life);
    if (q.kind == 1) drawHeart((int)q.x, (int)q.y, 4 + (int)(q.life * 3), c);
    else if (q.kind == 3) drawNote((int)q.x, (int)q.y, c);
    else canvas.fillCircle((int)q.x, (int)q.y, 1 + (int)(q.life * 2.f), c);
  }
}
#ifdef HOST
int mantisFam() { return s_fam; }
int mantisMove() { return s_move; }
bool mantisSinging() { return s_singAlong > 0; }
bool mantisFrozen() { return s_frozen; }
#endif
#ifdef HOST
void mantisFeelDump(char *o) { Feel &F = s_feel; snprintf(o, 200, "four=%.2f back=%+.2f off=%.2f dens=%.1f sus=%.2f tonal=%.2f eS=%.2f cen=%.2f wob=%.2f", F.four, F.back, F.off, F.dens, F.sus, F.tonal, F.eSlow, aud::centroid, F.wob); }
#endif
