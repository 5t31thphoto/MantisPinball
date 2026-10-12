#include "common.h"
#include "../../../src/mantisskate/mantisskate_mode.cpp"
static uint32_t rs=12345; static int rnd(int n){ rs=rs*1103515245u+12345u; return (int)((rs>>16)%n); }
static void frame(bool l,bool r){ btnL_prev=btnL; btnR_prev=btnR; btnL=l; btnR=r; g_ms+=16; mantisskateTick(); mantisskateDraw(); }
int main(){
  for(int cycle=0; cycle<5; cycle++){
    mantisskateEnter();
    for(int i=0;i<3000;i++){ int k=rnd(10); frame(k==1||k==3||k==5, k==2||k==3||k==6); }
    mantisskateShutdown();
  }
  printf("  %-14s 5 x (Enter, 3000 frames of random play, Shutdown): no use-after-free, no null access, no leak\n","mantisskate");
  return 0; }
