// ============================================================
//  SYNAPSE — a small green bug in a glass panel · M5Stack Core2
//  main.cpp: frame pipeline, input, visual modes.
//  audio.cpp: always-on listening. calm.cpp: the physics room.
//  fx.cpp: indexed-colour demo engine. mantis.cpp: the puppet.
// ============================================================
#include "dance_net.h"
#include "app.h"
#include "audio.h"
#include "fx.h"
#include "wire.h"
#include "stats.h"
#ifndef HOST
#include <Preferences.h>
#endif
#include "mantis_splash.h"
#include <string.h>

// ---------------- shared state ----------------
M5Canvas *g_cv = nullptr;
float g_t = 0, g_dt = 0.033f, g_hue = 160;
float g_level = 0, g_peak = 0;
float g_lookX = 0, g_lookY = 0;
float g_gravX = 0, g_gravY = 0;        // downhill direction in screen space (1 = 1 g)
float g_gravZ = 1, g_jolt = 0;          // |gravity into the screen| (1 = lying flat), instantaneous jolt (g)
float g_gyroX = 0, g_gyroY = 0, g_gyroZ = 0;
int16_t g_mic[MIC_N];

static float g_ax = 0, g_ay = 0, g_az = 1, g_gx = 0, g_gy = 0, g_gz = 0;
static float g_shake = 0;
static uint32_t g_shakeAt = 0;
bool g_shakeKick = false;                  // one-frame shake event

enum Mode : uint8_t { MODE_SWARM = 0, MODE_EYE, MODE_TUNNEL, MODE_PULSE, MODE_CALM, MODE_MANTIS, MODE_ROOMS, MODE_GARDEN, MODE_MEDITATE, MODE_COUNT };
enum SwarmVar : uint8_t { SV_FLOCK = 0, SV_ORBIT, SV_CHAOS, SV_COUNT };
enum TunnelMode : uint8_t { TM_DIVE = 0, TM_RECEDE, TM_FRACTAL, TM_PORTAL, TM_COUNT };
static const int PP_COUNT = 5;
static Mode g_mode = MODE_SWARM;
static SwarmVar g_swarmVar = SV_FLOCK;
static TunnelMode g_tunnelMode = TM_DIVE;
static int g_pulsePat = 0;
static bool g_eyeTrack = true;
static bool g_mantisSing = false;
static bool s_party = false;      // Dance Party broadcast: hold C in mantis mode
static uint8_t g_mantisMode = 0;              // 0 dance, 1 sing, 2 echo (yakback)

// ---------------- haptics: an expressive little mixer for the vibration motor ----------------
// The Core2 motor is an ERM whose strength follows setVibration(0..255), so we shape it:
//   taps      short hits with a decaying tail (hap)
//   rumble    a continuous layer: amount, pulse rate (Hz) and grit (roughness), refreshed per frame
//   gestures  keyframed envelopes (hapGesture) for moments that deserve a shape
//   cut       instant silence (hapCut), e.g. the moment we cross into a new dimension
// Everything is max-mixed, remapped above the motor's dead zone, and written only when it changes.
static uint32_t g_hapUntil = 0, g_hapStart = 0, g_kickStart = 0, g_kickEnd = 0, g_quietUntil = 0, g_lastVibWrite = 0;
static uint8_t g_hapLevel = 0, g_vibNow = 255;
static float g_rumAmt = 0, g_rumTarget = 0, g_rumRate = 6.f, g_rumGrit = 0, g_rumPh = 0, g_rumJit = 0;
static uint32_t g_rumStamp = 0, g_rumJitAt = 0, g_hapT = 0;
struct HapKey { uint8_t level; uint16_t ms; };
static const HapKey *g_gest = nullptr; static int g_gestN = 0, g_gestI = 0; static uint32_t g_gestT0 = 0; static uint8_t g_gestFrom = 0;
static const HapKey G_THREAD[]  = {{150, 22}, {40, 45}, {220, 28}, {0, 110}};                                   // ta-DING
static const HapKey G_CHAIN[]   = {{90, 70}, {170, 60}, {60, 50}, {200, 70}, {90, 50}, {255, 110}, {0, 260}};   // swelling triple
static const HapKey G_MISS[]    = {{170, 18}, {110, 90}, {50, 110}, {85, 70}, {0, 240}};                        // dull wobble, sinking
static const HapKey G_THUNDER[] = {{50, 160}, {150, 120}, {80, 170}, {210, 90}, {120, 260}, {70, 320}, {0, 480}};   // rolling, far away
static const HapKey G_SETTLE[]  = {{120, 40}, {0, 90}, {170, 55}, {0, 160}};                                    // two soft pulses: done
static const HapKey G_REBIRTH[] = {{30, 250}, {140, 350}, {230, 200}, {0, 30}};                                 // swell, then gone
static const HapKey G_PAIN[]    = {{255, 30}, {120, 60}, {190, 40}, {60, 140}, {0, 160}};                       // flinch + throb
static const HapKey G_LUBDUB[]  = {{175, 45}, {30, 90}, {125, 40}, {0, 220}};                                  // lub-dub
static const HapKey G_THREE[]   = {{140, 350}, {0, 450}, {140, 350}, {0, 450}, {140, 350}, {0, 300}};               // three slow pulses
static const HapKey G_POP[]     = {{255, 22}, {0, 16}, {190, 12}, {0, 40}};                                        // bubble-wrap pop
static const HapKey G_CRACK[]   = {{255, 16}, {150, 26}, {235, 20}, {90, 40}, {0, 50}};                         // a close lightning crack
static const HapKey *const GEST[] = {G_THREAD, G_CHAIN, G_MISS, G_THUNDER, G_SETTLE, G_REBIRTH, G_PAIN, G_CRACK, G_LUBDUB, G_THREE, G_POP};
static const int GEST_N[] = {4, 7, 5, 7, 4, 4, 5, 5, 4, 6, 4};

static void vib(uint8_t v) {
  uint32_t now = millis();
  if (v == g_vibNow) return;
  if (v != 0 && g_vibNow != 0 && abs((int)v - (int)g_vibNow) < 4) return;       // don't spam I2C with tiny changes
  if (v != 0 && now - g_lastVibWrite < 4) return;
  g_vibNow = v; g_lastVibWrite = now; M5.Power.setVibration(v);
}
void hap(uint8_t level, uint16_t ms) {
  if (millis() < g_quietUntil) return;
  if (level >= g_hapLevel || millis() >= g_hapUntil) { g_hapLevel = level; g_hapStart = millis(); g_hapUntil = millis() + ms; }
}
void kickSubHaptic() { g_kickStart = millis(); g_kickEnd = g_kickStart + 150; }
void hapRumble(float amount, float rateHz, float grit) {
  g_rumTarget = clampf(amount, 0.f, 1.f); g_rumRate = rateHz; g_rumGrit = clampf(grit, 0.f, 1.f); g_rumStamp = millis();
}
void hapGesture(uint8_t id) {
  if (millis() < g_quietUntil || id >= sizeof(GEST_N) / sizeof(GEST_N[0])) return;
  g_gestFrom = g_vibNow == 255 ? 0 : g_vibNow; g_gest = GEST[id]; g_gestN = GEST_N[id]; g_gestI = 0; g_gestT0 = millis();
}
void hapCut(uint16_t quietMs) {
  g_rumAmt = g_rumTarget = 0; g_gest = nullptr; g_hapLevel = 0; g_hapUntil = 0; g_kickEnd = 0;
  g_quietUntil = millis() + quietMs;
  g_vibNow = 0; M5.Power.setVibration(0);
}
static void hapService() {
  uint32_t now = millis();
  float dt = (now - g_hapT) / 1000.f; if (dt > 0.1f) dt = 0.1f; g_hapT = now;
  if (now < g_quietUntil) { vib(0); return; }
  float out = 0;
  // taps: hit hard, then a soft tail
  if (g_hapLevel && now < g_hapUntil) {
    float u = (float)(now - g_hapStart) / (float)(g_hapUntil - g_hapStart + 1);
    out = fmaxf(out, g_hapLevel / 255.f * (1.f - 0.45f * u * u));
  } else g_hapLevel = 0;
  // kick "subwoofer" throb
  if (g_kickEnd && now < g_kickEnd) {
    float u = (float)(now - g_kickStart) / (float)(g_kickEnd - g_kickStart), env = (1.f - u) * (1.f - u);
    out = fmaxf(out, env * (((now / 9) & 1) ? 0.9f : 0.15f));
  } else g_kickEnd = 0;
  // rumble: eases toward its target; if nobody refreshes it, it fades away
  if (now - g_rumStamp > 120) g_rumTarget = 0;
  g_rumAmt += (g_rumTarget - g_rumAmt) * clampf(dt * (g_rumTarget > g_rumAmt ? 10.f : 6.f), 0.f, 1.f);
  if (g_rumAmt > 0.01f) {
    g_rumPh += dt * g_rumRate; if (g_rumPh > 1000.f) g_rumPh -= 1000.f;
    float w = 0.5f + 0.5f * sinf(g_rumPh * 6.2831853f);
    w = w * w * (3.f - 2.f * w);                                 // rounder throb
    if (now - g_rumJitAt > 12) { g_rumJitAt = now; g_rumJit = (esp_random() % 1000) / 1000.f; }
    float floor_ = 0.45f + 0.5f * g_rumAmt * g_rumAmt;            // strong rumbles fill in: pulses become a roar
    float v = g_rumAmt * (floor_ + (1.f - floor_) * w) * (1.f - g_rumGrit * 0.35f * g_rumJit);
    out = fmaxf(out, v);
  }
  // gesture: linear ramps between keyframes
  if (g_gest) {
    uint32_t e = now - g_gestT0;
    while (g_gest && e >= g_gest[g_gestI].ms) {
      e -= g_gest[g_gestI].ms; g_gestT0 += g_gest[g_gestI].ms; g_gestFrom = g_gest[g_gestI].level;
      if (++g_gestI >= g_gestN) g_gest = nullptr;
    }
    if (g_gest) {
      float u = (float)e / (float)(g_gest[g_gestI].ms + 1);
      out = fmaxf(out, (g_gestFrom + (g_gest[g_gestI].level - g_gestFrom) * u) / 255.f);
    }
  }
  // above the dead zone the motor responds; below it, silence
  vib(out < 0.03f ? 0 : (uint8_t)(55.f + out * 200.f));
}

// ---------------- frame pipeline: render core 1, push core 0 ----------------
static M5Canvas s_fbA(&M5.Display), s_fbB(&M5.Display);
static M5Canvas *s_fb[2] = {&s_fbA, &s_fbB};
static int s_cur = 0;
static bool s_double = false;
static TaskHandle_t s_dispTask = nullptr;
static SemaphoreHandle_t s_dispIdle = nullptr;
static volatile int s_pushIdx = 0;

static void displayTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_fb[s_pushIdx]->pushSprite(&M5.Display, 0, 0);
    xSemaphoreGive(s_dispIdle);
  }
}

static void pollInput();
static void present() {
  if (!s_double) {
    s_fb[0]->pushSprite(&M5.Display, 0, 0);
    pollInput();
    return;
  }
  // wait for the LCD to finish the previous frame — and keep reading input meanwhile
  while (xSemaphoreTake(s_dispIdle, 0) != pdTRUE) { pollInput(); vTaskDelay(1); }
  s_pushIdx = s_cur;
  xTaskNotifyGive(s_dispTask);
  s_cur ^= 1;
  g_cv = s_fb[s_cur];
}

// ---------------- input ----------------
static bool g_tapLatch = false;
static int g_tapX = 0, g_tapY = 0;
static bool takeTap(int &x, int &y) {
  if (!g_tapLatch) return false;
  g_tapLatch = false; x = g_tapX; y = g_tapY;
  return true;
}
static void nextMode(int dir);
enum CalState : uint8_t { CAL_IDLE = 0, CAL_RING, CAL_RUN, CAL_DONE };
static uint8_t s_cal = CAL_IDLE;
static uint32_t s_calT = 0;
static int s_calX = 0, s_calY = 0;
static float s_calS[3]; static int s_calN = 0;
static void btnBShort();
static void eyePoke(int x, int y);
static uint32_t s_bDown = 0;
static bool s_bLong = false;
static uint32_t s_cDown = 0;
static bool s_cLong = false;
static uint8_t s_eyeStyle = 0;                 // 0 basic, 1 cat, 2 dragon (long-press B in EYE)
static uint32_t s_touchDown = 0;
static int s_touchX0 = 0, s_touchY0 = 0;
static bool s_longFired = false;
static bool s_centreTouch = false;

static void pollInput() {
  if (g_mode == MODE_MEDITATE) {                          // ~250 Hz motion sampling for breath + heart
    static uint32_t lastUs = 0;
    uint32_t us = micros();
    if (us - lastUs >= 4000 && M5.Imu.update()) {
      auto d = M5.Imu.getImuData();
      float dts = lastUs ? (us - lastUs) / 1e6f : 0.004f; if (dts > 0.05f) dts = 0.05f;
      lastUs = us;
      medSample(d.accel.x, d.accel.y, d.accel.z, dts);
    }
  }
  M5.update();
  hapService();
  uint32_t now = millis();
  if (M5.BtnA.wasPressed()) nextMode(-1);
  if (M5.BtnC.wasPressed()) { s_cDown = now; s_cLong = false; }
  if (M5.BtnC.isPressed() && s_cDown && !s_cLong && g_mode == MODE_MANTIS && now - s_cDown > 650) {
    s_cLong = true; s_party = !s_party; hapGesture(HG_SETTLE);   // hold C in mantis mode: Dance Party on/off
  }
  if (M5.BtnC.wasReleased() && s_cDown) { if (!s_cLong) nextMode(1); s_cDown = 0; }
  if (M5.BtnB.wasPressed()) { s_bDown = now; s_bLong = false; }
  if (M5.BtnB.isPressed() && s_bDown && !s_bLong && g_mode == MODE_EYE && now - s_bDown > 650) {
    s_bLong = true; s_eyeStyle = (uint8_t)((s_eyeStyle + 1) % 3); hapGesture(HG_SETTLE);
  }
  if (M5.BtnB.isPressed() && s_bDown && !s_bLong && g_mode == MODE_CALM && calmIsWaves() && now - s_bDown > 650) {
    s_bLong = true; calmBoatNext(); hapGesture(HG_THREAD);                   // hold B in the wave tank: another boat
  }
  if (M5.BtnB.isPressed() && s_bDown && !s_bLong && g_mode == MODE_MANTIS && g_mantisMode == 2 && now - s_bDown > 650) {
    s_bLong = true; g_mantisMode = 0; g_mantisSing = false; aud::cave(false); hapGesture(HG_SETTLE);   // hold B: leave the cave
  }
  if (M5.BtnB.wasReleased() && s_bDown) { if (!s_bLong) btnBShort(); s_bDown = 0; }

  auto td = M5.Touch.getDetail();
  bool inStage = td.y >= 14 && td.y < H - 14;
  if (td.wasPressed()) {
    if (td.y >= H - 14 && td.y < H) { if (td.x < 90) nextMode(-1); else if (td.x > 230) nextMode(1); }
    else if (inStage) {
      g_tapLatch = true; g_tapX = td.x; g_tapY = td.y;
      s_touchDown = now; s_touchX0 = td.x; s_touchY0 = td.y; s_longFired = false;
      s_centreTouch = g_mode == MODE_TUNNEL && (td.x - W / 2) * (td.x - W / 2) + (td.y - H / 2) * (td.y - H / 2) < 52 * 52;   // re-centre: flying modes only
      if (g_mode == MODE_EYE) eyePoke(td.x, td.y);
      else if (g_mode == MODE_MANTIS) mantisTap(td.x, td.y);
      else if (g_mode == MODE_CALM) calmTouch(td.x, td.y, true);
      else if (g_mode == MODE_ROOMS) roomsTouch(td.x, td.y);
      else if (g_mode == MODE_GARDEN) gardenTouch(td.x, td.y);
      else if (g_mode == MODE_MEDITATE) medTouch(td.x, td.y);
    }
  } else if (td.isPressed() && inStage && s_touchDown) {
    if (abs(td.x - s_touchX0) + abs(td.y - s_touchY0) > 14) { s_touchDown = 0; if (s_cal == CAL_RING) s_cal = CAL_IDLE; }   // a drag
    else {
      uint32_t held = now - s_touchDown;
      if (!s_longFired && !s_centreTouch && held > 600) {
        s_longFired = true;
        if (g_mode == MODE_CALM) { calmLongPress(); hap(60, 60); }
      }
      // hold still near the centre: after 2 s a ring closes in on the finger, then we calibrate
      if (s_centreTouch && s_cal == CAL_IDLE && held > 2000) { s_cal = CAL_RING; s_calT = now; s_calX = td.x; s_calY = td.y; }
      if (s_cal == CAL_RING && now - s_calT > 1200) { s_cal = CAL_RUN; s_calT = now; s_calN = 0; s_calS[0] = s_calS[1] = s_calS[2] = 0; hapCut(1150); }   // motor off: it would shake the gyro
    }
  }
  if (td.wasReleased() && s_touchDown && s_centreTouch && s_cal == CAL_IDLE && g_mode == MODE_CALM) {
    uint32_t held = now - s_touchDown;
    if (held > 600 && held < 2000) { calmLongPress(); hap(60, 60); }
  }
  if (!td.isPressed()) { s_touchDown = 0; if (s_cal == CAL_RING) s_cal = CAL_IDLE; }
}

