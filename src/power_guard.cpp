// FILE: src/power_guard.cpp
#include "power_guard.h"

static constexpr int PG_MAX = 24;
static FlushFn pgTable[PG_MAX];
static int     pgCount = 0;

void pgRegisterFlush(FlushFn fn){
    if(!fn) return;
    for(int i=0;i<pgCount;i++) if(pgTable[i]==fn) return;
    if(pgCount<PG_MAX) pgTable[pgCount++]=fn;
}

void pgFlushAll(){
    for(int i=0;i<pgCount;i++) pgTable[i]();
}
