#include "cube_core.h"
#include "cube_endgame_api.h"
#include "cube_tables.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv){
  int trials = argc>1?atoi(argv[1]):2000;
  srand(4242);
  int inball=0, solved=0, optimal=0, rejected=0, wrong=0;
  for(int d=1; d<=5; d++){
    for(int t=0;t<trials/5;t++){
      McCube c; mcSolved(c);
      int used[8]; int n=0;
      for(int i=0;i<d;i++){
        int m; bool ok;
        do { m=rand()%18; ok=true;
             for(int j=0;j<n;j++) if(used[j]/3==m/3) { ok=(j!=n-1); }
             if(n>0 && used[n-1]/3==m/3) ok=false;
        } while(!ok);
        used[n++]=m; mcApplyMove(c,m);
      }
      if(mcIsSolved(c)) continue;
      inball++;
      uint8_t mv[8];
      int len = mcEndgameSolve(c, mv, 8);
      if(len<0){ rejected++; continue; }
      McCube v=c; for(int i=0;i<len;i++) mcApplyMove(v,mv[i]);
      if(!mcIsSolved(v)) wrong++; else { solved++; if(len<=d) optimal++; }
    }
  }
  printf("in-ball scrambles : %d\n", inball);
  printf("  solved          : %d\n", solved);
  printf("  optimal or better: %d\n", optimal);
  printf("  rejected        : %d\n", rejected);
  printf("  WRONG           : %d\n", wrong);

  int rej=0, badrej=0;
  for(int t=0;t<1000;t++){
    McCube c; mcSolved(c);
    for(int i=0;i<25;i++) mcApplyMove(c, rand()%18);
    uint8_t mv[8];
    int len = mcEndgameSolve(c, mv, 8);
    if(len<0) rej++;
    else { McCube v=c; for(int i=0;i<len;i++) mcApplyMove(v,mv[i]);
           if(!mcIsSolved(v)) badrej++; }
  }
  printf("random cubes rejected: %d/1000, wrong answers: %d\n", rej, badrej);
  return wrong+badrej;
}