// ---------------- sensors ----------------
// g_lookX/Y and g_gravX/Y: exactly V12 (gaze, parallax, swarm chaos, calm gravity were right).
// g_flyX/Y: steering for flight modes, measured from the neutral pose captured during calibration,
// with a deadzone, so holding the Core2 at your natural angle no longer keeps pushing one way.
float g_flyX = 0, g_flyY = 0;
// Flight steering = how far you've ROTATED the Core2 away from the pose you hold it in.
// Reference pose = the gravity direction while you hold it naturally (captured by the centre-hold gesture,
// and automatically each time you enter a flight mode once you're still). The steering vector is the true
// rotation from that pose: axis = ref x now, angle = atan2(|ref x now|, ref . now). It grows the more you
// tip - past 90 degrees too - and stays put while you hold the tilt. Near a flat pose it is exactly the
// flat-pose mapping (ref=(0,0,1): steer = (-gx, gy)), the same axes as gravity and gaze.
static float s_ref[3] = {0.f, 0.f, 1.f};
static float s_gN[3] = {0.f, 0.f, 1.f};
static bool s_autoCap = false;
static float s_stillT = 0;
static uint32_t s_centeredAt = 0;
static void loadCal() {
#ifndef HOST
  Preferences p;
  if (p.begin("synapse", true)) { s_ref[0] = p.getFloat("rx", 0.f); s_ref[1] = p.getFloat("ry", 0.f); s_ref[2] = p.getFloat("rz", 1.f); p.end(); }
#endif
}
static void saveCal() {
#ifndef HOST
  Preferences p;
  if (p.begin("synapse", false)) { p.putFloat("rx", s_ref[0]); p.putFloat("ry", s_ref[1]); p.putFloat("rz", s_ref[2]); p.end(); }
#endif
}
static void capturePose() { s_ref[0] = s_gN[0]; s_ref[1] = s_gN[1]; s_ref[2] = s_gN[2]; s_centeredAt = millis(); }
void flightPoseSoon() { s_autoCap = true; s_stillT = 0; }        // called when a flight mode starts
static inline float shapeSteer(float v) {                    // 3 deg deadzone, 1.0 at ~34 deg, growing faster beyond
  float a = fabsf(v) - 0.05f;
  if (a <= 0) return 0;
  float s = a / 0.55f;
  float o = s * (0.55f + 0.45f * fminf(s, 2.5f));
  return copysignf(fminf(o, 3.f), v);
}
static inline float softDz(float v, float dz) { return v > dz ? v - dz : (v < -dz ? v + dz : 0.f); }
static void sampleImu() {
  if (!M5.Imu.update()) return;
  auto d = M5.Imu.getImuData();
  float ax = d.accel.x, ay = d.accel.y, az = d.accel.z;
  g_ax = g_ax * 0.7f + ax * 0.3f; g_ay = g_ay * 0.7f + ay * 0.3f; g_az = g_az * 0.7f + az * 0.3f;
  g_gx = d.gyro.x; g_gy = d.gyro.y; g_gz = d.gyro.z;
  // accelerometer -> screen, consistent with the gyro half below (IMU X = screen right, Y = screen up):
  g_gravX = -g_ax; g_gravY = g_ay; g_gravZ = fabsf(g_az);
  g_gyroX = g_gx; g_gyroY = g_gy; g_gyroZ = g_gz;
  float tx = -g_ax, ty = g_ay;
  if (s_cal == CAL_RUN) s_calN++;
  if (fabsf(tx) < 0.08f) tx = 0;
  if (fabsf(ty) < 0.08f) ty = 0;
  if (fabsf(g_gx) + fabsf(g_gy) > 8.f) { g_lookX += g_gy * 0.0025f; g_lookY += g_gx * 0.0025f; }
  float k = (fabsf(tx) + fabsf(ty) < 0.15f) ? 0.22f : 0.12f;
  g_lookX += (tx - g_lookX) * k; g_lookY += (ty - g_lookY) * k;
  g_lookX = clampf(g_lookX, -1.1f, 1.1f); g_lookY = clampf(g_lookY, -1.1f, 1.1f);
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  g_jolt = fabsf(mag - 1.f);
  if (mag > 0.3f && fabsf(mag - 1.f) < 0.45f) {                // unit gravity, shakes rejected
    for (int i = 0; i < 3; i++) s_gN[i] += ((i == 0 ? ax : (i == 1 ? ay : az)) / mag - s_gN[i]) * 0.35f;
    float n = sqrtf(s_gN[0] * s_gN[0] + s_gN[1] * s_gN[1] + s_gN[2] * s_gN[2]) + 1e-6f;
    s_gN[0] /= n; s_gN[1] /= n; s_gN[2] /= n;
  }
  if (s_cal == CAL_RUN) { s_calS[0] += s_gN[0]; s_calS[1] += s_gN[1]; s_calS[2] += s_gN[2]; }
  if (s_autoCap) {                                               // entering a flight mode: centre on how you're holding it
    float gyr = fabsf(g_gx) + fabsf(g_gy) + fabsf(g_gz);
    s_stillT = gyr < 30.f ? s_stillT + g_dt : 0.f;
    if (s_stillT > 0.35f) { capturePose(); s_autoCap = false; }
  }
  {
    float cx = s_ref[1] * s_gN[2] - s_ref[2] * s_gN[1], cy = s_ref[2] * s_gN[0] - s_ref[0] * s_gN[2], cz = s_ref[0] * s_gN[1] - s_ref[1] * s_gN[0];
    float sn = sqrtf(cx * cx + cy * cy + cz * cz), cs = s_ref[0] * s_gN[0] + s_ref[1] * s_gN[1] + s_ref[2] * s_gN[2];
    float ang = atan2f(sn, cs), k = sn > 1e-5f ? ang / sn : 1.f;
    float rx = cx * k, ry = cy * k, rz = cz * k;                    // rotation vector (device frame)
    // Split it the way a person means it, whatever the grip: "right edge dropped" = rotation about the
    // horizontal forward axis F; "top tipped away" = rotation about the horizontal right axis Xh.
    // Flat grip: F = screen-up. Held upright in front of you: F = screen normal (a steering-wheel turn).
    float hx = 1.f - s_ref[0] * s_ref[0], hy = -s_ref[0] * s_ref[1], hz = -s_ref[0] * s_ref[2];
    float hn = sqrtf(hx * hx + hy * hy + hz * hz) + 1e-6f; hx /= hn; hy /= hn; hz /= hn;
    float fx_ = s_ref[1] * hz - s_ref[2] * hy, fy_ = s_ref[2] * hx - s_ref[0] * hz, fz_ = s_ref[0] * hy - s_ref[1] * hx;
    g_flyX = shapeSteer(-(rx * fx_ + ry * fy_ + rz * fz_));
    g_flyY = shapeSteer(-(rx * hx + ry * hy + rz * hz));
  }
  g_shake = g_shake * 0.8f + fabsf(mag - 1.f) * 0.2f;
  if (g_shake > 0.5f && millis() - g_shakeAt > 350) {
    g_shakeAt = millis(); g_shakeKick = true;
    g_hue = fmodf(g_hue + 40.f + (esp_random() % 120), 360.f);
    hap(150, 40);
  }
}
static void calService() {
  uint32_t now = millis();
  if (s_cal == CAL_RING) {                                  // a heartbeat that quickens as the ring closes
    float u = clampf((now - s_calT) / 1200.f, 0.f, 1.f);
    hapRumble(0.25f + u * 0.35f, 1.3f + u * 5.5f, 0.f);
  }
  // M5Unified's own gyro offset calibration runs while the device is held still (its documented method),
  // then the offsets are saved to NVS. The neutral pose for flying is averaged over the same second.
  static bool calOn = false;
  if (s_cal == CAL_RUN && !calOn) { calOn = true; M5.Imu.setCalibration(0, 200, 0); }
  if (s_cal == CAL_RUN && now - s_calT > 1200) {
    M5.Imu.setCalibration(0, 0, 0); calOn = false;
    M5.Imu.saveOffsetToNVS();
    if (s_calN >= 8) {
      float x = s_calS[0], y = s_calS[1], z = s_calS[2], n = sqrtf(x * x + y * y + z * z) + 1e-6f;
      s_ref[0] = x / n; s_ref[1] = y / n; s_ref[2] = z / n; s_centeredAt = millis(); saveCal();
    }
    g_lookX = -g_ax; g_lookY = g_ay;
    s_cal = CAL_DONE; s_calT = now; hapGesture(HG_SETTLE);
  } else if (s_cal == CAL_DONE && now - s_calT > 900) s_cal = CAL_IDLE;
}
static void drawCal() {
  uint32_t now = millis();
  if (g_mode == MODE_TUNNEL && s_centeredAt && now - s_centeredAt < 900 && s_cal == CAL_IDLE) {
    float u = (now - s_centeredAt) / 900.f;
    canvas.drawCircle(160, 120, 14 + (int)(u * 30.f), wire::LIME);
    canvas.setTextColor(wire::LIME); canvas.setCursor(160 - 24, 146); canvas.print("centered");
  }
  if (s_cal == CAL_RING) {
    float u = clampf((now - s_calT) / 1200.f, 0.f, 1.f), r = 64.f * (1.f - u) * (1.f - u * 0.3f);
    canvas.drawCircle(s_calX, s_calY, (int)r + 2, wire::PLUM);
    canvas.drawCircle(s_calX, s_calY, (int)r + 1, wire::TEAL);
    canvas.drawCircle(s_calX, s_calY, (int)r, wire::LIME);
    for (int k = 0; k < 6; k++) {                              // six sigils converge with the ring
      float a = k * 1.0471976f + u * 3.f;
      canvas.fillCircle(s_calX + (int)(cosf(a) * r), s_calY + (int)(sinf(a) * r), 2, wire::LIME);
    }
    canvas.fillCircle(s_calX, s_calY, 1 + (int)(u * 3.f), wire::LIME);
  } else if (s_cal == CAL_RUN || s_cal == CAL_DONE) {
    bool run = s_cal == CAL_RUN;
    canvas.fillRoundRect(60, 96, 200, 48, 10, rgb565(4, 18, 20));
    canvas.drawRoundRect(60, 96, 200, 48, 10, wire::TEAL);
    canvas.drawRoundRect(61, 97, 198, 46, 9, wire::PLUM);
    canvas.setTextSize(1); canvas.setTextColor(wire::LIME);
    const char *t = run ? "hold it how you like - calibrating" : "centered on your grip";
    canvas.setCursor(160 - (int)strlen(t) * 3, 108); canvas.print(t);
    float u = run ? clampf((now - s_calT) / 1200.f, 0.f, 1.f) : 1.f;
    canvas.fillRect(80, 126, (int)(160 * u), 4, wire::TEAL);
    canvas.drawRect(80, 126, 160, 4, wire::PLUM);
  }
}

// ============================================================
//  SWARM
// ============================================================
static const int N_PART = 72, N_TRAIL = 96;
struct Particle { float x, y, vx, vy, hue; };
struct Trail { int16_t x, y; uint16_t c; uint8_t life; };
static Particle g_p[N_PART];
static Trail g_tr[N_TRAIL];
static int g_trI = 0;

static void seedParticles() {
  for (int i = 0; i < N_PART; i++) {
    g_p[i].x = (float)(esp_random() % W); g_p[i].y = (float)(14 + esp_random() % (H - 30));
    g_p[i].vx = ((int)(esp_random() % 100) - 50) * 0.03f; g_p[i].vy = ((int)(esp_random() % 100) - 50) * 0.03f;
    g_p[i].hue = g_hue + (esp_random() % 60) - 30;
  }
}
static void addTrail(int x, int y, uint16_t c) {
  g_tr[g_trI] = {(int16_t)x, (int16_t)y, c, 40};
  g_trI = (g_trI + 1) % N_TRAIL;
}
static void drawPsyBg() {
  float pulse = 0.08f + g_level * 0.18f + g_peak * 0.12f;
  // mantis aura: teal / plum breathing with the room's sound
  for (int y = 14; y < H - 14; y += 4) {
    float u = (float)(y - 14) / (H - 28), s = 0.5f + 0.5f * sinf(u * 6.f + g_t * 0.7f);
    float k = 0.10f + g_level * 0.25f;
    canvas.fillRect(0, y, W, 4, rgb565((uint8_t)(93 * s * k), (uint8_t)(115 * (1.f - s) * k), (uint8_t)((115 * (1.f - s) + 93 * s) * k)));
  }
  for (int i = 0; i < 12; i++) {
    float yy = 16.f + fmodf(i * 19.f + g_t * (12.f + g_level * 40.f) + g_lookY * 30.f + 400.f, (float)(H - 30));
    canvas.drawFastHLine(0, (int)yy, W, hsv565(g_hue + i * 18.f + g_t * 15.f, 0.7f, pulse * (0.5f + 0.5f * sinf(i + g_t * 2.f))));
  }
  for (int i = 0; i < 10; i++) {
    float xx = fmodf(i * 37.f + g_t * (8.f + g_peak * 25.f) + g_lookX * 40.f + 400.f, (float)W);
    canvas.drawFastVLine((int)xx, 14, H - 28, hsv565(g_hue + 80.f + i * 12.f, 0.6f, 0.06f + g_peak * 0.15f * (0.5f + 0.5f * sinf(i * 1.7f + g_t))));
  }
}
static void drawTinyMantis(int cx, int cy) {
  const int sc = 5;
  int dw = MANTIS_W / sc, dh = MANTIS_H / sc, ox = cx - dw / 2, oy = cy - dh / 2;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      uint16_t c = mantis_splash[(y * sc) * MANTIS_W + x * sc];
      if (c) canvas.drawPixel(ox + x, oy + y, c);
    }
}
// ---- CHAOS: jelly globs under real gravity. They merge (weakly) into bigger globs, burst into
//      droplets when you shake, flow around, avoid your finger, and never settle into a dead heap. ----
struct Jelly { float x, y, vx, vy, m, hue, wob; bool live; };
static const int NJ = 70;
static Jelly s_j[NJ];
static bool s_jInit = false;
static inline float jR(float m) { return 4.f + sqrtf(m) * 3.2f; }
static int jSpawn(float x, float y, float vx, float vy, float m, float hue) {
  for (int i = 0; i < NJ; i++) if (!s_j[i].live) { s_j[i] = {x, y, vx, vy, m, hue, (esp_random() % 628) / 100.f, true}; return i; }
  return -1;
}
static void chaosJelly() {
  float dt = fminf(g_dt, 0.05f);
  if (!s_jInit) {
    s_jInit = true;
    for (int i = 0; i < 26; i++) jSpawn(30.f + (esp_random() % 260), 30.f + (esp_random() % 170), 0, 0, 1.f + (esp_random() % 30) / 10.f, (float)(esp_random() % 360));
  }
  float soundHue = g_hue + g_level * 100.f + aud::centroid * 90.f;
  // shake: globs burst into droplets of random sizes, flung apart
  if (g_shakeKick) {
    for (int i = 0; i < NJ; i++) {
      Jelly &b = s_j[i];
      if (!b.live) continue;
      float a0 = (esp_random() % 628) / 100.f, sp = 160.f + (esp_random() % 220);
      if (b.m > 1.6f) {
        int pieces = 2 + (int)(esp_random() % 4);
        float left = b.m;
        for (int k = 0; k < pieces && left > 0.4f; k++) {
          float share = k == pieces - 1 ? left : left * (0.2f + (esp_random() % 50) / 100.f);
          float a = a0 + k * 6.2831853f / pieces + ((int)(esp_random() % 60) - 30) / 100.f;
          if (k == 0) { b.m = share; b.vx += cosf(a) * sp; b.vy += sinf(a) * sp; }
          else jSpawn(b.x + cosf(a) * 4.f, b.y + sinf(a) * 4.f, b.vx + cosf(a) * sp, b.vy + sinf(a) * sp, share, b.hue + k * 25.f);
          left -= share;
        }
      } else { b.vx += cosf(a0) * sp; b.vy += sinf(a0) * sp; }
    }
  }
  // pairs: squishy contact (no stacking), weak cohesion nearby, gentle merging when they meet slowly
  for (int i = 0; i < NJ; i++) {
    Jelly &a = s_j[i];
    if (!a.live) continue;
    float ra = jR(a.m);
    for (int k = i + 1; k < NJ; k++) {
      Jelly &b = s_j[k];
      if (!b.live) continue;
      float dx = b.x - a.x, dy = b.y - a.y, d2 = dx * dx + dy * dy, rr = ra + jR(b.m);
      if (d2 > (rr + 16.f) * (rr + 16.f) || d2 < 0.01f) continue;
      float d = sqrtf(d2), nx = dx / d, ny = dy / d;
      if (d < rr) {
        float rv = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny;
        if (fabsf(rv) < 45.f && a.m + b.m < 14.f && (esp_random() % 1000) < 6) {       // merge (weak tendency)
          float M = a.m + b.m;
          a.x = (a.x * a.m + b.x * b.m) / M; a.y = (a.y * a.m + b.y * b.m) / M;
          a.vx = (a.vx * a.m + b.vx * b.m) / M; a.vy = (a.vy * a.m + b.vy * b.m) / M;
          a.m = M; a.wob += 1.5f; b.live = false; ra = jR(a.m);
          continue;
        }
        float push = (rr - d) * 9.f;                                          // jelly pressure
        a.vx -= nx * push * dt * 60.f * b.m / (a.m + b.m); a.vy -= ny * push * dt * 60.f * b.m / (a.m + b.m);
        b.vx += nx * push * dt * 60.f * a.m / (a.m + b.m); b.vy += ny * push * dt * 60.f * a.m / (a.m + b.m);
      } else {
        float pull = 26.f * dt;                                                // weak cohesion
        a.vx += nx * pull; a.vy += ny * pull; b.vx -= nx * pull; b.vy -= ny * pull;
      }
    }
  }
  // forces, motion, walls
  auto td = M5.Touch.getDetail();
  bool touching = td.isPressed() && td.y > 16 && td.y < H - 18;
  float G = 330.f;
  for (int i = 0; i < NJ; i++) {
    Jelly &b = s_j[i];
    if (!b.live) continue;
    b.vx += g_gravX * G * dt; b.vy += g_gravY * G * dt;
    float st = 34.f + g_level * 110.f + aud::onset * 140.f;                     // the room's sound keeps them stirring
    if (aud::onset > 0.35f && (esp_random() % 3) == 0) { b.vx += ((int)(esp_random() % 200) - 100) * aud::onset; b.vy -= (60 + esp_random() % 120) * aud::onset; }   // hops on the beat
    if (b.m > 6.f && (esp_random() % 1000) < (int)(dt * 150.f)) {            // big globs sometimes pinch off a droplet
      float part = b.m * (0.25f + (esp_random() % 30) / 100.f), a = (esp_random() % 628) / 100.f;
      if (jSpawn(b.x + cosf(a) * jR(b.m), b.y + sinf(a) * jR(b.m), b.vx + cosf(a) * 90.f, b.vy + sinf(a) * 90.f, part, b.hue + 30.f) >= 0) b.m -= part;
    }
    b.vx += sinf(g_t * 1.3f + b.y * 0.03f + i) * st * dt; b.vy += cosf(g_t * 1.1f + b.x * 0.03f) * st * dt;
    if (touching) {
      float dx = b.x - td.x, dy = b.y - td.y, d2 = dx * dx + dy * dy + 1.f;
      if (d2 < 60.f * 60.f) { float d = sqrtf(d2); b.vx += dx / d * 900.f * dt; b.vy += dy / d * 900.f * dt; }
    }
    float drag = 1.f - 1.4f * dt; b.vx *= drag; b.vy *= drag;
    b.x += b.vx * dt; b.y += b.vy * dt;
    float r = jR(b.m);
    if (b.x < r) { b.x = r; b.vx = fabsf(b.vx) * 0.45f; }
    if (b.x > W - r) { b.x = W - r; b.vx = -fabsf(b.vx) * 0.45f; }
    if (b.y < 15 + r) { b.y = 15 + r; b.vy = fabsf(b.vy) * 0.45f; }
    if (b.y > H - 15 - r) { b.y = H - 15 - r; b.vy = -fabsf(b.vy) * 0.45f; }
    b.wob = fmaxf(0.f, b.wob - dt * 2.f);
  }
  // draw: liquid bridges first (metaball look), then glossy jelly bodies
  for (int i = 0; i < NJ; i++) {
    if (!s_j[i].live) continue;
    for (int k = i + 1; k < NJ; k++) {
      if (!s_j[k].live) continue;
      float dx = s_j[k].x - s_j[i].x, dy = s_j[k].y - s_j[i].y, d = sqrtf(dx * dx + dy * dy);
      float ri = jR(s_j[i].m), rk = jR(s_j[k].m);
      if (d < (ri + rk) * 1.15f) {
        float br = fminf(ri, rk) * (1.15f - d / (ri + rk)) * 1.6f;
        if (br > 1.5f) canvas.fillCircle((int)((s_j[i].x * rk + s_j[k].x * ri) / (ri + rk)), (int)((s_j[i].y * rk + s_j[k].y * ri) / (ri + rk)), (int)br,
                                         hsv565(soundHue + s_j[i].hue * 0.3f, 0.75f, 0.55f + g_level * 0.3f));
      }
    }
  }
  for (int i = 0; i < NJ; i++) {
    Jelly &b = s_j[i];
    if (!b.live) continue;
    float r = jR(b.m) * (1.f + aud::bass * 0.12f + 0.08f * b.wob * sinf(g_t * 20.f));
    float h = soundHue + b.hue * 0.3f;
    canvas.fillCircle((int)b.x, (int)b.y, (int)r + 1, hsv565(h + 20.f, 0.9f, 0.3f));
    canvas.fillCircle((int)b.x, (int)b.y, (int)r, hsv565(h, 0.75f, 0.55f + g_level * 0.3f));
    canvas.fillCircle((int)(b.x - r * 0.2f), (int)(b.y - r * 0.2f), (int)(r * 0.62f), hsv565(h - 10.f, 0.55f, 0.8f + g_level * 0.2f));
    canvas.fillCircle((int)(b.x - r * 0.4f), (int)(b.y - r * 0.45f), (int)fmaxf(1.f, r * 0.18f), rgb565(255, 255, 255));
  }
  if (touching) drawTinyMantis(td.x, td.y);
}

