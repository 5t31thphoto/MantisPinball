#include "cube_core.h"
#include "cube_policy.h"
#include "cube_endgame_api.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
int main(int argc,char**argv){
  int trials=argc>1?atoi(argv[1]):20;
  srand(7); 
  printf("policy arena: %.1f KB\n", mcPolicyArenaBytes()/1024.0);
  if(!mcPolicyArenaAcquire()){ printf("arena alloc failed\n"); return 1; }
  int ok=0,tot=0,worst=0; double ms=0;
  for(int t=0;t<trials;t++){
    McCube c; mcSolved(c);
    for(int i=0;i<25;i++) mcApplyMove(c,rand()%18);
    McSolution s; McProgress p{}; McPolicyOptions o; mcDefaultPolicyOptions(o);
    auto t0=std::chrono::steady_clock::now();
    bool f=mcPolicySolve(c,s,p,o);
    ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    if(!f) continue;
    McCube v=c; for(int i=0;i<s.length;i++) mcApplyMove(v,s.moves[i]);
    if(mcIsSolved(v)){ok++;tot+=s.length;if(s.length>worst)worst=s.length;}
    else printf("  trial %d INVALID\n",t);
  }
  printf("solved %d/%d  avg %.1f  worst %d  avg %.0f ms\n",
     ok,trials,ok?(double)tot/ok:0.0,worst,ms/trials);
}
