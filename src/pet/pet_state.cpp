// pet_state.cpp - mantis pet persistent state + derived stats.
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "pet_state.h"

static PetState g_pet;
static const uint32_t PET_VERSION = 1;

// ── Leveling ──────────────────────────────────────────────────────────────────
// Pet level span grows gently: need(L) = 40*L + 20.
uint32_t petLevel()     { return g_pet.petLevel; }
uint32_t petXp()        { return g_pet.petXp; }
uint32_t petXpForNext() { return 40u * (g_pet.petLevel ? g_pet.petLevel : 1) + 20u; }

void petAddXp(uint32_t n) {
    g_pet.petXp += n;
    bool leveled = false;
    while (g_pet.petXp >= petXpForNext()) {
        g_pet.petXp -= petXpForNext();
        g_pet.petLevel++;
        leveled = true;
        if (g_pet.petLevel > 999) { g_pet.petLevel = 999; g_pet.petXp = 0; break; }
    }
    if (leveled) {
        g_pet.hp = petMaxHp();   // level-up restores HP
        if (g_pet.happiness < 100)
            g_pet.happiness = (g_pet.happiness + 20 > 100) ? 100 : g_pet.happiness + 20;
    }
    petSave();
}

// ── Derived battle stats ──────────────────────────────────────────────────────
uint8_t petMaxHp()   { uint32_t v = 30 + petLevel() * 6; return v > 250 ? 250 : (uint8_t)v; }
uint8_t petAttack()  { uint32_t v = 8  + petLevel() * 2; return v > 200 ? 200 : (uint8_t)v; }
uint8_t petDefense() { uint32_t v = 5  + petLevel() * 2; return v > 200 ? 200 : (uint8_t)v; }
uint8_t petSpeed()   { uint32_t v = 6  + petLevel();     return v > 200 ? 200 : (uint8_t)v; }

// ── Decay (slow, only while Pet Mode is open) ─────────────────────────────────
void petDecayTick() {
    uint32_t now = millis();
    if (g_pet.lastTickMs == 0) { g_pet.lastTickMs = now; return; }
    if (now - g_pet.lastTickMs < 20000) return;   // 20s cadence
    g_pet.lastTickMs = now;
    if (g_pet.hunger > 0)    g_pet.hunger--;
    if (g_pet.happiness > 0) g_pet.happiness--;
    if (g_pet.clean > 0)     g_pet.clean--;
    if (g_pet.energy < 100)  g_pet.energy++;       // rests up while idle
    petSave();
}

void petFullHeal() { g_pet.hp = petMaxHp(); }

// ── Persistence ───────────────────────────────────────────────────────────────
void petResetDefaults() {
    memset(&g_pet, 0, sizeof(g_pet));
    g_pet.version = PET_VERSION;
    strncpy(g_pet.name, "Mantis", sizeof(g_pet.name) - 1);
    g_pet.petLevel = 1;
    g_pet.petXp = 0;
    g_pet.hunger = 70;
    g_pet.happiness = 70;
    g_pet.energy = 100;
    g_pet.clean = 90;
    g_pet.battlesWon = 0;
    g_pet.hp = petMaxHp();
    g_pet.lastTickMs = 0;
}

void petLoad() {
    Preferences p;
    bool ok = false;
    if (p.begin("petstate", true)) {
        size_t n = p.getBytesLength("pet");
        if (n == sizeof(PetState)) {
            p.getBytes("pet", &g_pet, sizeof(PetState));
            if (g_pet.version == PET_VERSION) ok = true;
        }
        p.end();
    }
    if (!ok) { petResetDefaults(); petSave(); }
    if (g_pet.hp > petMaxHp()) g_pet.hp = petMaxHp();
    g_pet.lastTickMs = 0;
}

void petSave() {
    Preferences p;
    if (p.begin("petstate", false)) {
        p.putBytes("pet", &g_pet, sizeof(PetState));
        p.end();
    }
}

PetState& petData() { return g_pet; }