static void modeSwarm() {
  canvas.fillSprite(rgb565(8, 4, 16));
  drawPsyBg();
  for (int i = 0; i < N_TRAIL; i++) {
    Trail &t = g_tr[i];
    if (t.life < 2) continue;
    int r = 1 + (t.life >> 4);
    canvas.fillCircle(t.x, t.y, r, t.c);
    t.life = (uint8_t)(t.life * 0.88f);
  }
  float gx = 0, gy = 0;
  if (g_swarmVar == SV_CHAOS) { chaosJelly(); return; }
  if (g_shakeKick) {                                    // shake: the swarm bursts apart
    for (int i = 0; i < N_PART; i++) {
      float a = (esp_random() % 6283) / 1000.f, s = 6.f + (esp_random() % 800) / 100.f;
      float ox = g_p[i].x - W * 0.5f, oy = g_p[i].y - H * 0.5f, on = sqrtf(ox * ox + oy * oy) + 1.f;
      g_p[i].vx += cosf(a) * s + ox / on * 5.f; g_p[i].vy += sinf(a) * s + oy / on * 5.f;
      g_p[i].hue += 60.f;
      addTrail((int)g_p[i].x, (int)g_p[i].y, hsv565(g_hue + i * 5.f, 0.9f, 0.9f));
    }
  }
  float pulse = 0.45f + g_level * 1.5f;
  float soundHue = g_hue + g_level * 100.f + g_peak * 40.f + aud::centroid * 90.f;
  float metaR = (g_swarmVar == SV_ORBIT) ? 220.f : 500.f;
  float metaPull = (g_swarmVar == SV_ORBIT) ? 0.0004f : 0.0015f;
  for (int i = 0; i < N_PART; i++)
    for (int j = i + 1; j < N_PART; j += 3) {
      float dx = g_p[i].x - g_p[j].x, dy = g_p[i].y - g_p[j].y, d2 = dx * dx + dy * dy;
      if (d2 < metaR && d2 > 1.f) {
        float tt = 1.f - d2 / metaR;
        int br = 2 + (int)(tt * (g_swarmVar == SV_ORBIT ? 4.f : 8.f) * (0.5f + g_level + aud::bass * 0.6f));
        canvas.fillCircle((int)((g_p[i].x + g_p[j].x) * 0.5f), (int)((g_p[i].y + g_p[j].y) * 0.5f), br,
                          hsv565(soundHue + i + j, 0.75f, 0.2f + tt * (0.4f + aud::onset * 0.4f)));
        g_p[i].vx -= dx * metaPull * tt; g_p[i].vy -= dy * metaPull * tt;
        g_p[j].vx += dx * metaPull * tt; g_p[j].vy += dy * metaPull * tt;
      }
    }
  for (int i = 0; i < N_PART; i++) {
    Particle &p = g_p[i];
    float cx = W * 0.5f + sinf(g_t * 0.6f + i * 0.1f) * 22.f * g_peak;
    float cy = H * 0.5f + cosf(g_t * 0.5f) * 16.f * g_level;
    if (g_swarmVar == SV_FLOCK) {
      p.vx += (cx - p.x) * 0.0022f * pulse; p.vy += (cy - p.y) * 0.0022f * pulse;
      for (int j = 0; j < N_PART; j += 2) {
        if (j == i) continue;
        float sx = p.x - g_p[j].x, sy = p.y - g_p[j].y, d2 = sx * sx + sy * sy + 0.01f;
        if (d2 < 900.f) { p.vx += sx / d2 * 12.f; p.vy += sy / d2 * 12.f; }
      }
      p.vx *= 0.94f; p.vy *= 0.94f;
    } else if (g_swarmVar == SV_ORBIT) {
      float dx = p.x - cx, dy = p.y - cy;
      p.vx += -dy * 0.006f * pulse - dx * 0.001f; p.vy += dx * 0.006f * pulse - dy * 0.001f;
      p.vx *= 0.95f; p.vy *= 0.95f;
    } else {
      p.vx += gx * 1.2f + sinf(g_t * 2.f + p.y * 0.05f) * 0.15f * (0.3f + g_level);
      p.vy += gy * 1.2f + cosf(g_t * 1.7f + p.x * 0.05f) * 0.15f * (0.3f + g_level);
      p.vx *= 0.92f; p.vy *= 0.92f;
    }
    if (aud::onset > 0.4f) { p.vx *= 1.4f; p.vy *= 1.4f; }
    p.x += p.vx; p.y += p.vy;
    if (p.x < 4) { p.x = 4; p.vx *= -0.6f; }
    if (p.x > W - 5) { p.x = W - 5; p.vx *= -0.6f; }
    if (p.y < 16) { p.y = 16; p.vy *= -0.6f; }
    if (p.y > H - 18) { p.y = H - 18; p.vy *= -0.6f; }
    float h = soundHue + p.hue * 0.3f + i * 2.f, v = 0.4f + g_level * 0.5f + g_peak * 0.15f;
    int r = 3 + (int)(g_level * 7.f + aud::bass * 5.f + aud::onset * 3.f) + (i & 1);   // the mic makes them swell and throb
    uint16_t col = hsv565(h, 0.8f, v);
    canvas.fillCircle((int)p.x, (int)p.y, r, col);
    if (r > 4) canvas.fillCircle((int)p.x - 1, (int)p.y - 1, r / 2, hsv565(h + 20.f, 0.5f, fminf(1.f, v + 0.2f)));
    if ((i & 1) == 0) addTrail((int)p.x, (int)p.y, col);
  }
  auto td = M5.Touch.getDetail();
  if (td.isPressed() && td.y > 16 && td.y < H - 18) {
    int tx = td.x, ty = td.y;
    drawTinyMantis(tx, ty);
    for (int i = 0; i < 8; i++) addTrail(tx + (int)(esp_random() % 11) - 5, ty + (int)(esp_random() % 11) - 5, hsv565(soundHue + 40.f, 0.9f, 0.7f));
    for (int i = 0; i < N_PART; i++) {
      float dx = g_p[i].x - tx, dy = g_p[i].y - ty, d2 = dx * dx + dy * dy + 0.01f;
      if (d2 < 900.f) { g_p[i].vx += dx / d2 * 40.f; g_p[i].vy += dy / d2 * 40.f; }
      else if (d2 < 10000.f) { g_p[i].vx -= dx * 0.012f; g_p[i].vy -= dy * 0.012f; }
    }
  }
}

// ============================================================
//  EYE — hypnotic moire, wet eyeball, real eyelids, squirting tears
// ============================================================
struct Tear { float x, y, vx, vy, life; uint8_t kind; };   // 0 squirt, 1 drip, 2 splash
static Tear s_tears[48];
static float s_pain = 0, s_flinch = 0, s_shakeEye = 0, s_dizzy = 0, s_blinkE = 0, s_blinkTE = 2.f;
static float s_irisHue = 110.f, s_gzX = 0, s_gzY = 0, s_startle = 0, s_angry = 0;
static int s_pokeX = 0, s_pokeY = 0;
static uint32_t s_pokeAt = 0;
static const int ERX = 62, ERY = 44;

// ---- cat & dragon irises (long-press B). The basic eye is untouched. Everything here is clipped to the
//      eyeball ellipse so nothing spills past the lids. ----
static float s_ecx = 160, s_ecy = 120;                          // eyeball centre (set by the eye each frame)
static float s_rage = 0, s_catSnap = 0, s_quietT = 0; static bool s_catSlow = false;
static inline float eyeHalfW(float y) { float u = (y - s_ecy) / (ERY - 1); return u * u >= 1.f ? -1.f : (ERX - 1) * sqrtf(1.f - u * u); }
static void spanEllipse(float cx, float cy, float rx, float ry, uint16_t c) {   // filled ellipse ∩ eyeball
  for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++) {
    float v = (y - cy) / ry; if (v * v > 1.f) continue;
    float w = rx * sqrtf(1.f - v * v), ew = eyeHalfW((float)y); if (ew < 0) continue;
    int x0 = (int)fmaxf(cx - w, s_ecx - ew), x1 = (int)fminf(cx + w, s_ecx + ew);
    if (x1 >= x0) canvas.drawFastHLine(x0, y, x1 - x0 + 1, c);
  }
}
static inline bool inEye(float x, float y) { float ew = eyeHalfW(y); return ew >= 0 && fabsf(x - s_ecx) <= ew; }
static void eyeLine(float x0, float y0, float x1, float y1, uint16_t c) {       // trim to the eyeball
  if (!inEye(x0, y0)) return;
  if (!inEye(x1, y1)) { float a = 0, b = 1; for (int k = 0; k < 6; k++) { float mm = (a + b) * 0.5f; if (inEye(x0 + (x1 - x0) * mm, y0 + (y1 - y0) * mm)) a = mm; else b = mm; } x1 = x0 + (x1 - x0) * a; y1 = y0 + (y1 - y0) * a; }
  canvas.drawLine((int)x0, (int)y0, (int)x1, (int)y1, c);
}
static void eyeIrisStyled(float ix, float iy) {
  float slitK = 1.f - clampf(g_level * 1.4f + aud::bass * 0.6f, 0.f, 1.f);   // sound opens the slit, quiet narrows it
  if (s_eyeStyle == 1) {                                                     // CAT: big amber-green iris, vertical slit
    int ir = (int)(ERY - 4 + g_peak * 3.f);
    float h0 = 95.f + sinf(g_t * 0.2f) * 10.f;
    spanEllipse(ix, iy, ir + 2, ir + 2, rgb565(20, 30, 8));
    for (int k = 0; k < 5; k++) spanEllipse(ix, iy, ir - k * (ir / 6), ir - k * (ir / 6), hsv565(h0 - k * 14.f, 0.9f - k * 0.05f, 0.45f + k * 0.12f + g_level * 0.1f));
    for (int k = 0; k < 36; k++) {
      float a = k * 0.1745f + g_t * 0.05f;
      eyeLine(ix + cosf(a) * ir * 0.35f, iy + sinf(a) * ir * 0.35f, ix + cosf(a) * (ir - 2), iy + sinf(a) * (ir - 2), hsv565(h0 + (k % 3) * 8.f, 0.7f, (k & 1) ? 0.85f : 0.5f));
    }
    float open = (1.f - slitK) * (1.f - s_catSnap);
    float pw = fmaxf(2.f, 3.f + open * 15.f - s_pain * 2.f), ph = ir * 0.92f;
    spanEllipse(ix, iy, pw + 2, ph + 1, hsv565(150.f, 0.8f, 0.25f + open * 0.3f));   // tapetum glow
    spanEllipse(ix, iy, pw, ph, rgb565(4, 4, 6));
    if (inEye(ix - ir / 3, iy - ir / 3)) canvas.fillCircle((int)ix - ir / 3, (int)iy - ir / 3, 5, rgb565(255, 255, 255));
    if (inEye(ix + ir / 3, iy + ir / 4)) canvas.fillCircle((int)ix + ir / 3, (int)iy + ir / 4, 2, rgb565(230, 240, 255));
  } else {                                                                   // DRAGON: molten iris, knife slit, scales
    int ir = ERY - 1;
    float hot = clampf(s_rage, 0.f, 1.f);
    float fl = 0.5f + 0.5f * sinf(g_t * 9.f) * aud::onset;
    spanEllipse(ix, iy, ir + 2, ir + 2, rgb565(40, 4, 0));
    for (int k = 0; k < 6; k++)
      spanEllipse(ix, iy, ir - k * (ir / 7), ir - k * (ir / 7), hsv565(8.f + k * 9.f + g_level * 10.f + hot * 20.f, 1.f - k * 0.06f - hot * 0.3f, 0.35f + k * 0.12f + fl * 0.1f + hot * 0.2f));
    for (int k = 0; k < 28; k++) {
      float a = k * 0.2244f + sinf(g_t * 2.f + k) * 0.05f, fl2 = 0.55f + 0.45f * sinf(g_t * (5.f + hot * 8.f) + k * 1.7f);
      eyeLine(ix + cosf(a) * 7.f, iy + sinf(a) * 7.f, ix + cosf(a) * (ir - 3) * fl2, iy + sinf(a) * (ir - 3) * fl2, hsv565(30.f + (k & 1) * 15.f + hot * 15.f, 0.9f - hot * 0.4f, 1.f));
    }
    for (int k = 0; k < 18; k++) {
      float a = k * 0.349f, sx = ix + cosf(a) * (ir - 1), sy = iy + sinf(a) * (ir - 1);
      if (inEye(sx, sy + 4) && inEye(sx, sy - 4)) canvas.drawCircle((int)sx, (int)sy, 4, rgb565(90, 20, 4));
    }
    float pw = fmaxf(1.f, 1.5f + (1.f - slitK) * 5.f - s_pain - hot * 1.5f), ph = ir * 0.95f;
    spanEllipse(ix, iy, pw, ph, rgb565(2, 0, 0));
    if (inEye(ix - ir / 3, iy - ir / 3)) canvas.fillCircle((int)ix - ir / 3, (int)iy - ir / 3, 4, rgb565(255, 240, 200));
  }
}
// dragon: pokes stoke it (embers, sparks, smoke with the tears); cat: pokes snap the pupil, quiet brings slow blinks
struct EyeFx { float x, y, vx, vy, life, r; uint8_t kind; bool live; };
static EyeFx s_efx[60];
static void efx(float x, float y, float vx, float vy, float life, float r, uint8_t kind) { for (auto &e : s_efx) if (!e.live) { e = {x, y, vx, vy, life, r, kind, true}; return; } }
static void eyeStylePoke() {
  if (s_eyeStyle == 2) {
    s_rage = fminf(1.5f, s_rage + 0.35f);
    int n = 6 + (int)(s_rage * 10.f);
    for (int k = 0; k < n; k++) {
      bool left = k & 1;
      float sx = s_ecx + (left ? -ERX * 0.9f : ERX * 0.9f), sy = s_ecy + 4.f;
      efx(sx, sy, (left ? -1.f : 1.f) * (30.f + (esp_random() % 80)), -60.f - (esp_random() % 120), 0.6f + (esp_random() % 60) / 100.f, 1.f, 0);   // embers
    }
    if (s_rage > 0.6f) for (int k = 0; k < 3; k++) efx(s_ecx + ((int)(esp_random() % 80) - 40), s_ecy - ERY, ((int)(esp_random() % 40) - 20), -25.f, 1.6f, 4.f, 1);  // smoke
    hapGesture(HG_CRACK);
  } else if (s_eyeStyle == 1) s_catSnap = 1.f;
}
static void eyeStyleFx(float dt) {
  s_rage = fmaxf(0.f, s_rage - dt * 0.12f);
  s_catSnap = fmaxf(0.f, s_catSnap - dt * 0.8f);
  if (s_eyeStyle == 2 && s_rage > 0.3f && (esp_random() % 100) < (int)(s_rage * 12.f))       // it keeps smouldering
    efx(s_ecx + ((int)(esp_random() % 60) - 30), s_ecy - ERY + 6, ((int)(esp_random() % 30) - 15), -20.f, 1.4f, 3.f, 1);
  if (s_eyeStyle == 1) {                                                                        // cat kiss
    s_quietT = g_level < 0.05f ? s_quietT + dt : 0.f;
    if (s_quietT > 4.f && (esp_random() % 1000) < 6) { s_blinkE = 1.f; s_catSlow = true; s_quietT = 0; }
    if (s_blinkE <= 0.f) s_catSlow = false;
  }
  for (auto &e : s_efx) {
    if (!e.live) continue;
    e.life -= dt; if (e.life <= 0) { e.live = false; continue; }
    if (e.kind == 0) { e.vy += 90.f * dt; e.x += e.vx * dt; e.y += e.vy * dt;
      canvas.fillCircle((int)e.x, (int)e.y, e.life > 0.4f ? 2 : 1, (esp_random() & 1) ? rgb565(255, 200, 60) : rgb565(255, 110, 20)); }
    else { e.x += e.vx * dt; e.y += e.vy * dt; e.r += dt * 9.f; canvas.drawCircle((int)e.x, (int)e.y, (int)e.r, rgb565((uint8_t)(60 * e.life), (uint8_t)(55 * e.life), (uint8_t)(55 * e.life))); }
  }
}

