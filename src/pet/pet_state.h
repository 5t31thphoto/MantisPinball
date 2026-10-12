// pet_state.h - Persistent state for the mantis pet (Tamagotchi/Digimon style).
//
// The pet has its OWN level and XP, distinct from the quest-profile player
// level. Pet XP comes from pet activities (training, battles, care) AND from
// deposits made by Quests/CYOA via qpGrantPetXp (consumed on Pet Mode entry).
//
// Care stats (hunger/happiness/energy/clean) decay SLOWLY and only while Pet
// Mode is open. HP is the battle health pool. Feeding/cleaning/playing use
// shared inventory items (TAG_PET) and raise the relevant stats.
//
// Persisted to NVS namespace "petstate".
#pragma once
#include <stdint.h>

struct PetState {
    uint32_t version;
    char     name[16];
    uint32_t petLevel;
    uint32_t petXp;        // XP toward the next pet level
    uint8_t  hunger;       // 0..100 (100 = full)
    uint8_t  happiness;    // 0..100
    uint8_t  energy;       // 0..100
    uint8_t  hp;           // current battle HP (0..maxHp)
    uint8_t  clean;        // 0..100 (100 = spotless)
    uint32_t battlesWon;
    uint32_t lastTickMs;   // for slow decay while open
};

void     petLoad();
void     petSave();
void     petResetDefaults();

uint32_t petLevel();
uint32_t petXp();
uint32_t petXpForNext();        // XP span of current pet level
void     petAddXp(uint32_t n);  // may level up (also heals on level up)

uint8_t  petMaxHp();            // derived from level
uint8_t  petAttack();           // derived from level
uint8_t  petDefense();          // derived from level
uint8_t  petSpeed();            // derived from level

void     petDecayTick();        // slow decay; call each frame while mode open
void     petFullHeal();

PetState& petData();
