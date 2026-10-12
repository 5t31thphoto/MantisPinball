// Dumps logits for states given as scramble move sequences, so the host
// reference implementation can be diffed against device code bit-for-bit.
#include "cube_core.h"
#include "cube_policy.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv){
  if(!mcPolicyArenaAcquire()) return 1;   // arena is heap since the SRAM fix
  McCube c; mcSolved(c);
  for(int i=1;i<argc;i++) mcApplyMove(c, atoi(argv[i]));
  int32_t lg[18];
  mcPolicy(c, lg);
  for(int i=0;i<18;i++) printf("%d%s", lg[i], i==17?"\n":" ");
  return 0;
}