static void spawnTear(float x, float y, float vx, float vy, uint8_t kind) {
  for (auto &t : s_tears) if (t.life <= 0) { t = {x, y, vx, vy, 1.f, kind}; return; }
}
static void eyePoke(int x, int y) {
  eyeStylePoke();
  float cx = W / 2 + g_lookX * 12.f, cy = H / 2 + g_lookY * 10.f;
  float dx = (x - cx) / ERX, dy = (y - cy) / ERY;
  if (dx * dx + dy * dy > 1.25f) return;
  float ix = cx + s_gzX, iy = cy + s_gzY;
  bool bull = (x - ix) * (x - ix) + (y - iy) * (y - iy) < 16 * 16;     // right in the pupil!
  float hurt = bull ? 0.55f : 0.35f;
  s_pain = fminf(1.6f, s_pain + hurt);
  s_flinch = 1.f; s_shakeEye = 1.f; s_angry = fminf(1.f, s_angry + 0.4f);
  s_pokeX = x; s_pokeY = y; s_pokeAt = millis();
  hap(255, bull ? 140 : 90);
  int n = 6 + (int)(s_pain * 8.f) + (bull ? 6 : 0);
  for (int i = 0; i < n; i++) {
    bool left = i & 1;
    float sx = cx + (left ? -ERX * 0.92f : ERX * 0.92f), sy = cy + 6.f;
    float sp = 90.f + (esp_random() % 120) + s_pain * 80.f;
    spawnTear(sx, sy, (left ? -1.f : 1.f) * sp * (0.6f + (esp_random() % 50) / 100.f), -120.f - (esp_random() % 140) - s_pain * 60.f, 0);
  }
}

static void moireBg() {
  // Set A: audio rings (centre breathes with bass, spacing with level, spin with mids)
  // Set B: IMU rings (centre slides with tilt, colour from tilt direction)
  static float phA = 0, phB = 0;
  phA += g_dt * (40.f + aud::bass * 260.f);
  phB += g_dt * (25.f + (fabsf(g_gx) + fabsf(g_gy)) * 0.6f);
  int fA = (int)(1500 + g_level * 900.f + aud::onset * 400.f);        // ring frequency, 1/64 steps
  int fB = (int)(1620 + (fabsf(g_lookX) + fabsf(g_lookY)) * 500.f);
  int twist = (int)(aud::mid * 4.f);
  int cax = 80 + (int)(sinf(g_t * 0.7f) * 6.f * (0.3f + aud::bass)), cay = 60 + (int)(cosf(g_t * 0.5f) * 4.f);
  int cbx = 80 - (int)(g_lookX * 55.f), cby = 60 + (int)(g_lookY * 42.f);
  int pA = (int)phA, pB = (int)phB;
  auto rows_ = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
    uint8_t *row = fx::buf + y * fx::LW;
    for (int x = 0; x < fx::LW; x++) {
      int ra = fx::radAt(x - cax, y - cay);
      int aa = twist ? (fx::tunAt(x - cax, y - cay) >> 8) * twist : 0;
      int rb = fx::radAt(x - cbx, y - cby);
      int a = fx::sn[(uint8_t)(((ra * fA) >> 6) + aa - pA)] + 128;
      int b = fx::sn[(uint8_t)(((rb * fB) >> 6) + pB)] + 128;
      row[x] = (uint8_t)(((a >> 4) << 4) | (b >> 4));
    }
  }
  };
  fx::parallel(rows_);
  float hA = g_hue + g_level * 140.f + aud::centroid * 120.f;
  float hB = atan2f(g_lookY, g_lookX + 0.0001f) * 57.3f + 180.f + g_hue * 0.3f;
  uint16_t ca = hsv565(hA, 0.9f, 1.f), cb = hsv565(hB, 0.85f, 1.f);
  float ar = ((ca >> 11) << 3), ag = (((ca >> 5) & 63) << 2), ab = ((ca & 31) << 3);
  float br = ((cb >> 11) << 3), bg = (((cb >> 5) & 63) << 2), bb = ((cb & 31) << 3);
  float gA = 0.34f + g_level * 0.35f, gB = 0.3f + fminf(0.3f, fabsf(g_lookX) + fabsf(g_lookY)) * 0.5f;
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++) {
      float wa = i / 15.f, wb = j / 15.f;
      wa = wa * wa * gA; wb = wb * wb * gB;
      fx::palSet(i * 16 + j, (uint8_t)clampf(ar * wa + br * wb, 0, 255), (uint8_t)clampf(ag * wa + bg * wb, 0, 255),
                 (uint8_t)clampf(ab * wa + bb * wb, 0, 255));
    }
  fx::present(canvas);
}

static void modeEye() {
  float dt = g_dt;
  moireBg();

  if (g_shakeKick) { s_irisHue = (float)(esp_random() % 360); s_dizzy = 1.2f; }
  s_dizzy = fmaxf(0.f, s_dizzy - dt);
  s_pain = fmaxf(0.f, s_pain - dt * 0.22f);
  s_flinch = fmaxf(0.f, s_flinch - dt * 2.2f);
  s_shakeEye = fmaxf(0.f, s_shakeEye - dt * 3.f);
  s_angry = fmaxf(0.f, s_angry - dt * 0.12f);
  if (aud::onset > 0.6f && s_pain < 0.3f) s_startle = 1.f;
  s_startle = fmaxf(0.f, s_startle - dt * 3.f);

  float jit = s_shakeEye * (3.f + s_pain * 4.f);
  float cx = W / 2 + (g_eyeTrack ? g_lookX * 12.f : 0) + ((int)(esp_random() % 21) - 10) * jit * 0.1f;
  float cy = H / 2 + (g_eyeTrack ? g_lookY * 10.f : 0) + ((int)(esp_random() % 21) - 10) * jit * 0.1f;

  // gaze target: tilt, or glare at the finger that hurt it, or dizzy spin
  float tx = g_eyeTrack ? clampf(g_lookX * 30.f, -26.f, 26.f) : 0, ty = g_eyeTrack ? clampf(g_lookY * 22.f, -16.f, 16.f) : 0;
  if (millis() - s_pokeAt < 1500) { tx = clampf((s_pokeX - cx) * 0.5f, -26.f, 26.f); ty = clampf((s_pokeY - cy) * 0.5f, -16.f, 16.f); }
  if (s_dizzy > 0) { float r = 24.f * fminf(1.f, s_dizzy); tx = cosf(g_t * 30.f) * r; ty = sinf(g_t * 30.f) * r * 0.65f; }
  float gk = clampf(dt * (s_dizzy > 0 ? 25.f : 10.f), 0, 1);
  s_gzX += (tx - s_gzX) * gk; s_gzY += (ty - s_gzY) * gk;

  // lids: 0 = open. blink, flinch squeeze, pain squint, startle wide
  s_blinkTE -= dt;
  if (s_blinkTE < 0) { s_blinkE = 1.f; s_blinkTE = 2.5f + (esp_random() % 3000) / 1000.f; }
  s_blinkE = fmaxf(0.f, s_blinkE - dt * (s_eyeStyle == 1 && s_catSlow ? 1.6f : 6.5f));
  float blink = s_blinkE > 0.5f ? (1.f - s_blinkE) * 2.f : s_blinkE * 2.f;
  float squeeze = fmaxf(blink, s_flinch > 0.55f ? 1.f : s_flinch * 1.3f);
  float cU = clampf(0.08f + s_pain * 0.22f + squeeze * 0.5f - s_startle * 0.14f + sinf(g_t * 40.f) * 0.02f * s_pain, -0.1f, 0.52f);
  float cL = clampf(0.04f + s_pain * 0.16f + squeeze * 0.5f - s_startle * 0.06f, -0.05f, 0.5f);

  float skH = 285.f + s_pain * 45.f + sinf(g_t * 0.3f) * 12.f;
  uint16_t skin = hsv565(skH, 0.42f + s_pain * 0.25f, 0.62f);
  uint16_t skinS = hsv565(skH, 0.55f + s_pain * 0.2f, 0.36f);
  uint16_t skinD = hsv565(skH, 0.6f, 0.16f);
  uint16_t crease = hsv565(skH, 0.65f, 0.28f);
  uint16_t scl = rgb565(236, (uint8_t)(234 - s_pain * 70.f), (uint8_t)(228 - s_pain * 90.f));

  // socket + sclera
  canvas.fillEllipse((int)cx, (int)cy, ERX + 11, ERY + 11, skinD);
  canvas.fillEllipse((int)cx, (int)cy, ERX + 9, ERY + 9, skin);
  canvas.fillEllipse((int)cx, (int)cy, ERX, ERY, scl);
  // bloodshot veins
  int nv = (int)(s_pain * 10.f);
  for (int v = 0; v < nv; v++) {
    float a = v * 2.39996f, r = 1.f;
    float x0 = cx + cosf(a) * ERX * r, y0 = cy + sinf(a) * ERY * r;
    for (int s = 0; s < 4; s++) {
      float nr = r - 0.14f;
      float x1 = cx + cosf(a + sinf(v * 7.f + s * 3.f) * 0.18f) * ERX * nr, y1 = cy + sinf(a + cosf(v * 5.f + s) * 0.18f) * ERY * nr;
      canvas.drawLine((int)x0, (int)y0, (int)x1, (int)y1, rgb565(200, 30, 40));
      x0 = x1; y0 = y1; r = nr;
    }
  }
  s_ecx = cx; s_ecy = cy;
  if (s_eyeStyle != 0) eyeIrisStyled(cx + s_gzX, cy + s_gzY);
  else {
  // iris with striations, pupil dilates with sound, pinpoints with pain
  float ix = cx + s_gzX, iy = cy + s_gzY;
  int ir = (int)(25 + g_peak * 4.f - s_pain * 3.f);
  uint16_t irisC = hsv565(s_irisHue + g_level * 40.f, 0.85f, 0.65f + g_level * 0.25f);
  canvas.fillCircle((int)ix, (int)iy, ir + 1, hsv565(s_irisHue, 0.9f, 0.22f));
  canvas.fillCircle((int)ix, (int)iy, ir - 1, irisC);
  for (int k = 0; k < 20; k++) {
    float a = k * 0.314f + g_t * 0.2f;
    int r0 = 8, r1 = ir - 2;
    canvas.drawLine((int)(ix + cosf(a) * r0), (int)(iy + sinf(a) * r0), (int)(ix + cosf(a) * r1), (int)(iy + sinf(a) * r1),
                    hsv565(s_irisHue + (k & 1 ? 25.f : -20.f), 0.8f, (k & 1) ? 0.95f : 0.4f));
  }
  canvas.drawCircle((int)ix, (int)iy, ir / 2 + 2, hsv565(s_irisHue + 60.f, 0.5f, 0.9f));
  int pr = (int)(9 + g_level * 7.f + aud::bass * 3.f - s_pain * 5.f - s_startle * 3.f);
  if (pr < 2) pr = 2;
  canvas.fillCircle((int)(ix + s_gzX * 0.15f), (int)(iy + s_gzY * 0.15f), pr, rgb565(6, 4, 10));
  canvas.fillCircle((int)ix - 8, (int)iy - 9, 4, rgb565(255, 255, 255));
  canvas.fillCircle((int)ix + 7, (int)iy + 6, 2, rgb565(230, 240, 255));
  }

  // eyelids: arcs that follow the eyeball's curvature; angry slant after pokes
  int lashC = rgb565(15, 8, 20);
  for (int x = -ERX - 9; x <= ERX + 9; x++) {
    float u = (float)x / ERX;
    float e = u * u < 1.f ? sqrtf(1.f - u * u) : 0.f;
    float us = (float)x / (ERX + 9);
    float soc = us * us < 1.f ? (ERY + 9) * sqrtf(1.f - us * us) : 0.f;
    float slant = s_angry * (x < 0 ? -x : x) * -0.12f;           // inner corners drop
    float yu = cy - ERY * (1.f - 2.f * cU) * e + slant * (1.f - e) + s_angry * 6.f * e;
    float yl = cy + ERY * (1.f - 2.f * cL) * e;
    if (yu > yl) { float m = (yu + yl) * 0.5f; yu = yl = m; }
    int X = (int)cx + x;
    int top = (int)(cy - soc), bot = (int)(cy + soc);
    if (yu > top) {
      int sh = (int)yu - top < 7 ? (int)yu - top : 7;           // lid rolls into shadow at the edge
      canvas.drawFastVLine(X, top, (int)yu - top - sh, skin);
      canvas.drawFastVLine(X, (int)yu - sh, sh, skinS);
      int cr = (int)(yu - 7.f - e * 5.f * (1.f - cU));
      if (e > 0 && cr > top + 1 && cU < 0.4f) canvas.drawPixel(X, cr, crease);
    }
    if (bot > yl) {
      canvas.drawFastVLine(X, (int)yl, bot - (int)yl + 1, skin);
      canvas.drawFastVLine(X, (int)yl, 3, skinS);
    }
    if (e > 0) {
      canvas.drawFastVLine(X, (int)yu - 1, 3, lashC);
      canvas.drawPixel(X, (int)yl, rgb565(200, 90, 110));
      if ((x & 7) == 0 && cU < 0.45f) {
        float lx = u * 5.f;
        canvas.drawLine(X, (int)yu - 1, X + (int)lx, (int)yu - 6 - (int)(e * 3.f), lashC);
      }
    }
  }

  // tears: squirt from the corners, arc, splat, dribble
  if (s_pain > 0.4f && (esp_random() % 100) < (int)(s_pain * 30.f)) {
    float u = ((int)(esp_random() % 160) - 80) / 100.f;
    spawnTear(cx + u * ERX, cy + ERY * 0.75f * sqrtf(fmaxf(0.f, 1.f - u * u)), 0, 10.f, 1);
  }
  for (auto &t : s_tears) {
    if (t.life <= 0) continue;
    float gdrop = t.kind == 1 ? 140.f : 520.f;
    t.vy += gdrop * dt; t.x += t.vx * dt; t.y += t.vy * dt;
    t.life -= dt * (t.kind == 2 ? 2.5f : 0.35f);
    if (t.y > H - 16 && t.kind != 2) {
      for (int k = 0; k < 3; k++) spawnTear(t.x, H - 17.f, ((int)(esp_random() % 120) - 60), -60.f - (esp_random() % 80), 2);
      t.life = 0; continue;
    }
    uint16_t tc = rgb565(120, 200, 255);
    int r = t.kind == 2 ? 1 : (t.kind == 0 ? 4 : 3);
    float tl = t.kind == 0 ? 0.045f : 0.08f;
    for (int k = -1; k <= 1; k++)
      canvas.drawLine((int)t.x + k, (int)t.y, (int)(t.x - t.vx * tl), (int)(t.y - t.vy * tl), rgb565(90, 160, 230));
    canvas.fillCircle((int)t.x, (int)t.y, r, tc);
    canvas.drawPixel((int)t.x - 1, (int)t.y - 1, rgb565(255, 255, 255));
  }
  if (s_eyeStyle) eyeStyleFx(dt);
  if (millis() - s_pokeAt < 260) {
    for (int k = 0; k < 8; k++) {
      float a = k * 0.785f; int r0 = 8, r1 = 16 + (int)(s_pain * 8);
      canvas.drawLine(s_pokeX + (int)(cosf(a) * r0), s_pokeY + (int)(sinf(a) * r0), s_pokeX + (int)(cosf(a) * r1),
                      s_pokeY + (int)(sinf(a) * r1), rgb565(255, 230, 80));
    }
  }
}

// ============================================================
//  TUNNEL — dive / recede (audio-sliced, bendable), fractal, portal
// ============================================================
struct Slice { uint8_t b[16]; int8_t ox, oy; uint8_t en; };
static Slice s_sl[256];
static float s_tz = 0, s_camX = 0, s_camY = 0, s_bendX = 0, s_bendY = 0, s_scrape = 0;
static int s_zi = 0;
static int16_t s_offX[256], s_offY[256];
static uint8_t s_fog[256], s_twist[256];

