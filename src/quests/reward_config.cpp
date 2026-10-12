// FILE: src/quests/reward_config.cpp
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>
#include "reward_config.h"
#include "quest_profile.h"

static constexpr int RC_MAX_DIFF = 5;
static constexpr int RC_MAX_CAT  = 8;
static constexpr int RC_NAME_LEN = 18;

struct RcDiff {
    bool     set;
    uint16_t xpBase, xpSpread;
    uint8_t  diePct, dieSides;
    bool     tierSet;
    uint8_t  cum[5];        // cumulative percents: none,low,mid,high,special
};
struct RcItem {
    bool    tierSet, tagSet;
    uint8_t tier, tag;
    char    name[RC_NAME_LEN];   // empty = no override
};

static RcDiff  g_diff[RC_MAX_DIFF+1];
static uint8_t g_catAttr[RC_MAX_CAT][4];
static uint8_t g_catAttrN[RC_MAX_CAT];
static RcItem  g_item[QP_ITEM_COUNT];
static bool    g_loaded = false;

bool rcLoaded(){ return g_loaded; }

// tiny field splitter for 'a|b|c' lines
static int rcSplit(char* line, char* out[], int maxF){
    int n=0; char* p=line;
    out[n++]=p;
    while(*p && n<maxF){
        if(*p=='|'){ *p='\0'; out[n++]=p+1; }
        p++;
    }
    return n;
}

bool rcLoad(){
    memset(g_diff,0,sizeof(g_diff));
    memset(g_catAttr,0,sizeof(g_catAttr));
    memset(g_catAttrN,0,sizeof(g_catAttrN));
    memset(g_item,0,sizeof(g_item));
    g_loaded=false;

    if(!LittleFS.begin(false)) return false;
    fs::File f = LittleFS.open("/content/rewards.cfg","r");
    if(!f) return false;

    char line[96];
    bool sawHeader=false;
    while(f.available()){
        int n=f.readBytesUntil('\n',line,sizeof(line)-1);
        if(n<=0) continue;
        line[n]='\0';
        // strip CR and trailing space
        while(n>0 && (line[n-1]=='\r'||line[n-1]==' '||line[n-1]=='\t')) line[--n]='\0';
        if(n==0||line[0]=='#') continue;

        if(!sawHeader){
            if(strncmp(line,"@REWARDCFG",10)==0){ sawHeader=true; continue; }
            // header must come first; anything else means this isn't our file
            f.close(); return false;
        }
        char* fld[8];
        int fn = rcSplit(line,fld,8);
        if(fn<2) continue;

        switch(line[0]){
        case 'D': {                     // D|diff|xpBase|xpSpread|diePct|dieSides
            if(fn<6) break;
            int d=atoi(fld[1]);
            if(d<1||d>RC_MAX_DIFF) break;
            g_diff[d].set=true;
            g_diff[d].xpBase   =(uint16_t)atoi(fld[2]);
            g_diff[d].xpSpread =(uint16_t)atoi(fld[3]);
            g_diff[d].diePct   =(uint8_t)atoi(fld[4]);
            g_diff[d].dieSides =(uint8_t)atoi(fld[5]);
            break; }
        case 'T': {                     // T|diff|none|low|mid|high|special
            if(fn<7) break;
            int d=atoi(fld[1]);
            if(d<1||d>RC_MAX_DIFF) break;
            int acc=0;
            for(int i=0;i<5;i++){
                acc += atoi(fld[2+i]);
                if(acc>100) acc=100;
                g_diff[d].cum[i]=(uint8_t)acc;
            }
            g_diff[d].cum[4]=100;       // always closed so a roll can't fall off
            g_diff[d].tierSet=true;
            break; }
        case 'C': {                     // C|cat|a,b,c
            if(fn<3) break;
            int c=atoi(fld[1]);
            if(c<0||c>=RC_MAX_CAT) break;
            int k=0; char* p=fld[2];
            while(*p&&k<4){
                g_catAttr[c][k++]=(uint8_t)atoi(p);
                char* comma=strchr(p,',');
                if(!comma) break;
                p=comma+1;
            }
            g_catAttrN[c]=(uint8_t)k;
            break; }
        case 'I': {                     // I|id|tier|tag|name
            if(fn<4) break;
            int id=atoi(fld[1]);
            if(id<0||id>=QP_ITEM_COUNT) break;
            g_item[id].tier=(uint8_t)atoi(fld[2]); g_item[id].tierSet=true;
            g_item[id].tag =(uint8_t)atoi(fld[3]); g_item[id].tagSet=true;
            if(fn>=5&&fld[4][0]){ strncpy(g_item[id].name,fld[4],RC_NAME_LEN-1);
                                  g_item[id].name[RC_NAME_LEN-1]='\0'; }
            break; }
        default: break;
        }
    }
    f.close();
    g_loaded = sawHeader;
    return g_loaded;
}

bool rcXp(uint8_t diff,uint16_t& base,uint16_t& spread){
    if(!g_loaded||diff<1||diff>RC_MAX_DIFF||!g_diff[diff].set) return false;
    base=g_diff[diff].xpBase; spread=g_diff[diff].xpSpread; return true;
}
bool rcTierOdds(uint8_t diff,uint8_t out[5]){
    if(!g_loaded||diff<1||diff>RC_MAX_DIFF||!g_diff[diff].tierSet) return false;
    memcpy(out,g_diff[diff].cum,5); return true;
}
bool rcDie(uint8_t diff,uint8_t& pct,uint8_t& sides){
    if(!g_loaded||diff<1||diff>RC_MAX_DIFF||!g_diff[diff].set) return false;
    pct=g_diff[diff].diePct; sides=g_diff[diff].dieSides; return true;
}
int rcDieAttrs(uint8_t cat,uint8_t out[4]){
    if(!g_loaded||cat>=RC_MAX_CAT||g_catAttrN[cat]==0) return 0;
    int k=g_catAttrN[cat]; memcpy(out,g_catAttr[cat],k); return k;
}
bool rcItemTier(uint8_t id,uint8_t& tier){
    if(!g_loaded||id>=QP_ITEM_COUNT||!g_item[id].tierSet) return false;
    tier=g_item[id].tier; return true;
}
bool rcItemTag(uint8_t id,uint8_t& tag){
    if(!g_loaded||id>=QP_ITEM_COUNT||!g_item[id].tagSet) return false;
    tag=g_item[id].tag; return true;
}
const char* rcItemName(uint8_t id){
    if(!g_loaded||id>=QP_ITEM_COUNT||!g_item[id].name[0]) return nullptr;
    return g_item[id].name;
}