static inline float pathX(float z) { return sinf(z * 0.021f) * 0.95f + sinf(z * 0.047f + 1.3f) * 0.5f; }
static inline float pathY(float z) { return cosf(z * 0.017f) * 0.55f + sinf(z * 0.039f) * 0.4f; }
static void fillSlice(Slice &s, float ox, float oy) {
  for (int i = 0; i < 16; i++) {
    float v = (aud::bands[i * 2] + aud::bands[i * 2 + 1]) * 0.5f;
    s.b[i] = (uint8_t)clampf(40.f + v * 200.f, 0, 255);
  }
  s.en = (uint8_t)clampf(aud::onset * 255.f + g_level * 60.f, 0, 255);   // beats become rings you fly through
  s.ox = (int8_t)clampf(ox * 50.f, -127, 127);
  s.oy = (int8_t)clampf(oy * 50.f, -127, 127);
}

static void tunnelRender(bool dive) {
  float dt = g_dt;
  float speed = 16.f + g_level * 45.f + aud::bass * 30.f;
  auto td = M5.Touch.getDetail();
  static int ptx = -1, pty = -1;
  float dragX = 0, dragY = 0;
  if (td.isPressed() && td.y > 14 && td.y < H - 14) {
    if (ptx >= 0) { dragX = (td.x - ptx) * 0.02f; dragY = (td.y - pty) * 0.02f; }
    ptx = td.x; pty = td.y;
  } else ptx = -1;

  if (dive) {
    s_tz += speed * dt;
    int zi = (int)s_tz;
    while (s_zi < zi) {
      s_zi++;
      float z = (float)(s_zi + 255);
      fillSlice(s_sl[(s_zi + 255) & 255], pathX(z) + aud::bass * 0.25f * sinf(z * 0.3f), pathY(z));
    }
    // steer: tilt (and drag) moves the camera inside the tube
    s_camX += (g_flyX * 1.9f + dragX * 8.f) * dt * (1.f + g_level * 0.5f);
    s_camY += (g_flyY * 1.6f + dragY * 8.f) * dt * (1.f + g_level * 0.5f);
    float nx = s_sl[(s_zi + 6) & 255].ox / 50.f, ny = s_sl[(s_zi + 6) & 255].oy / 50.f;

    float ex = s_camX - nx, ey = s_camY - ny, er = sqrtf(ex * ex + ey * ey);
    if (er > 0.62f) {
      s_camX = nx + ex / er * 0.6f; s_camY = ny + ey / er * 0.6f;
      if (s_scrape < 0.3f) hap(170, 25);
      s_scrape = 1.f;
    }
  } else {
    s_tz -= speed * dt;
    int zi = (int)floorf(s_tz);
    s_bendX += (g_flyX * 1.8f + dragX * 10.f + sinf(g_t * 3.f) * aud::bass * 0.8f) * dt;
    s_bendY += (g_flyY * 1.5f + dragY * 10.f + cosf(g_t * 2.3f) * aud::mid * 0.6f) * dt;
    s_bendX = clampf(s_bendX, -2.4f, 2.4f); s_bendY = clampf(s_bendY, -2.4f, 2.4f);
    while (s_zi > zi) { s_zi--; fillSlice(s_sl[(s_zi + 2) & 255], s_bendX, s_bendY); }
    s_camX += (s_bendX - s_camX) * clampf(dt * 8.f, 0, 1);
    s_camY += (s_bendY - s_camY) * clampf(dt * 8.f, 0, 1);
  }
  s_scrape = fmaxf(0.f, s_scrape - dt * 3.f);
  if (s_scrape > 0.05f) hapRumble(0.3f + s_scrape * 0.5f, 26.f, 0.9f);

  // per-depth tables
  float spin = g_t * (dive ? 20.f : -14.f) + g_gz * 0.05f;
  for (int d = 0; d < 256; d++) {
    const Slice &s = s_sl[(s_zi + d) & 255];
    float R = 900.f / (d + 0.6f);
    s_offX[d] = (int16_t)clampf((s.ox / 50.f - s_camX) * R, -150.f, 150.f);
    s_offY[d] = (int16_t)clampf((s.oy / 50.f - s_camY) * R, -110.f, 110.f);
    float fog = d < 5 ? d / 5.f : 1.f - (d - 5) / 250.f;
    s_fog[d] = (uint8_t)(clampf(fog, 0, 1) * 255.f);
    s_twist[d] = (uint8_t)(spin + d * (0.3f + aud::mid * 1.5f));
  }
  int cx = 80, cy = 60;
  uint8_t ph = (uint8_t)(g_t * 60.f);
  uint8_t ringPh = (uint8_t)(s_tz * 4.f);
  auto rows_ = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
    uint8_t *row = fx::buf + y * fx::LW;
    for (int x = 0; x < fx::LW; x++) {
      int d1 = fx::tunAt(x - cx, y - cy) & 255;
      uint16_t t = fx::tunAt(x - cx - s_offX[d1], y - cy - s_offY[d1]);
      int d = t & 255;
      int a = (t >> 8) + s_twist[d];
      const Slice &s = s_sl[(s_zi + d) & 255];
      int sec = (a >> 3) & 31;
      int bv = s.b[sec < 16 ? sec : 31 - sec];
      int wz = s_zi + d;
      int stripe = fx::sn[(uint8_t)(a * 4 + d * 3 + ph)];
      int xr = ((a << 1) ^ (wz << 2)) & 127;                     // old-school XOR texture, flowing in world space
      int rim = ((wz & 7) == 0) ? 40 : 0;
      int v = (xr >> 1) + ((bv * (128 + stripe)) >> 8) + (s.en >> 1);
      v += rim + (fx::sn[(uint8_t)(d * 8 - ringPh)] > 100 ? 24 : 0);
      v = (v * s_fog[d]) >> 8;
      row[x] = (uint8_t)(v > 255 ? 255 : v);
    }
  }
  };
  fx::parallel(rows_);
  float h = g_hue / 360.f;
  if (dive) fx::palCosine(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 0.0f + h, 0.33f + h + aud::centroid * 0.3f, 0.67f + h, g_t * 0.05f);
  else fx::palCosine(0.5f, 0.4f, 0.6f, 0.5f, 0.45f, 0.4f, 1.f, 0.8f, 0.6f, 0.8f + h, 0.2f + h, 0.5f + h, -g_t * 0.04f);
  fx::palOpal(0.22f + g_level * 0.15f, 0.5f + g_lookX * 0.35f + g_lookY * 0.25f + g_t * 0.03f, g_t * 0.05f, 0.35f + aud::onset * 0.5f);
  fx::palFlash(s_scrape * 0.7f + aud::onset * 0.25f);
  fx::present(canvas);
  {  // geometric wireframe glitching through the ether, riding the tube's bend
    float vpX = 160.f + s_offX[40] * 2.f, vpY = 120.f + s_offY[40] * 2.f;
    float gl = 0.06f + aud::onset * 0.9f + g_level * 0.15f + s_scrape * 0.6f;
    wire::tunnelRings(vpX, vpY, s_tz * 0.07f, 0.25f * sinf(g_t * 0.4f) + aud::mid * 0.3f, g_level + aud::bass * 0.5f, gl, !dive);
    if (aud::onset > 0.5f)                                        // on hits: a crystal phases through
      wire::tesseract(vpX, vpY, 16.f + aud::bass * 22.f, g_t * 1.3f, g_t * 0.9f, g_t * 0.5f, 0.5f, wire::LIME);
  }

  if (dive) {    // reticle shows where the tube heads next
    int rx = 160 + (int)(s_offX[40] * 2), ry = 120 + (int)(s_offY[40] * 2);
    canvas.drawCircle(160, 120, 9, rgb565(255, 255, 200));
    canvas.drawLine(rx - 4, ry, rx + 4, ry, hsv565(g_hue + 180.f, 0.6f, 1.f));
    canvas.drawLine(rx, ry - 4, rx, ry + 4, hsv565(g_hue + 180.f, 0.6f, 1.f));
  }
}

// ---- fractal: morphing Julia, adaptive quality, orbit-trap colouring (no flat fields) ----
static float s_fx = 0, s_fy = 0, s_fs = 3.2f / 160.f, s_frot = 0, s_fzoomDir = -1.f;
static float s_reborn = 0, s_empty = 0;          // dissolve progress, time spent in a void
static int s_fIt = 22, s_preset = 0;
static float s_cTr = 1.f;
static float s_cr0 = -0.8f, s_ci0 = 0.156f, s_crP = -0.8f, s_ciP = 0.156f;
static const float PRESETS[][2] = {{-0.8f, 0.156f}, {0.285f, 0.01f}, {-0.4f, 0.6f}, {-0.70176f, -0.3842f},
                                   {0.355f, 0.355f}, {-0.835f, -0.2321f}, {-0.1f, 0.651f}, {0.37f, -0.1f}};
static void fractalRender() {
  float dt = g_dt;
  uint32_t t0 = micros();
  auto td = M5.Touch.getDetail();
  static int ptx = -1, pty = -1, dragAcc = 0;
  static uint32_t downAt = 0;
  if (td.isPressed() && td.y > 14 && td.y < H - 14) {
    if (ptx < 0) { downAt = millis(); dragAcc = 0; }
    else {
      int dx = td.x - ptx, dy = td.y - pty; dragAcc += abs(dx) + abs(dy);
      float c = cosf(s_frot), s = sinf(s_frot);
      s_fx -= (c * dx - s * dy) * s_fs * 0.5f; s_fy -= (s * dx + c * dy) * s_fs * 0.5f;
    }
    ptx = td.x; pty = td.y;
  } else {
    if (ptx >= 0 && dragAcc < 10 && millis() - downAt < 350) {       // tap = new world
      s_preset = (s_preset + 1) % 8; s_crP = s_cr0; s_ciP = s_ci0; s_cTr = 0; hap(120, 40);
    }
    ptx = -1;
  }
  s_cTr = fminf(1.f, s_cTr + dt * 0.8f);
  float tr = s_cTr * s_cTr * (3.f - 2.f * s_cTr);
  s_cr0 = s_crP + (PRESETS[s_preset][0] - s_crP) * tr;
  s_ci0 = s_ciP + (PRESETS[s_preset][1] - s_ciP) * tr;
  float cr = s_cr0 + 0.006f * cosf(g_t * 0.31f), ci = s_ci0 + 0.006f * sinf(g_t * 0.23f);   // near-steady: keeps the self-similarity exact

  // ======== a TRUE fractal dive ========
  // The Julia set of z^2+c has a repelling fixed point z* = (1 + sqrt(1-4c))/2 with multiplier L = f'(z*) = 2 z*.
  // Near z* the set is exactly self-similar: scaled by |L| and rotated by arg(L) it maps onto itself, and every
  // escape count shifts by one. So: zoom into z* by one factor of |L| while rotating by -arg(L), then wrap the
  // camera back (offset from z* multiplied by L) and shift the colours one step. The picture continues exactly:
  // an infinite dive that keeps revealing the same structure inside itself, never running out of float precision.
  // Preimages of z* (the points that map onto it) are self-similar with the same L: the autopilot pulls you toward
  // whichever of them is nearest to where YOU are aiming.
  auto csq = [](float ar, float ai, float &rr, float &ri) {        // principal complex square root
    float m = sqrtf(ar * ar + ai * ai); rr = sqrtf(fmaxf(0.f, (m + ar) * 0.5f)); ri = copysignf(sqrtf(fmaxf(0.f, (m - ar) * 0.5f)), ai);
  };
  float wr, wi; csq(1.f - 4.f * cr, -4.f * ci, wr, wi);
  float zsr = (1.f + wr) * 0.5f, zsi = wi * 0.5f;
  if (zsr * zsr + zsi * zsi < 0.25f) { zsr = (1.f - wr) * 0.5f; zsi = -wi * 0.5f; }        // the repelling one (|2z*| > 1)
  float lr = 2.f * zsr, li = 2.f * zsi, lAbs = sqrtf(lr * lr + li * li), lArg = atan2f(li, lr);
  if (lAbs < 1.05f) { lAbs = 1.05f; }
  // self-similar targets: z*, -z*, and preimages (+-sqrt(p - c)), breadth-first
  float TX[15], TY[15]; int nT = 0;
  TX[nT] = zsr; TY[nT++] = zsi; TX[nT] = -zsr; TY[nT++] = -zsi;
  for (int q = 1; q < 7 && nT < 15; q++) {
    float pr, pi; csq(TX[q] - cr, TY[q] - ci, pr, pi);
    if (nT < 15) { TX[nT] = pr; TY[nT++] = pi; }
    if (nT < 15) { TX[nT] = -pr; TY[nT++] = -pi; }
  }
  static int tg = 0; static float s_du = 0; static int s_colOff = 0, s_wraps = 0;
  const float S0 = 2.2f / 160.f;
  // steer with tilt / drag (the autopilot only helps, it doesn't fight you)
  float c = cosf(s_frot), s = sinf(s_frot);
  s_fx += (c * g_flyX - s * g_flyY) * s_fs * 140.f * dt;
  s_fy += (s * g_flyX + c * g_flyY) * s_fs * 140.f * dt;
  bool piloting = fabsf(g_flyX) + fabsf(g_flyY) > 0.06f || ptx >= 0;
  {  // target = the self-similar point nearest to where you're aiming (with hysteresis)
    float bd = 1e9f; int bi = tg;
    for (int k = 0; k < nT; k++) { float dx = TX[k] - s_fx, dy = TY[k] - s_fy, d = dx * dx + dy * dy; if (d < bd) { bd = d; bi = k; } }
    float cdx = TX[tg % nT] - s_fx, cdy = TY[tg % nT] - s_fy;
    if (bi != tg && bd < 0.36f * (cdx * cdx + cdy * cdy)) tg = bi;
    tg %= nT;
  }
  float tx0 = TX[tg], ty0 = TY[tg];
  float pull = clampf(dt * (piloting ? 0.35f : 1.8f), 0.f, 1.f);              // autopilot: into the detail
  s_fx += (tx0 - s_fx) * pull; s_fy += (ty0 - s_fy) * pull;
  // dive: continuous zoom by |L| per level, turning with it
  s_du += dt * (0.14f + g_level * 0.3f + aud::bass * 0.15f);
  s_frot += (g_gz * 0.004f + 0.03f) * dt;
  if (s_du >= 1.f) {
    float dxr = s_fx - tx0, dxi = s_fy - ty0, view = S0 * 160.f;
    if (dxr * dxr + dxi * dxi < view * view) {                                // wrap: same picture, one level shallower
      s_du -= 1.f; s_colOff += 9; s_wraps++;
      s_fx = tx0 + (dxr * lr - dxi * li); s_fy = ty0 + (dxr * li + dxi * lr); // offset from the target scales by L
      if (s_wraps % 10 == 0) { s_preset = (s_preset + 1 + (esp_random() % 3)) % 8; s_crP = s_cr0; s_ciP = s_ci0; s_cTr = 0; }   // now and then, drift to another world
    }
  }
  s_fs = S0 * powf(lAbs, -s_du);
  float rotNow = s_frot - lArg * s_du;
  if (s_fs < 3e-6f) {                                                          // steered far off the path: re-enter the dive
    memcpy(fx::back, fx::buf, fx::LW * fx::LH);
    s_du = 0; s_fx = tx0; s_fy = ty0; s_reborn = 1.f; hapGesture(HG_REBIRTH);
    s_fs = S0;
  }
  c = cosf(rotNow); s = sinf(rotNow);
  float sc = s_fs * (1.f - aud::bass * 0.08f);

  float ux = c * sc, uy = s * sc, vx = -s * sc, vy = c * sc;
  float ox = s_fx - ux * 80.f - vx * 60.f, oy = s_fy - uy * 80.f - vy * 60.f;
  const int maxIt = s_fIt;
  int ph = (int)(g_t * 40.f) + s_colOff;
  auto rows_ = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
    uint8_t *row = fx::buf + y * fx::LW;
    float zr0 = ox + vx * y, zi0 = oy + vy * y;
    for (int x = 0; x < fx::LW; x++) {
      float zr = zr0 + ux * x, zi = zi0 + uy * x;
      float trap = 10.f, trap2 = 100.f;            // cross trap (outside), point trap (inside)
      int k = 0;
      for (; k < maxIt; k++) {
        float r2 = zr * zr, i2 = zi * zi;
        if (r2 + i2 > 16.f) break;
        zi = 2.f * zr * zi + ci; zr = r2 - i2 + cr;
        float ax = fabsf(zr), ay = fabsf(zi);
        float tx = ax < ay ? ax : ay; if (tx < trap) trap = tx;
        float tp = (zr - 0.15f) * (zr - 0.15f) + zi * zi; if (tp < trap2) trap2 = tp;
      }
      int v;
      if (k < maxIt) {
        float m = zr * zr + zi * zi;
        int fine = (int)(clampf(1.f - (m - 16.f) / (m + 16.f), 0.f, 1.f) * 12.f);
        v = 24 + ((k * 9 + fine + (int)(sqrtf(trap) * 110.f) + ph) & 127);
      } else v = 152 + (((int)(sqrtf(trap2) * 240.f) + (int)(trap * 40.f) - ph * 2) & 103);
      row[x] = (uint8_t)v;
    }
  }
  };
  fx::parallel(rows_);
  if (s_reborn > 0) {                                  // dither-dissolve: the old view keeps falling inward
    s_reborn = fmaxf(0.f, s_reborn - dt * 0.9f);
    float zf = 1.f + (1.f - s_reborn) * 1.5f;
    int thr = (int)(s_reborn * 255.f);
    for (int y = 0; y < fx::LH; y++)
      for (int x = 0; x < fx::LW; x++) {
        uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u); hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
        if ((int)(hsh & 255) < thr) {
          int sx = 80 + (int)((x - 80) / zf), sy = 60 + (int)((y - 60) / zf);
          fx::buf[y * fx::LW + x] = fx::back[sy * fx::LW + sx];
        }
      }
  }
  float h = g_hue / 360.f + aud::centroid * 0.4f;
  fx::palCosine(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 2.f, 1.f, 1.f + g_level, 0.5f + h, 0.2f + h, 0.25f + h, g_t * 0.03f);
  fx::palOpal(0.25f, 0.5f + g_lookX * 0.35f + g_lookY * 0.25f, g_t * 0.04f, 0.3f + aud::onset * 0.6f);
  fx::palFlash(aud::onset * 0.3f + s_reborn * 0.2f);
  fx::present(canvas);
  uint32_t took = micros() - t0;                       // adaptive quality, demo-style
  if (took > 21000 && s_fIt > 12) s_fIt--;
  else if (took < 14000 && s_fIt < 40) s_fIt++;
}

// ---- portal: fly the ether; thread 3 hoops to summon a portal; fly INTO the next dimension ----
struct Star { float x, y, z, pz; };
static Star s_st[200];
static float s_pcx = 0, s_pcy = 0, s_emerge = 0;
static int s_dim = 0;
static float s_dimP[12], s_nextP[12];
static float s_hx = 0, s_hy = 0, s_hz = 0, s_hoopFlash = 0;   // the current hoop (world x/y, depth)
static bool s_hoopAlive = false;
static int s_hoopN = 0;                                          // consecutive hoops threaded (0..3)
static uint32_t s_hoopNext = 0;
static float s_ptx = 0, s_pty = 0, s_ptz = 0;                  // the portal
static bool s_portal = false;
static uint16_t s_palN[256];                                     // next dimension's palette (byte-swapped 565)
static inline uint16_t mix565c(uint16_t a, uint16_t b, float u) {
  u = clampf(u, 0.f, 1.f); int ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31, rb = b >> 11, gb = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((int)(ra + (rb - ra) * u) << 11) | ((int)(ga + (gb - ga) * u) << 5) | (int)(ba + (bb - ba) * u));
}
// ---- stellar landmarks flown past between gates ----
enum BodyT : uint8_t { BD_STAR = 0, BD_PLANET, BD_GIANT, BD_DWARF, BD_NEUTRON, BD_HOLE };
struct Body { float x, y, z, r, seed; uint8_t type; bool live; };
static Body s_body = {0, 0, 0, 0, 0, 0, false};
static uint32_t s_bodyNext = 4000;
static float s_lensX = -999, s_lensY = -999, s_lensR = 0;          // black-hole lens in lores coords (for the warp + gates)
static inline void lensShift(float &x, float &y) {                 // screen-space gate positions bend around a black hole
  if (s_lensR <= 0) return;
  float dx = x * 0.5f - s_lensX, dy = y * 0.5f - s_lensY, d2 = dx * dx + dy * dy + 1.f;
  float k = s_lensR * s_lensR * 1.6f / d2; if (k > 3.f) k = 3.f;
  x += dx * k * 2.f; y += dy * k * 2.f;
}
static void drawBody(const Body &b, float pcx, float pcy, float alien) {
  float z = fmaxf(b.z, 0.2f);
  float px = 160.f + (b.x - pcx) / z * 60.f, py = 120.f + (b.y - pcy) / z * 60.f, pr = b.r / z * 60.f;
  if (pr > 420.f || px < -pr - 40 || px > W + pr + 40) return;
  int X = (int)px, Y = (int)py, R = (int)fmaxf(1.f, pr);
  uint32_t sd = (uint32_t)(b.seed * 1e6f);
  switch (b.type) {
    case BD_STAR: {                                              // a sun: glow, disc, corona spikes
      float temp = (sd % 100) / 100.f;
      uint16_t core = temp < 0.33f ? rgb565(255, 190, 110) : (temp < 0.66f ? rgb565(255, 245, 200) : rgb565(190, 220, 255));
      for (int k = 4; k >= 1; k--) canvas.fillCircle(X, Y, R + k * R / 3, mix565c(rgb565(0, 0, 0), core, 0.12f * (5 - k)));
      canvas.fillCircle(X, Y, R, core);
      for (int k = 0; k < 8; k++) { float a = k * 0.785f + g_t * 0.05f; canvas.drawLine(X, Y, X + (int)(cosf(a) * R * 2.4f), Y + (int)(sinf(a) * R * 2.4f), mix565c(core, 0, 0.5f)); }
      break;
    }
    case BD_PLANET: case BD_GIANT: {                             // shaded sphere, bands on giants, sometimes rings
      float hue = (sd % 360) + alien * 120.f;
      uint16_t base = hsv565(hue, 0.35f + alien * 0.4f, 0.55f), dark = hsv565(hue, 0.5f, 0.12f);
      bool rings = (sd >> 9) % 3 == 0;
      if (rings) canvas.drawEllipse(X, Y, (int)(R * 2.1f), (int)(R * 0.5f), hsv565(hue + 30.f, 0.3f, 0.6f));
      canvas.fillCircle(X, Y, R, base);
      if (b.type == BD_GIANT) for (int k = -3; k <= 3; k++) { int yy = Y + k * R / 4; float w = sqrtf(fmaxf(0.f, 1.f - (float)(k * k) / 16.f)) * R; canvas.drawFastHLine(X - (int)w, yy, (int)(w * 2), hsv565(hue + k * 12.f, 0.45f, 0.4f + (k & 1) * 0.15f)); }
      canvas.fillCircle(X + R / 3, Y + R / 4, (int)(R * 0.95f), dark);                 // night side
      canvas.fillCircle(X - R / 4, Y - R / 4, (int)(R * 0.75f), base);
      if (rings) canvas.drawEllipse(X, Y, (int)(R * 2.1f), (int)(R * 0.5f), hsv565(hue + 30.f, 0.3f, 0.75f));
      break;
    }
    case BD_DWARF: {                                             // tiny, fierce, blue-white glare
      canvas.fillCircle(X, Y, R + 2, rgb565(200, 230, 255));
      for (int k = 0; k < 4; k++) { float a = k * 1.5708f; canvas.drawLine(X, Y, X + (int)(cosf(a) * (R + 30)), Y + (int)(sinf(a) * (R + 30)), rgb565(170, 210, 255)); }
      break;
    }
    case BD_NEUTRON: {                                           // pulsar: two sweeping beams
      float a = g_t * 6.f;
      for (int s = -1; s <= 1; s += 2) for (int k = -1; k <= 1; k++) canvas.drawLine(X, Y, X + (int)(cosf(a + k * 0.03f) * 260.f * s), Y + (int)(sinf(a + k * 0.03f) * 260.f * s), k ? rgb565(80, 140, 200) : rgb565(200, 240, 255));
      canvas.fillCircle(X, Y, R + 2, rgb565(230, 245, 255));
      break;
    }
    default: {                                                   // black hole: accretion disk, photon ring, darkness
      canvas.drawEllipse(X, Y, (int)(R * 3.2f), (int)(R * 0.9f), rgb565(255, 150, 60));
      canvas.drawEllipse(X, Y, (int)(R * 2.6f), (int)(R * 0.7f), wire::PLUM);
      canvas.fillCircle(X, Y, R + 3, rgb565(255, 220, 160));
      canvas.fillCircle(X, Y, R, rgb565(0, 0, 0));
      canvas.drawEllipse(X, Y - R, (int)(R * 1.4f), (int)(R * 0.5f), rgb565(255, 190, 110));   // disk light bent over the top
      break;
    }
  }
}
static void rollDim(float *p) { for (int i = 0; i < 12; i++) p[i] = (esp_random() % 1000) / 1000.f; }
static inline float alienOf(int dim) { return clampf((dim - 1) / 8.f, 0.f, 1.f); }   // 0 = real space .. 1 = witch space
static void dimPalette(const float *p, float t, int set, float alien) {
  // real space: black sky, faint dusty nebula (blue / violet / rust), white-blue stars.
  // deeper: more saturation, stranger hue pairs, mantis colours, then outright alien skies.
  float hA = (p[0] < 0.5f ? 220.f : 280.f) + (p[0] - 0.5f) * 60.f + alien * (p[1] * 300.f) + t * 6.f * alien;
  float hB = hA + 30.f + alien * (60.f + p[2] * 180.f);
  float sat = 0.25f + alien * 0.65f, deep = 0.02f + alien * 0.08f;
  for (int i = 0; i < 256; i++) {
    float u = i / 255.f; uint16_t c;
    if (u < 0.3f) c = hsv565(hA, sat, deep + u * (0.35f + alien * 0.9f));                       // sky + nebula
    else if (u < 0.75f) c = hsv565(hA + (hB - hA) * (u - 0.3f) / 0.45f, sat * (1.f - (u - 0.3f) * 0.8f), 0.12f + u * (0.55f + alien * 0.35f));
    else c = hsv565(alien > 0.5f ? hB : 210.f, (1.f - u) * (0.6f + alien), 0.75f + 0.25f * u);    // stars: white-blue, alien-tinted deeper
    if (alien > 0.6f && i > 40 && i < 120) c = mix565c(c, (i & 16) ? wire::LIME : wire::PLUM, (alien - 0.6f) * 0.6f);   // witch space
    if (set == 0) fx::palSet(i, (uint8_t)(((c >> 11) & 31) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3));
    else s_palN[i] = (uint16_t)((c >> 8) | (c << 8));
  }
}
static inline int nebulaAt(const float *p, int x, int y, float t, int ox, int oy) {
  uint8_t t1 = (uint8_t)(t * (8.f + p[5] * 20.f)), t2 = (uint8_t)(t * 13.f), t3 = (uint8_t)(-t * 11.f);
  int f1 = 1 + (int)(p[6] * 3.f), f2 = 1 + (int)(p[7] * 3.f);
  return fx::sn[(uint8_t)((x + ox) * f1 + t1)] + fx::sn[(uint8_t)((y + oy) * f2 + t2)] + fx::sn[(uint8_t)((x + y + ox) * 2 + t3)];
}
static void portalRender() {
  float dt = g_dt;
  static bool init = false;
  if (!init) {
    init = true; rollDim(s_dimP); rollDim(s_nextP); s_dim = 1;
    for (auto &s : s_st) { s.x = ((int)(esp_random() % 2000) - 1000) / 1000.f; s.y = ((int)(esp_random() % 2000) - 1000) / 1000.f; s.z = s.pz = (esp_random() % 1000) / 1000.f + 0.05f; }
    s_hoopNext = millis() + 1500;
  }
  float speed = 0.45f + aud::bass * 1.3f + g_level * 0.6f + aud::onset * 0.8f;
  auto td = M5.Touch.getDetail();
  float steerX = g_flyX, steerY = g_flyY;
  static int pdx = -1, pdy = -1;
  static float s_svx = 0, s_svy = 0;                             // ship velocity (world units / s)
  if (td.isPressed() && td.y > 14 && td.y < H - 14) {
    steerX += (td.x - 160) / 80.f; steerY += (td.y - 120) / 60.f;   // hold: fly toward your finger
    if (pdx >= 0) { s_pcx -= (td.x - pdx) * 0.006f; s_pcy -= (td.y - pdy) * 0.006f; }   // drag: grab the space and pull it
    pdx = td.x; pdy = td.y;
  } else pdx = -1;
#ifdef PORTAL_AUTOPILOT
  { float ax_ = s_portal ? s_ptx - s_pcx : s_hx - s_pcx, ay_ = s_portal ? s_pty - s_pcy : s_hy - s_pcy; steerX = clampf(ax_ * 3.f, -1.f, 1.f); steerY = clampf(ay_ * 3.f, -1.f, 1.f); }
#endif
#ifdef PORTAL_TILTPILOT
  {   // test pilot: tilts the DEVICE toward what it sees, like a person would (goes through the real IMU path)
    extern m5_imu_data_t g_mockImu;
    float ox_ = s_portal ? s_ptx - s_pcx : s_hx - s_pcx, oy_ = s_portal ? s_pty - s_pcy : s_hy - s_pcy;
    float rx = clampf(ox_ * 1.2f, -TP_MAX, TP_MAX), ry = clampf(oy_ * 1.2f, -TP_MAX, TP_MAX);   // max tilt (radians)
    g_mockImu.accel = {-sinf(rx), sinf(ry), cosf(rx) * cosf(ry)};
  }
#endif
  s_svx += (clampf(steerX, -3.f, 3.f) * 0.75f - s_svx) * clampf(dt * 5.f, 0.f, 1.f);
  s_svy += (clampf(steerY, -3.f, 3.f) * 0.75f - s_svy) * clampf(dt * 5.f, 0.f, 1.f);
  s_pcx += s_svx * dt; s_pcy += s_svy * dt;
  if (s_hoopAlive && s_hz < 1.4f) {                              // close to a hoop and nearly lined up: a gentle nudge
    float ox = s_hx - s_pcx, oy = s_hy - s_pcy;
    if (ox * ox + oy * oy < 0.3f * 0.3f) { s_pcx += ox * dt * 1.2f; s_pcy += oy * dt * 1.2f; }
  }

  // ---- this dimension: feedback streaks + nebula + stars ----
  fx::swap();
  float alienNow = alienOf(s_dim);
  float swirl = (s_dimP[3] - 0.5f) * (0.01f + 0.07f * alienNow) + g_gz * 0.0004f;
  float zoom = 1.035f + aud::bass * 0.05f;
  float c = cosf(swirl), s = sinf(swirl);
  for (int j = 0; j < fx::GH; j++)
    for (int i = 0; i < fx::GW; i++) {
      float dx = i * 8.f - 80.f, dy = j * 8.f - 60.f;
      fx::gx[j][i] = 80.f + (c * dx + s * dy) / zoom; fx::gy[j][i] = 60.f + (-s * dx + c * dy) / zoom;
    }
  if (s_lensR > 0)                                                   // gravitational lensing warps the view
    for (int j = 0; j < fx::GH; j++)
      for (int i = 0; i < fx::GW; i++) {
        float dx = i * 8.f - s_lensX, dy = j * 8.f - s_lensY, d2 = dx * dx + dy * dy + 4.f;
        float k = s_lensR * s_lensR / d2; if (k > 0.9f) k = 0.9f;
        fx::gx[j][i] += dx * k; fx::gy[j][i] += dy * k;
      }
  fx::warp((uint8_t)(12 + s_dimP[4] * 10.f), false);
  {
    int ox = (int)(s_pcx * 40.f), oy = (int)(s_pcy * 40.f), lift = (int)(14 + alienNow * 40.f) + (int)(aud::mid * 40.f);
    float t = g_t;
    auto rows_ = [&](int y0, int y1) {
      for (int y = y0; y < y1; y++) {
        uint8_t *row = fx::buf + y * fx::LW;
        for (int x = 0; x < fx::LW; x++) {
          int n = nebulaAt(s_dimP, x, y, t, ox, oy);
          n = n > 0 ? (n * lift) >> 8 : 0;
          if (n > row[x]) row[x] = (uint8_t)n;
        }
      }
    };
    fx::parallel(rows_);
  }
  float fov = 95.f + aud::bass * 30.f;
  for (auto &st : s_st) {
    st.pz = st.z;
    st.z -= speed * dt * (0.6f + s_dimP[6]);
    if (st.z < 0.03f) { st.x = ((int)(esp_random() % 2000) - 1000) / 1000.f; st.y = ((int)(esp_random() % 2000) - 1000) / 1000.f; st.z = st.pz = 1.f; continue; }
    float sx0 = 80.f + (st.x - s_pcx * 0.3f) / st.pz * fov * 0.5f, sy0 = 60.f + (st.y - s_pcy * 0.3f) / st.pz * fov * 0.5f;
    float sx = 80.f + (st.x - s_pcx * 0.3f) / st.z * fov * 0.5f, sy = 60.f + (st.y - s_pcy * 0.3f) / st.z * fov * 0.5f;
    uint8_t b = (uint8_t)clampf(90.f + 200.f * (1.f - st.z) + aud::treble * 60.f, 60, 255);
    fx::line((int)sx0, (int)sy0, (int)sx, (int)sy, b);
    if (st.z < 0.3f) fx::line((int)sx0 + 1, (int)sy0, (int)sx + 1, (int)sy, b);
  }
  dimPalette(s_dimP, g_t, 0, alienOf(s_dim));
  fx::palFlash(s_emerge * 0.6f + s_hoopFlash * 0.25f + aud::onset * 0.2f);
  s_emerge = fmaxf(0.f, s_emerge - dt * 1.2f);
  s_hoopFlash = fmaxf(0.f, s_hoopFlash - dt * 3.f);
  fx::present(canvas);

  // ---- stellar landmarks ----
  if (!s_body.live && millis() > s_bodyNext) {
    uint32_t r = esp_random() % 1000; float al = alienNow;
#ifdef FORCE_HOLE
    r = 999;
#endif
    uint8_t t = r < 330 ? BD_STAR : (r < 660 ? BD_PLANET : (r < 850 ? BD_GIANT : (r < 920 ? BD_DWARF : (r < 975 - (int)(al * 30) ? BD_NEUTRON : BD_HOLE))));
    static const float RW[6] = {0.22f, 0.3f, 0.75f, 0.05f, 0.04f, 0.25f};
    float side = (esp_random() & 1) ? 1.f : -1.f;
    s_body = {s_pcx + side * (0.9f + (esp_random() % 100) / 90.f), s_pcy + ((int)(esp_random() % 160) - 80) / 100.f, 7.f, RW[t], (esp_random() % 10000) / 10000.f, t, true};
  }
  s_lensR = 0;
  if (s_body.live) {
    s_body.z -= dt * (0.3f + speed * 0.25f);
    if (s_body.z < 0.25f) { s_body.live = false; s_bodyNext = millis() + 3000 + esp_random() % 6000; }
    else {
      drawBody(s_body, s_pcx, s_pcy, alienNow);
      if (s_body.type == BD_HOLE) {
        float z = s_body.z;
        s_lensX = 80.f + (s_body.x - s_pcx) / z * 30.f; s_lensY = 60.f + (s_body.y - s_pcy) / z * 30.f;
        s_lensR = fminf(60.f, s_body.r / z * 30.f * 2.2f);
      }
    }
  }

  // ---- hoops: thread three in a row ----
  float approach = dt * (0.42f + speed * 0.28f);
  if (!s_portal && !s_hoopAlive && millis() > s_hoopNext) {
    s_hoopAlive = true; s_hz = 3.2f;
    s_hx = s_pcx + ((int)(esp_random() % 140) - 70) / 100.f; s_hy = s_pcy + ((int)(esp_random() % 100) - 50) / 100.f;
  }
  if (s_hoopAlive) {
    s_hz -= approach;
    float ox = s_hx - s_pcx, oy = s_hy - s_pcy;
    if (s_hz < 0.18f) {
      bool in = ox * ox + oy * oy < 0.34f * 0.34f;
      if (in) { s_hoopN++; s_hoopFlash = 1.f; hapGesture(s_hoopN >= 3 ? HG_CHAIN : HG_THREAD); stats::event(stats::EV_HOOP); } else { s_hoopN = 0; hapGesture(HG_MISS); }
#if defined(PORTAL_AUTOPILOT) || defined(PORTAL_TILTPILOT)
      printf("t=%.1f hoop %s (off %.2f,%.2f) chain=%d\n", millis() / 1000.f, in ? "THREAD" : "miss", ox, oy, s_hoopN);
#endif
      s_hoopAlive = false; s_hoopNext = millis() + 900;
      if (s_hoopN >= 3) { s_portal = true; s_ptz = 4.2f; s_ptx = s_pcx + ((int)(esp_random() % 120) - 60) / 100.f; s_pty = s_pcy + ((int)(esp_random() % 80) - 40) / 100.f; }
    } else {
      float px = 160.f + ox / s_hz * 60.f, py = 120.f + oy / s_hz * 60.f, r = 20.f / s_hz;
      lensShift(px, py);
      float pulse = 1.f + aud::bass * 0.15f;
      uint16_t main = (s_hoopN == 2) ? rgb565(255, 230, 120) : wire::LIME;
      for (int k = 0; k < 3; k++) canvas.drawEllipse((int)px, (int)py, (int)(r * pulse) + k, (int)(r * pulse * 0.9f) + k, k == 0 ? wire::PLUM : (k == 1 ? main : wire::TEAL));
      for (int k = 0; k < 10; k++) {                                   // beads spin faster with the music
        float a = k * 0.6283f + g_t * (1.5f + g_level * 4.f);
        canvas.fillCircle((int)(px + cosf(a) * r * pulse), (int)(py + sinf(a) * r * pulse * 0.9f), r > 30 ? 2 : 1, main);
      }
    }
  }

  // ---- the portal: its inside is the next dimension; fly into it ----
  if (s_portal) {
    s_ptz -= dt * (0.30f + speed * 0.18f);
    float ox = s_ptx - s_pcx, oy = s_pty - s_pcy;
    if (s_ptz < 2.0f && ox * ox + oy * oy < 0.6f * 0.6f) { s_pcx += ox * dt * 1.2f; s_pcy += oy * dt * 1.2f; }   // aim assist once it's close
    float px = 80.f + ox / s_ptz * 30.f, py = 60.f + oy / s_ptz * 30.f;          // lores
    float r = 13.f / s_ptz * (1.f + aud::bass * 0.12f);
    bool aligned = ox * ox + oy * oy < 0.45f * 0.45f;
    {  // a massive disruption in space-time: you feel it before you reach it
      float u = clampf((4.2f - s_ptz) / 4.085f, 0.f, 1.f);
      float amt = (0.14f + 0.86f * powf(u, 2.4f)) * (0.9f + aud::bass * 0.25f);
      hapRumble(amt, 4.f + u * u * 20.f, 0.15f + u * 0.7f);
    }
    float t = g_t;
    uint8_t fly = (uint8_t)(t * 120.f), spin = (uint8_t)(t * 60.f);
    int lift = 40 + (int)(aud::mid * 40.f);
    auto inside = [&](int x, int y) -> int {                           // the next world, seen down its own tunnel
      float dx = x - px, dy = y - py;
      uint16_t tt = fx::tunAt((int)(dx * 40.f / fmaxf(r, 1.f)), (int)(dy * 40.f / fmaxf(r, 1.f)));
      int dd = tt & 255, aa = tt >> 8;
      int n = nebulaAt(s_nextP, x, y, t, 0, 0);
      n = (n > 0 ? (n * lift) >> 8 : 0) + ((fx::sn[(uint8_t)(dd * 5 - fly)] + fx::sn[(uint8_t)(aa * 3 + spin)]) >> 2) + 60;
      return n < 0 ? 0 : (n > 255 ? 255 : n);
    };
    if (s_ptz < 0.115f) {                                              // the portal now covers the whole view
      if (aligned) {                                                   // we're through: this IS the new world now
        for (int y = 0; y < fx::LH; y++) for (int x = 0; x < fx::LW; x++) fx::buf[y * fx::LW + x] = (uint8_t)inside(x, y);
        memcpy(s_dimP, s_nextP, sizeof(s_dimP)); rollDim(s_nextP); s_dim++;
        s_emerge = 0.f; s_hoopN = 0; s_portal = false; s_hoopNext = millis() + 2500;
        hapCut(350);                                                     // ...and then: silence. we're through.
        stats::event(stats::EV_PORTAL);
#if defined(PORTAL_AUTOPILOT) || defined(PORTAL_TILTPILOT)
        printf("t=%.1f THROUGH PORTAL -> dim %d\n", millis() / 1000.f, s_dim);
#endif
      } else { s_ptz = 4.2f; s_ptx = s_pcx + ((int)(esp_random() % 120) - 60) / 100.f; s_pty = s_pcy; hapGesture(HG_MISS); }
    } else {
      dimPalette(s_nextP, g_t, 1, alienOf(s_dim + 1));
      uint16_t *fb = (uint16_t *)canvas.getBuffer();
      int x0 = (int)fmaxf(0.f, px - r), x1 = (int)fminf(159.f, px + r), y0 = (int)fmaxf(0.f, py - r), y1 = (int)fminf(119.f, py + r);
      float r2 = r * r;
      for (int y = y0; y <= y1 && fb; y++)
        for (int x = x0; x <= x1; x++) {
          float dx = x - px, dy = y - py;
          if (dx * dx + dy * dy > r2) continue;
          uint16_t v = s_palN[inside(x, y)];
          fb[(y * 2) * W + x * 2] = v; fb[(y * 2) * W + x * 2 + 1] = v;
          fb[(y * 2 + 1) * W + x * 2] = v; fb[(y * 2 + 1) * W + x * 2 + 1] = v;
        }
      int R = (int)(r * 2.f), X = (int)(px * 2.f), Y = (int)(py * 2.f);       // crisp rim, full res
      for (int k = 0; k < 5; k++) canvas.drawCircle(X, Y, R + k, wire::PLUM);                                  // deep plum rim
      for (int k = 0; k < 3; k++) canvas.drawCircle(X, Y, R + 6 + k * 3, mix565c(rgb565(140, 200, 255), rgb565(0, 0, 0), k * 0.35f));   // light-blue glow
      int arcs = 24;
      for (int k = 0; k < arcs; k++) {                                                                        // teal plasma fringe
        float a0 = k * 6.2831853f / arcs + g_t * 1.7f, a1 = a0 + 0.2f;
        float j0 = 1.f + 0.05f * sinf(g_t * 13.f + k * 2.1f) * (1.f + g_level * 2.f), j1 = 1.f + 0.05f * sinf(g_t * 11.f + k * 1.3f);
        canvas.drawLine(X + (int)(cosf(a0) * R * j0), Y + (int)(sinf(a0) * R * j0), X + (int)(cosf(a1) * R * j1), Y + (int)(sinf(a1) * R * j1), wire::TEAL);
      }
      for (int k = 0; k < 10 + (int)(g_level * 12); k++) {                                                   // lime sparks spitting off the rim
        float a = (esp_random() % 628) / 100.f, rr = R * (1.02f + (esp_random() % 20) / 100.f);
        int sx = X + (int)(cosf(a) * rr), sy = Y + (int)(sinf(a) * rr);
        canvas.drawLine(sx, sy, sx + (int)(cosf(a) * 5), sy + (int)(sinf(a) * 5), wire::LIME);
      }
    }
  }

  // ---- HUD: aim point, hoop chain, dimension ----
  {
    int rx = 160 + (int)(s_svx * 22.f), ry = 120 + (int)(s_svy * 22.f);   // the ship leans where you steer
    canvas.drawLine(160, 120, rx, ry, wire::TEAL);
    canvas.drawCircle(rx, ry, 6, rgb565(255, 255, 220));
    canvas.drawCircle(160, 120, 2, wire::PLUM);
  }
  float tx = 0, ty = 0; bool target = false;
  if (s_portal) { tx = s_ptx - s_pcx; ty = s_pty - s_pcy; target = true; }
  else if (s_hoopAlive) { tx = s_hx - s_pcx; ty = s_hy - s_pcy; target = true; }
  if (target) { float d = sqrtf(tx * tx + ty * ty); if (d > 0.2f) canvas.fillCircle(160 + (int)(tx / d * 18.f), 120 + (int)(ty / d * 18.f), 2, wire::LIME); }
  for (int k = 0; k < 3; k++) {
    int cx = W / 2 - 16 + k * 16;
    if (k < s_hoopN || s_portal) canvas.fillCircle(cx, 22, 4, wire::LIME); else canvas.drawCircle(cx, 22, 4, wire::TEAL);
  }
  canvas.setTextSize(1); canvas.setTextColor(wire::TEAL); canvas.setCursor(W - 50, 18); canvas.printf("dim %d", s_dim);
}

static void modeTunnel() {
  switch (g_tunnelMode) {
    case TM_DIVE: tunnelRender(true); break;
    case TM_RECEDE: tunnelRender(false); break;
    case TM_FRACTAL: fractalRender(); break;
    default: portalRender(); break;
  }
}

// ============================================================
//  PULSE — audio feedback visualizers, increasingly cross-mapped
// ============================================================
struct Mote { float x, y, life; };
static Mote s_mote[64];
static float s_shock = -1;
static float s_palSeed[9] = {0.f, 0.33f, 0.67f, 1.f, 1.f, 1.f, 0.5f, 0.5f, 0.5f};

static void gridWarp(float cx, float cy, float zoom, float rot, float wobA, float wobF, float wobT) {
  float c = cosf(rot), s = sinf(rot);
  for (int j = 0; j < fx::GH; j++)
    for (int i = 0; i < fx::GW; i++) {
      float dx = i * 8.f - cx, dy = j * 8.f - cy;
      float sx = cx + (c * dx + s * dy) / zoom, sy = cy + (-s * dx + c * dy) / zoom;
      if (wobA != 0.f) { sx += sinf(j * wobF + wobT) * wobA; sy += cosf(i * wobF * 1.3f - wobT) * wobA; }
      fx::gx[j][i] = sx; fx::gy[j][i] = sy;
    }
}

// ---- the crystal glass panel: a rosette of facets (mandala-symmetric Voronoi) ----
static uint8_t *s_fid = nullptr;
static const int NFAC = 37;
static float s_fnx[NFAC], s_fny[NFAC];
static int8_t s_fdx[NFAC], s_fdy[NFAC];
static int16_t s_fsh[NFAC];
static void crystallize() {
  if (!s_fid) s_fid = (uint8_t *)malloc(fx::LW * fx::LH);
  float sx[NFAC], sy[NFAC];
  float rot = (esp_random() % 628) / 100.f;
  int n = 0;
  sx[n] = 80; sy[n] = 60; n++;
  const int ringN[3] = {6, 12, 18}; const float ringR[3] = {20.f, 44.f, 74.f};
  for (int r = 0; r < 3; r++)
    for (int k = 0; k < ringN[r]; k++) {
      float a = rot + (k + (r & 1) * 0.5f) * 6.2831853f / ringN[r];
      float j = ((int)(esp_random() % 100) - 50) * 0.04f;
      sx[n] = 80 + cosf(a) * (ringR[r] + j) * 1.25f; sy[n] = 60 + sinf(a) * (ringR[r] + j); n++;
    }
  for (int i = 0; i < NFAC; i++) { float a = (esp_random() % 628) / 100.f; s_fnx[i] = cosf(a); s_fny[i] = sinf(a); }
  for (int y = 0; y < fx::LH; y++)
    for (int x = 0; x < fx::LW; x++) {
      int best = 0; float bd = 1e9f;
      for (int i = 0; i < NFAC; i++) { float dx = x - sx[i], dy = (y - sy[i]) * 1.25f, d = dx * dx + dy * dy; if (d < bd) { bd = d; best = i; } }
      s_fid[y * fx::LW + x] = (uint8_t)best;
    }
}

static void modePulse() {
  float dt = g_dt;
  const int16_t *sc = aud::scope;
  int cx = 80 - (int)(g_lookX * 20.f), cy = 60 + (int)(g_lookY * 15.f);
  if (g_shakeKick) for (int i = 0; i < 9; i++) s_palSeed[i] = (esp_random() % 1000) / 1000.f * (i < 3 ? 1.f : 1.5f) + (i >= 3 && i < 6 ? 0.5f : 0.f);
  if (aud::onset > 0.45f) { s_shock = 4.f; if (!aud::speakerLive()) hap(40, 12); }
  if (s_shock >= 0) s_shock += dt * 140.f;
  if (s_shock > 110) s_shock = -1;
  fx::swap();
  float t = g_t;
  switch (g_pulsePat) {
    case 0: {  // BLOOM: scope ring blossoms outward
      gridWarp(cx, cy, 1.02f + aud::bass * 0.035f, 0.012f + aud::mid * 0.04f, 0, 0, 0);
      fx::warp(5, true);
      int px = 0, py = 0;
      for (int i = 0; i <= 128; i++) {
        int k = i & 127;
        float a = k / 128.f * 6.2831853f + t * 0.4f;
        float v = sc[(k < 64 ? k : 127 - k) * 2] / 9000.f;
        float r = 18.f + g_level * 18.f + v * 26.f;
        int x = cx + (int)(cosf(a) * r), y = cy + (int)(sinf(a) * r * 0.9f);
        if (i) fx::line(px, py, x, y, 255);
        px = x; py = y;
      }
      fx::palCosine(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f, 1.f, 1.f, 0.f + aud::centroid, 0.33f, 0.67f, t * 0.05f);
      break;
    }
    case 1: {  // KALEIDO: mirrored waveform wedges, implosion swirl steered by tilt
      gridWarp(cx, cy, 0.975f + aud::bass * 0.08f, (g_lookX >= 0 ? 1.f : -1.f) * (0.03f + aud::mid * 0.06f), 0, 0, 0);
      fx::warp(4, true);
      for (int w = 0; w < 8; w++) {
        float base = w * 0.785398f + t * 0.3f;
        float mir = (w & 1) ? -1.f : 1.f;
        int px = cx, py = cy;
        for (int i = 0; i < 48; i++) {
          float rr = 4.f + i * 1.3f;
          float off = sc[i * 5] / 12000.f * mir;
          float a = base + off;
          int x = cx + (int)(cosf(a) * rr), y = cy + (int)(sinf(a) * rr);
          fx::line(px, py, x, y, (uint8_t)(140 + i * 2));
          px = x; py = y;
        }
      }
      fx::palCosine(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f, 1.f, 0.5f, 0.8f + g_lookX * 0.2f, 0.9f, 0.3f + g_lookY * 0.2f, t * 0.07f);
      break;
    }
    case 2: {  // SPECTRUM STAR: 64 band spokes rocketing through a hyperspace zoom
      gridWarp(cx, cy, 1.05f + aud::bass * 0.05f, g_lookX * 0.06f + 0.01f, 0, 0, 0);
      fx::warp(7, true);
      for (int b = 0; b < 64; b++) {
        int band = b < 32 ? b : 63 - b;
        float a = b / 64.f * 6.2831853f + t * 0.25f;
        float v = aud::bands[band];
        float r0 = 8.f + aud::bass * 8.f, r1 = r0 + v * 55.f;
        fx::line(cx + (int)(cosf(a) * r0), cy + (int)(sinf(a) * r0), cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1),
                 (uint8_t)(120 + v * 110.f));
      }
      if (s_shock >= 0) fx::ring(cx, cy, (int)s_shock, 255);
      fx::disc(cx, cy, 3 + (int)(aud::bass * 8.f), 255);
      fx::palCosine(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f, 0.7f, 0.4f, 0.f, 0.15f + aud::bass * 0.2f, 0.2f, t * 0.03f);
      break;
    }
    case 3: {  // PHASE SPACE: lissajous of the sound against itself, liquid warp
      gridWarp(80, 60, 1.0f + aud::bass * 0.03f, 0.004f, 1.5f + aud::mid * 5.f, 0.25f + aud::treble * 0.6f, t * 3.f);
      fx::warp(3, true);
      int lag = 3 + (int)(aud::centroid * 40.f);
      float rot = t * 0.4f + g_lookX;
      float cr = cosf(rot), sr = sinf(rot);
      int px = 0, py = 0;
      for (int i = 0; i < 200; i++) {
        float xa = sc[i] / 11000.f * 44.f, ya = sc[(i + lag) & 255] / 11000.f * 44.f;
        for (int m = 0; m < 4; m++) {
          float mx = (m & 1) ? -xa : xa, my = (m & 2) ? -ya : ya;
          int x = cx + (int)(cr * mx - sr * my), y = cy + (int)(sr * mx + cr * my);
          if (m == 0 && i) fx::line(px, py, x, y, 230);
          else fx::plot(x, y, 200);
          if (m == 0) { px = x; py = y; }
        }
      }
      float tl = atan2f(g_lookY, g_lookX + 0.001f) / 6.2831853f;
      fx::palCosine(0.6f, 0.5f, 0.5f, 0.4f, 0.5f, 0.5f, 1.f, 1.f, 1.f, tl, tl + 0.2f, tl + 0.5f, t * 0.02f);
      break;
    }
    default: {  // SYNESTHESIA: spectrum sculpts the flow field; motes paint it; everything cross-maps
      float big = aud::bass, fine = aud::treble, loud = g_level;
      float cxs = cx, cys = cy;
      auto td = M5.Touch.getDetail();
      if (td.isPressed() && td.y > 14 && td.y < H - 14) { cxs = td.x / 2.f; cys = td.y / 2.f; }
      for (int j = 0; j < fx::GH; j++)
        for (int i = 0; i < fx::GW; i++) {
          float x = i * 8.f, y = j * 8.f, dx = x - cxs, dy = y - cys;
          float r = sqrtf(dx * dx + dy * dy) + 1.f;
          float sw = (0.4f + big * 2.5f) / (1.f + r * 0.03f);              // inverse: near centre spins hardest
          float rip = sinf(r * (0.08f + fine * 0.5f) - t * 4.f) * (1.f + loud * 4.f);
          fx::gx[j][i] = x + (-dy / r) * sw * 6.f + dx / r * rip * 0.8f + sinf(y * 0.05f + t) * 0.6f;
          fx::gy[j][i] = y + (dx / r) * sw * 6.f + dy / r * rip * 0.8f + cosf(x * 0.05f - t) * 0.6f;
        }
      fx::warp(loud > 0.5f ? 7 : 3, true);
      if (aud::onset > 0.3f)
        for (int k = 0; k < 10; k++) {
          for (auto &m : s_mote) if (m.life <= 0) { float a = (esp_random() % 628) / 100.f; m = {cxs + cosf(a) * 6.f, cys + sinf(a) * 6.f, 1.f}; break; }
        }
      for (auto &m : s_mote) {
        if (m.life <= 0) continue;
        int gi = (int)(m.x / 8.f), gj = (int)(m.y / 8.f);
        if (gi < 0 || gj < 0 || gi >= fx::GW || gj >= fx::GH) { m.life = 0; continue; }
        m.x += (m.x - fx::gx[gj][gi]) * 1.6f; m.y += (m.y - fx::gy[gj][gi]) * 1.6f;
        m.life -= dt * 0.4f;
        fx::disc((int)m.x, (int)m.y, 1, (uint8_t)(120 + m.life * 135.f));
      }
      if (td.isPressed() && td.y > 14 && td.y < H - 14) fx::disc(td.x / 2, td.y / 2, 3 + (int)(loud * 4), 255);
      int px = 0, py = 0;                                  // scope ring gets twisted into spirals by the field
      for (int i = 0; i <= 96; i++) {
        float a = (i % 96) / 96.f * 6.2831853f;
        float rr = 14.f + big * 10.f + sc[(i % 96) * 2] / 9000.f * 10.f;
        int x = (int)cxs + (int)(cosf(a) * rr), y = (int)cys + (int)(sinf(a) * rr);
        if (i) fx::line(px, py, x, y, (uint8_t)(150 + fine * 100.f));
        px = x; py = y;
      }
      // quiet = deep saturated slow colour; loud = fast pastel strobe (inverse, nonlinear)
      float spd = 0.02f + loud * loud * 0.6f;
      static float ph = 0; ph += dt * spd * 10.f;
      float amp = 0.5f - loud * 0.2f;
      fx::palCosine(0.5f + loud * 0.2f, 0.5f + loud * 0.2f, 0.5f + loud * 0.2f, amp, amp, amp,
                    s_palSeed[3], s_palSeed[4], s_palSeed[5], s_palSeed[0], s_palSeed[1], s_palSeed[2], ph);
      break;
    }
  }
  // mantis opal over the pattern's own colours; big hits let the original colours bloom through
  static float bloom = 0;
  bloom = fmaxf(aud::onset, bloom - dt * 1.5f);
  float sheen = 0.5f + g_lookX * 0.4f + g_lookY * 0.3f + t * 0.015f;           // tilt it like labradorite
  // the body game: jumping lands a shock bloom; moving then freezing into a pose gets a crystal flash
  static float jumpB = 0, poseB = 0, motion = 0; static uint32_t jumpAt = 0;
  float gyr = fabsf(g_gyroX) + fabsf(g_gyroY) + fabsf(g_gyroZ);
  if (g_jolt > 0.5f && millis() - jumpAt > 250) { jumpAt = millis(); jumpB = 1.f; hap(120, 20); }
  if (motion > 140.f && gyr < 25.f) { poseB = 1.f; motion = 0; hapGesture(HG_THREAD); }
  motion += (gyr - motion) * clampf(dt * 8.f, 0.f, 1.f);
  jumpB = fmaxf(0.f, jumpB - dt * 2.5f); poseB = fmaxf(0.f, poseB - dt * 1.2f);
  fx::palOpal(0.5f - bloom * 0.3f, sheen, t * 0.05f, 0.45f + aud::onset * 0.5f + poseB * 0.8f);
  fx::palFlash(aud::onset * 0.15f + jumpB * 0.3f);

  // crystal glass panel (display only): facets refract with the bass and flash with the tilt
  if (!s_fid || g_shakeKick) crystallize();
  float refr = 1.5f + aud::bass * 4.f + g_level * 1.5f;
  for (int i = 0; i < NFAC; i++) {
    s_fdx[i] = (int8_t)(s_fnx[i] * refr); s_fdy[i] = (int8_t)(s_fny[i] * refr);
    float f = s_fnx[i] * g_lookX + s_fny[i] * g_lookY;                         // facet faces the light?
    s_fsh[i] = (int16_t)(f * 34.f + (f > 0.55f ? 40.f : 0.f) * (0.4f + g_level));
  }
  fx::presentCrystal(canvas, s_fid, s_fdx, s_fdy, s_fsh, (uint8_t)(14 + g_level * 30.f + poseB * 90.f));
  if (jumpB > 0.05f) for (int k = 0; k < 3; k++) {                               // shock bloom rings
    int r = (int)((1.f - jumpB) * 170.f) + k * 12;
    canvas.drawCircle(cx * 2, cy * 2, r, k == 1 ? wire::TEAL : wire::LIME);
  }

  // ---- crisp layers: no echo, so they read as glass against the liquid ----
  int CX = cx * 2, CY = cy * 2;
  wire::engrave(CX, CY, 36.f + aud::bass * 10.f, t * 0.04f + g_lookX * 0.15f, 1 + (int)(g_level * 2.f + aud::onset * 4.f));
  // the waveform: bent by the same flow as the liquid, torn by loud entropy, leaves a faint ghost in the feedback
  {
    int py = -1, px = 0;
    float ent = g_level * 0.35f + aud::onset * 0.3f;
    uint32_t r = (uint32_t)(t * 1000.f);
    for (int x = 0; x < W; x += 2) {
      int gi = x / 16, gj = CY / 16; if (gi > fx::GW - 1) gi = fx::GW - 1; if (gj > fx::GH - 1) gj = fx::GH - 1; if (gj < 0) gj = 0;
      float dxw = (gi * 8.f - fx::gx[gj][gi]) * 6.f, dyw = (gj * 8.f - fx::gy[gj][gi]) * 6.f;
      int k0 = (x >> 1) & 255;
      float sv = (sc[k0] + sc[(k0 + 1) & 255] + sc[(k0 + 2) & 255] + sc[(k0 + 3) & 255]) * 0.25f;   // flowing, not spiky
      int y = CY + (int)(sv / 9000.f * (26.f + g_level * 30.f) + dyw);
      int xx = x + (int)dxw;
      r = r * 1664525u + 1013904223u;
      bool tear = ((r >> 24) & 255) < (uint32_t)(ent * 255.f);
      if (py >= 0 && !tear) {
        canvas.drawLine(px, py + 2, xx, y + 2, wire::PLUM);
        canvas.drawLine(px, py, xx, y, wire::LIME);
        fx::line(px / 2, py / 2, xx / 2, y / 2, 70);
      }
      px = xx; py = y;
    }
  }
  // the sage's crystal: a tesseract turning in four dimensions
  float ts = (g_pulsePat == 2 ? 12.f : 18.f) + aud::bass * 22.f + poseB * 26.f;
  wire::tesseract(CX, CY, ts, t * 0.7f + aud::mid, t * 0.5f + aud::treble * 2.f, t * 0.3f, aud::onset * 0.6f, rgb565(40, 220, 200));
}

// ============================================================
//  chrome, modes, buttons
// ============================================================
static void drawChrome() {
  static const char *names[] = {"swarm", "eye", "tunnel", "pulse", "calm", "mantis", "rooms", "garden", "meditate"};
  canvas.fillRect(0, 0, W, 13, rgb565(3, 10, 12));
  canvas.fillRect(0, H - 13, W, 13, rgb565(3, 10, 12));
  canvas.drawFastHLine(0, 13, W, wire::PLUM); canvas.drawFastHLine(0, H - 14, W, wire::PLUM);
  canvas.drawFastHLine(0, 13, (int)(W * clampf(g_level, 0.f, 1.f)), wire::LIME);            // live mic meter
  canvas.setTextSize(1);
  {  // battery: a small outline cell, fill = charge (lime > 30 %, amber > 15 %, red below), a spark when charging
    static int lvl = -1; static uint32_t at = 0; static bool chg = false;
    if (lvl < 0 || millis() - at > 5000) { at = millis(); lvl = M5.Power.getBatteryLevel(); chg = M5.Power.isCharging(); }
    int L = lvl < 0 ? 0 : (lvl > 100 ? 100 : lvl);
    canvas.drawRect(8, 3, 22, 8, rgb565(90, 110, 110)); canvas.fillRect(30, 5, 2, 4, rgb565(90, 110, 110));
    uint16_t bc = L > 30 ? wire::LIME : (L > 15 ? rgb565(255, 190, 40) : rgb565(255, 60, 60));
    canvas.fillRect(10, 5, (18 * L) / 100, 4, bc);
    if (chg) { canvas.drawLine(20, 4, 17, 7, rgb565(255, 255, 255)); canvas.drawLine(17, 7, 21, 7, rgb565(255, 255, 255)); canvas.drawLine(21, 7, 18, 10, rgb565(255, 255, 255)); }
  }
  canvas.setTextColor(rgb565(40, 190, 180)); canvas.setCursor(40, 3); canvas.print(g_mode == MODE_ROOMS ? roomsName() : (g_mode == MODE_GARDEN ? gardenName() : names[g_mode]));
  int orb = 2 + (int)(g_level * 5.f + g_peak * 3.f);
  if (orb > 6) orb = 6;
  canvas.fillCircle(W - 12, 6, orb + 1, wire::PLUM);
  canvas.fillCircle(W - 12, 6, orb, aud::onset > 0.3f ? wire::LIME : hsv565(g_hue + g_level * 60.f, 0.7f, 0.45f + g_level * 0.4f));

  const char *bl = "";
  switch (g_mode) {
    case MODE_SWARM: { static const char *v[] = {"flock", "orbit", "chaos"}; bl = v[g_swarmVar]; break; }
    case MODE_EYE: bl = g_eyeTrack ? "gaze" : "stare"; break;
    case MODE_TUNNEL: { static const char *v[] = {"dive", "recede", "fractal", "portal"}; bl = v[g_tunnelMode]; break; }
    case MODE_PULSE: { static const char *v[] = {"bloom", "kaleido", "star", "phase", "synesthesia"}; bl = v[g_pulsePat]; break; }
    case MODE_MANTIS: bl = g_mantisMode == 2 ? (aud::caveState() == 0 ? "lean in" : "...") : (g_mantisSing ? "sing" : "dance"); break;
    case MODE_GARDEN: bl = "switch"; break;
    case MODE_MEDITATE: bl = "begin / end"; break;
    case MODE_CALM: bl = calmName(); break;
    case MODE_ROOMS: bl = "next"; break;
    default: break;
  }
  canvas.setTextColor(hsv565(g_hue, 0.35f, 0.55f));
  canvas.setCursor(10, H - 10); canvas.print("<");
  canvas.setTextColor(g_mode == MODE_ROOMS && !roomsSolved() ? rgb565(55, 60, 70) : wire::LIME);   // greyed until solved
  canvas.setCursor(W / 2 - (int)strlen(bl) * 3, H - 10); canvas.print(bl);
  canvas.setTextColor(hsv565(g_hue, 0.35f, 0.55f));
  canvas.setCursor(W - 16, H - 10); canvas.print(">");
}

static void nextMode(int dir) {
  if (g_mode == MODE_MEDITATE) medLeave();
  if (g_mode == MODE_MANTIS) aud::cave(false);
  int m = ((int)g_mode + dir + MODE_COUNT) % MODE_COUNT;
  g_mode = (Mode)m;
  fx::clear(0);
  hap(100, 25);
  if (g_mode == MODE_TUNNEL) flightPoseSoon();
  if (g_mode == MODE_ROOMS) roomsEnter();                 // always starts back at Mantis NRG
  if (g_mode == MODE_MEDITATE) medEnter();
  if (g_mode == MODE_MANTIS && g_mantisMode == 2) aud::cave(true);
  stats::save();
}

static void btnBShort() {
  switch (g_mode) {
    case MODE_SWARM: g_swarmVar = (SwarmVar)((g_swarmVar + 1) % SV_COUNT); break;
    case MODE_EYE: g_eyeTrack = !g_eyeTrack; break;
    case MODE_TUNNEL: g_tunnelMode = (TunnelMode)((g_tunnelMode + 1) % TM_COUNT); fx::clear(0); flightPoseSoon(); break;
    case MODE_PULSE: g_pulsePat = (g_pulsePat + 1) % PP_COUNT; break;
    case MODE_MANTIS:
      if (g_mantisMode == 2) { aud::caveRecord(); break; }           // in the cave, B = lean in and speak
      g_mantisMode = (uint8_t)((g_mantisMode + 1) % 3);
      g_mantisSing = g_mantisMode == 1;
      aud::cave(g_mantisMode == 2);
      if (g_mantisMode >= 1) aud::calibrateAmbient(3000.f);
      break;
    case MODE_GARDEN: gardenNext(); break;
    case MODE_MEDITATE: medButton(); break;
    case MODE_CALM: calmNext(); break;
    case MODE_ROOMS: if (roomsSolved()) { roomsNext(); stats::event(stats::EV_ROOM); hapGesture(HG_THREAD); } else hap(35, 12); break;
    default: break;
  }
  hap(90, 20);
}
// ============================================================
//  boot
// ============================================================
static void splash() {
  M5Canvas &c = *s_fb[0];
  c.fillSprite(rgb565(6, 2, 14));
  int ox = (W - MANTIS_W) / 2, oy = 18;
  for (int y = 0; y < MANTIS_H; y++)
    for (int x = 0; x < MANTIS_W; x++) {
      uint16_t col = mantis_splash[y * MANTIS_W + x];
      if (col) c.drawPixel(ox + x, oy + y, col);
    }
  c.setTextSize(2); c.setTextColor(hsv565(160, 0.85f, 0.95f));
  c.setCursor((W - 7 * 12) / 2, oy + MANTIS_H + 8); c.print("SYNAPSE");
  c.setTextSize(1); c.setTextColor(rgb565(120, 180, 160));
  c.setCursor((W - 17 * 6) / 2, oy + MANTIS_H + 30); c.print("a small green bug");
  c.pushSprite(&M5.Display, 0, 0);
  M5.Speaker.begin();
  M5.Speaker.setVolume(170);
  static const int seq[][2] = {{523, 55}, {659, 55}, {784, 55}, {1047, 110}, {0, 35}, {784, 45}, {1047, 150}};
  for (auto &s : seq) {
    if (s[0]) M5.Speaker.tone((float)s[0], (uint32_t)s[1]);
    delay(s[1] + 12);
  }
  M5.Speaker.stop();
  M5.Speaker.end();
}

#ifdef HOST
M5Canvas *hostLastFrame() { return s_fb[s_pushIdx]; }
#endif

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true; cfg.internal_mic = true; cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  if (M5.Imu.isEnabled()) { M5.Imu.loadOffsetFromNVS(); for (int i = 0; i < 15; i++) { M5.Imu.update(); delay(4); } }
  loadCal();

  for (int i = 0; i < 2; i++) { s_fb[i]->setColorDepth(16); s_fb[i]->setPsram(true); }
  s_fb[0]->createSprite(W, H);
  s_double = s_fb[1]->createSprite(W, H) != nullptr;
  g_cv = s_fb[0];
  splash();
  fx::begin();
  mantisBegin();
  calmBegin();
  roomsBegin();
  stats::begin();
  gardenBegin();
  seedParticles();
  aud::begin();
  if (s_double) {
    s_dispIdle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_dispIdle);
    xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 2, &s_dispTask, 0);
  }
  g_cv = s_fb[s_cur];
}

void loop() {
  if (g_mode != MODE_MANTIS) s_party = false;   // party mode is per visit, default off
  danceNetService(s_party);                     // MantisNow DANCE sender (dance_net.cpp)
  static uint32_t last = micros();
  uint32_t now = micros();
  g_dt = clampf((now - last) / 1e6f, 0.004f, 0.06f);
  last = now;

  pollInput();
  aud::service(g_dt);
  sampleImu();
  calService();
  stats::tick(g_dt, g_mode == MODE_CALM || g_mode == MODE_GARDEN || g_mode == MODE_MEDITATE);
  g_level = aud::level; g_peak = aud::peak;
  for (int i = 0; i < MIC_N; i++) g_mic[i] = aud::scope[i * 2];

  g_t += g_dt * (0.9f + g_level * 0.6f);
  g_hue = fmodf(g_hue + g_dt * (3.6f + g_level * 2.4f), 360.f);

  switch (g_mode) {
    case MODE_SWARM: modeSwarm(); break;
    case MODE_EYE: modeEye(); break;
    case MODE_TUNNEL: modeTunnel(); break;
    case MODE_PULSE: modePulse(); break;
    case MODE_CALM: calmDraw(); break;
    case MODE_MANTIS: if (g_mantisMode == 2) mantisDrawCave(); else mantisDraw(g_mantisSing); break;
    case MODE_ROOMS: roomsDraw(); break;
    case MODE_GARDEN: gardenDraw(); break;
    case MODE_MEDITATE: medDraw(); break;
    default: break;
  }
  g_shakeKick = false;
  drawChrome();
  drawCal();
  if (g_mode == MODE_MANTIS && g_mantisMode == 1 && aud::calibrating()) {
    canvas.setTextColor(wire::LIME); canvas.setCursor(W / 2 - 51, 20); canvas.print("listening to the room");
  }
  present();
}
