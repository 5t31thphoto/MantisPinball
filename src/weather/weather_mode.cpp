// ============================================================================
//  Weather Report Mode  —  weather_mode.cpp
//
//  Landscape 320×170 display (same pattern as BTC miner mode).
//  Reuses main.cpp's TFT_eSprite fb by deleting its portrait buffer and
//  recreating it in landscape — exactly as the miner does.
//
//  Free APIs (no key, no account):
//    geocoding-api.open-meteo.com  — zip → lat/lon
//    api.open-meteo.com            — forecast (temp, wind, precip, pressure,
//                                     UV, solar radiation, sunrise/sunset)
//    air-quality-api.open-meteo.com — US AQI, PM2.5
//
//  NVS:
//    "btcminer" namespace  — WiFi ssid/pass  (READ ONLY)
//    "pinball"  namespace  — wx_zip, wx_lat, wx_lon, wx_city  (read/write)
//
//  Never runs at the same time as the miner.
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <math.h>
#include "weather_mode.h"
#include "../sysclock/sysclock.h"
#include "../theme_colors.h"
#include "../net_yield.h"

// Button state from main.cpp (read-only — weather handles its own reset gesture)
extern bool btnL, btnR;

// ── Shared objects (owned by main.cpp) ───────────────────────────────────────
extern TFT_eSPI    tft;
extern TFT_eSprite fb;
extern bool        fbReady;

// ── Landscape dimensions ─────────────────────────────────────────────────────
static constexpr int LW = 320;
static constexpr int LH = 170;

// ── Sprite (same reuse pattern as miner) ─────────────────────────────────────
static TFT_eSprite* sFb            = nullptr;
static uint8_t      sSavedRotation = 0;

// ── NVS ──────────────────────────────────────────────────────────────────────
static constexpr const char* NVS_MINER = "btcminer";
static constexpr const char* NVS_GAME  = "pinball";

// ── Portal ────────────────────────────────────────────────────────────────────
static constexpr const char* WX_AP   = "pinball-weather";
static constexpr uint32_t WX_PORTAL_TIMEOUT = 120000;

// ── Timing ────────────────────────────────────────────────────────────────────
static constexpr uint32_t WX_REFRESH_MS = 600000;  // 10 min full refresh
static constexpr uint32_t WX_WIFI_MS    = 15000;
static constexpr uint32_t WX_RETRY_MS   = 30000;

// ── State ─────────────────────────────────────────────────────────────────────
enum WxPhase { WX_SETUP, WX_WIFI, WX_FETCH, WX_DISPLAY, WX_ERROR };

// Long-press reset for zip code (same pattern as miner left-hold reset)
static uint32_t wxResetHoldStart = 0;
static bool     wxResetArmed     = false;   // first confirm stage shown
static bool     wxResetPending   = false;   // second hold confirms wipe
static WxPhase  wxPhase    = WX_WIFI;
static uint32_t wxPhaseAt  = 0;
static char     wxStatus[48] = "STARTING";

// ── Config ────────────────────────────────────────────────────────────────────
static char  wxSsid[64] = "";
static char  wxPass[64] = "";
static char  wxZip[16]  = "";
static float wxLat      = 0.0f;
static float wxLon      = 0.0f;
static char  wxCity[40] = "";
static bool  wxHasCoords = false;

// ── Weather data ──────────────────────────────────────────────────────────────
struct WxData {
    // Current
    float  tempF;
    float  feelsF;
    float  humidity;
    float  windMph;
    int    windDir;
    float  gustMph;
    float  precipMm;
    float  pressureHpa;
    int    uvIndex;
    int    wmoCode;
    int    isDay;
    float  solarWm2;        // shortwave_radiation current hour
    // Today summary
    float  maxF, minF;
    float  precipTotalMm;
    char   sunrise[8];      // "HH:MM"
    char   sunset[8];
    // Daily (7 days)
    float  dayMaxF[7];
    float  dayMinF[7];
    int    dayCode[7];
    float  dayPrecipMm[7];
    char   dayLabel[7][5];  // "MON" etc
    // Hourly (next 12h)
    float  hourTempF[12];
    float  hourPrecip[12];
    int    hourCode[12];
    int    hourIsDay[12];
    int    hourLabel[12];   // hour number 0-23
    // AQI
    int    usAqi;
    float  pm25;
    // 24h pressure trend (last 6 values, 4h apart)
    float  pressureTrend[6];
};
static WxData wxd  = {};
static bool   wxOk = false;
static uint32_t wxFetchedAt = 0;

// ── Portal objects ────────────────────────────────────────────────────────────
static WebServer* wxSrv = nullptr;
static DNSServer* wxDns = nullptr;

// ── Portal HTML ───────────────────────────────────────────────────────────────
static const char WX_HTML[] PROGMEM = R"RAW(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{background:#001111;color:#00e7e7;font-family:monospace;padding:20px;max-width:480px;margin:0 auto}
h2{color:#00ff00}
.card{background:#002222;border:1px solid #007373;border-radius:6px;padding:16px;margin:12px 0}
label{display:block;margin-bottom:4px;font-size:13px;color:#007373}
input{width:100%;box-sizing:border-box;background:#001a1a;border:1px solid #007373;
      color:#00e7e7;padding:8px;font-family:monospace;font-size:15px;border-radius:4px;margin-bottom:12px}
button{background:#003c3c;border:1px solid #00e7e7;color:#00e7e7;padding:10px 24px;
       font-family:monospace;font-size:14px;border-radius:4px;cursor:pointer;width:100%}
button:hover{background:#007373}
.note{font-size:11px;color:#005a5a;margin-top:4px}
</style></head><body>
<h2>&#9928; WEATHER SETUP</h2>
<div class='card'>
<p style='font-size:13px'>WiFi creds are shared with the miner.<br>Already configured there? They are pre-filled.</p>
<label>WiFi SSID</label><input id='s' value='%SSID%'>
<label>WiFi Password</label><input id='p' type='password' value='%PASS%'>
</div>
<div class='card'>
<label>ZIP Code</label><input id='z' value='%ZIP%' inputmode='numeric' maxlength='10' placeholder='84084'>
<p class='note'>Uses Open-Meteo — free, no account needed.</p>
</div>
<button onclick='go()'>SAVE &amp; CONNECT</button>
<script>function go(){
var x=new XMLHttpRequest();
x.open('POST','/wx',true);
x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
x.onload=function(){document.body.innerHTML='<h2 style="color:#0f0">SAVED!</h2>';};
var p=document.getElementById('p').value;
if(p==='••••••••')p='';
x.send('s='+encodeURIComponent(document.getElementById('s').value)+
       '&p='+encodeURIComponent(p)+
       '&z='+encodeURIComponent(document.getElementById('z').value));
}</script></body></html>)RAW";

// ── JSON helpers (no heap alloc, no library) ──────────────────────────────────
static bool jFloat(const char* s, const char* k, float& v) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    p+=strlen(pat); while(*p&&*p!=':')p++; if(!*p)return false; p++;
    while(*p==' '||*p=='\t')p++;
    char*e; float r=strtof(p,&e); if(e==p)return false;
    v=r; return true;
}
static bool jInt(const char* s, const char* k, int& v) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    p+=strlen(pat); while(*p&&*p!=':')p++; if(!*p)return false; p++;
    while(*p==' '||*p=='\t')p++;
    char*e; long r=strtol(p,&e,10); if(e==p)return false;
    v=(int)r; return true;
}
static bool jStr(const char* s, const char* k, char* out, int maxLen) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    p+=strlen(pat); while(*p&&*p!=':')p++; if(!*p)return false; p++;
    while(*p==' ')p++; if(*p!='"')return false; p++;
    int i=0; while(*p&&*p!='"'&&i<maxLen-1)out[i++]=*p++;
    out[i]=0; return true;
}
// Nth value in a JSON array after key
static bool jArrF(const char* s, const char* k, int idx, float& v) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    while(*p&&*p!='[')p++; if(!*p)return false; p++;
    for(int i=0;i<idx;i++){while(*p&&*p!=','&&*p!=']')p++;if(*p==']')return false;p++;}
    while(*p==' ')p++;
    char*e; float r=strtof(p,&e); if(e==p)return false;
    v=r; return true;
}
static bool jArrI(const char* s, const char* k, int idx, int& v) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    while(*p&&*p!='[')p++; if(!*p)return false; p++;
    for(int i=0;i<idx;i++){while(*p&&*p!=','&&*p!=']')p++;if(*p==']')return false;p++;}
    while(*p==' ')p++;
    char*e; long r=strtol(p,&e,10); if(e==p)return false;
    v=(int)r; return true;
}
// Extract time string from array (ISO "YYYY-MM-DDTHH:00") — returns just "HH:MM"
static bool jArrTime(const char* s, const char* k, int idx, int& hourOut) {
    char pat[64]; snprintf(pat,sizeof(pat),"\"%s\"",k);
    const char* p=strstr(s,pat); if(!p) return false;
    while(*p&&*p!='[')p++; if(!*p)return false; p++;
    for(int i=0;i<idx;i++){
        // skip to next quoted string
        while(*p&&*p!='"')p++; if(!*p)return false; p++;
        while(*p&&*p!='"')p++; if(!*p)return false; p++;
        if(*p==',')p++;
    }
    while(*p==','||*p==' ')p++;
    if(*p!='"')return false; p++;
    // p now points to "YYYY-MM-DDTHH:00"
    const char* t=strchr(p,'T'); if(!t||t-p>16)return false;
    char hb[3]; hb[0]=t[1]; hb[1]=t[2]; hb[2]=0;
    hourOut=atoi(hb); return true;
}

// ── HTTP GET (plain port 80) ──────────────────────────────────────────────────
static bool wxGet(const char* host, const char* path, char* buf, size_t blen) {
    WiFiClient c;
    if(!c.connect(host,80)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-wx/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n", path, host);
    uint32_t dl=millis()+12000;
    // skip headers
    while(c.connected()&&millis()<dl){
        String ln=c.readStringUntil('\n');
        if(ln=="\r"||ln=="")break;
        if(ln.startsWith("HTTP/")&&ln.indexOf(" 200")<0){c.stop();return false;}
    }
    size_t pos=0;
    while((c.connected()||c.available())&&millis()<dl){
        int ch=c.read(); if(ch<0){ sysNetYield(); if(sysNetAborted()) break; delay(1); continue;}
        if(pos<blen-1)buf[pos++]=(char)ch;
    }
    buf[pos]=0; c.stop();
    return pos>0;
}

// ── HTTPS GET (port 443, setInsecure) - for open-meteo + geocoding ──────────
static bool wxGetS(const char* host, const char* path, char* buf, size_t blen) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(12);
    if(!c.connect(host,443)) return false;
    c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: pinball-wx/1\r\n"
             "Accept: application/json\r\nConnection: close\r\n\r\n",path,host);
    uint32_t dl=millis()+14000;
    while(c.connected()&&millis()<dl){
        String ln=c.readStringUntil('\n');
        if(ln=="\r"||ln=="")break;
        if(ln.startsWith("HTTP/")&&ln.indexOf(" 200")<0){c.stop();return false;}
    }
    size_t pos=0;
    while((c.connected()||c.available())&&millis()<dl){
        int ch=c.read(); if(ch<0){ sysNetYield(); if(sysNetAborted()) break; delay(1); continue;}
        if(pos<blen-1)buf[pos++]=(char)ch;
    }
    buf[pos]=0; c.stop();
    return pos>0;
}

// ── NVS helpers ───────────────────────────────────────────────────────────────
static void wxLoadCfg() {
    { Preferences p; if(p.begin(NVS_MINER,true)){
        p.getString("ssid",wxSsid,sizeof(wxSsid));
        p.getString("pass",wxPass,sizeof(wxPass));
        p.end();
    }}
    { Preferences p; if(p.begin(NVS_GAME,true)){
        p.getString("wx_zip",wxZip,sizeof(wxZip));
        wxLat=p.getFloat("wx_lat",0.0f);
        wxLon=p.getFloat("wx_lon",0.0f);
        p.getString("wx_city",wxCity,sizeof(wxCity));
        p.end();
    }}
    wxHasCoords=(wxLat!=0.0f||wxLon!=0.0f);
}
static void wxSaveCfg(const char* s, const char* p2, const char* z) {
    // Only update miner WiFi if user actually changed it
    if(s[0] && strcmp(s,wxSsid)!=0) {
        Preferences p; if(p.begin(NVS_MINER,false)){
            p.putString("ssid",s); p.end();
        }
        strncpy(wxSsid,s,sizeof(wxSsid)-1);
    }
    if(p2[0] && strcmp(p2,wxPass)!=0) {
        Preferences p; if(p.begin(NVS_MINER,false)){
            p.putString("pass",p2); p.end();
        }
        strncpy(wxPass,p2,sizeof(wxPass)-1);
    }
    { Preferences p; if(p.begin(NVS_GAME,false)){
        p.putString("wx_zip",z);
        p.putFloat("wx_lat",0.0f); p.putFloat("wx_lon",0.0f); // invalidate cache
        p.end();
    }}
    strncpy(wxZip,z,sizeof(wxZip)-1);
    wxLat=0; wxLon=0; wxHasCoords=false; wxCity[0]=0;
}

// ── Geocode ───────────────────────────────────────────────────────────────────
static bool wxGeocode() {
    static char buf[1024];
    char path[128];
    snprintf(path,sizeof(path),
             "/v1/search?name=%s&count=1&language=en&format=json",wxZip);
    if(!wxGetS("geocoding-api.open-meteo.com",path,buf,sizeof(buf)))return false;
    const char* r=strstr(buf,"\"results\""); if(!r)return false;
    float lat,lon; char city[40]="";
    if(!jFloat(r,"latitude",lat))return false;
    if(!jFloat(r,"longitude",lon))return false;
    jStr(r,"name",city,sizeof(city));
    char admin[32]=""; jStr(r,"admin1",admin,sizeof(admin));
    wxLat=lat; wxLon=lon;
    if(admin[0]) snprintf(wxCity,sizeof(wxCity),"%s, %s",city,admin);
    else strncpy(wxCity,city,sizeof(wxCity)-1);
    wxHasCoords=true;
    { Preferences p; if(p.begin(NVS_GAME,false)){
        p.putFloat("wx_lat",lat); p.putFloat("wx_lon",lon);
        p.putString("wx_city",wxCity); p.end();
    }}
    return true;
}

// ── Find current hour index in hourly array ───────────────────────────────────
static int wxCurrentHourIdx(const char* body) {
    // current_weather has a "time" field like "2025-05-28T14:00"
    char ct[24]=""; jStr(body,"time",ct,sizeof(ct));
    if(!ct[0])return 0;
    const char* T=strchr(ct,'T'); if(!T)return 0;
    int curHr=atoi(T+1);
    // Find in hourly time array
    const char* hp=strstr(body,"\"hourly\""); if(!hp)return 0;
    for(int i=0;i<48;i++){
        int h; if(jArrTime(hp,"time",i,h)&&h==curHr)return i;
    }
    return 0;
}

// ── Fetch weather data ────────────────────────────────────────────────────────
static bool wxFetch() {
    if(!wxHasCoords)return false;

    static char body[7168];
    memset(body,0,sizeof(body));

    char path[768];
    snprintf(path,sizeof(path),
        "/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
          "precipitation,weather_code,is_day,wind_speed_10m,wind_gusts_10m,"
          "wind_direction_10m,surface_pressure,uv_index,shortwave_radiation"
        "&hourly=temperature_2m,precipitation_probability,weather_code,"
          "is_day,surface_pressure"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
          "precipitation_sum,sunrise,sunset,precipitation_probability_max"
        "&temperature_unit=fahrenheit"
        "&wind_speed_unit=mph"
        "&precipitation_unit=mm"
        "&timezone=auto"
        "&forecast_days=7"
        "&forecast_hours=24",
        wxLat,wxLon);

    if(!wxGetS("api.open-meteo.com",path,body,sizeof(body)))return false;

    const char* cur=strstr(body,"\"current\""); if(!cur)return false;

    jFloat(cur,"temperature_2m",       wxd.tempF);
    jFloat(cur,"apparent_temperature", wxd.feelsF);
    jFloat(cur,"relative_humidity_2m", wxd.humidity);
    jFloat(cur,"wind_speed_10m",       wxd.windMph);
    jFloat(cur,"wind_gusts_10m",       wxd.gustMph);
    jFloat(cur,"surface_pressure",     wxd.pressureHpa);
    jFloat(cur,"shortwave_radiation",  wxd.solarWm2);
    jFloat(cur,"precipitation",        wxd.precipMm);
    jInt(cur,"weather_code",           wxd.wmoCode);
    jInt(cur,"is_day",                 wxd.isDay);
    jInt(cur,"uv_index",               wxd.uvIndex);
    int wd=0; jInt(cur,"wind_direction_10m",wd); wxd.windDir=wd;

    // Daily
    const char* daily=strstr(body,"\"daily\"");
    if(daily){
        static const char* DOW[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
        for(int i=0;i<7;i++){
            jArrF(daily,"temperature_2m_max",i,wxd.dayMaxF[i]);
            jArrF(daily,"temperature_2m_min",i,wxd.dayMinF[i]);
            int dc=0; jArrI(daily,"weather_code",i,dc); wxd.dayCode[i]=dc;
            jArrF(daily,"precipitation_sum",i,wxd.dayPrecipMm[i]);
            if(i==0) strncpy(wxd.dayLabel[i],"TOD",5);
            else if(i==1) strncpy(wxd.dayLabel[i],"TMR",5);
            else {
                // get time string for day label
                char ts[24]=""; // "YYYY-MM-DD"
                // approximate DOW: we don't have RTC, label D+N
                snprintf(wxd.dayLabel[i],5,"D+%d",i);
            }
        }
        // Today's totals from daily[0]
        wxd.maxF=wxd.dayMaxF[0]; wxd.minF=wxd.dayMinF[0];
        wxd.precipTotalMm=wxd.dayPrecipMm[0];
        // Sunrise/sunset strings
        char sr[32]="",ss[32]="";
        jArrTime(daily,"sunrise",0,wd); // reuse wd as temp
        // Just grab raw strings from array
        // Find "sunrise":["YYYY-MM-DDTHH:MM",...] and extract HH:MM
        const char* srp=strstr(daily,"\"sunrise\"");
        if(srp){ srp=strchr(srp,'['); if(srp){ srp++;
            while(*srp&&*srp!='"')srp++; if(*srp=='"'){srp++;
            const char* T=strchr(srp,'T');
            if(T&&T-srp<16){ strncpy(wxd.sunrise,T+1,5); wxd.sunrise[5]=0; }}}}
        const char* ssp=strstr(daily,"\"sunset\"");
        if(ssp){ ssp=strchr(ssp,'['); if(ssp){ ssp++;
            while(*ssp&&*ssp!='"')ssp++; if(*ssp=='"'){ssp++;
            const char* T=strchr(ssp,'T');
            if(T&&T-ssp<16){ strncpy(wxd.sunset,T+1,5); wxd.sunset[5]=0; }}}}
    }

    // Hourly (next 12h from current)
    const char* hrly=strstr(body,"\"hourly\"");
    if(hrly){
        int h0=wxCurrentHourIdx(body);
        for(int i=0;i<12;i++){
            jArrF(hrly,"temperature_2m",h0+i,wxd.hourTempF[i]);
            jArrF(hrly,"precipitation_probability",h0+i,wxd.hourPrecip[i]);
            int hc=0; jArrI(hrly,"weather_code",h0+i,hc); wxd.hourCode[i]=hc;
            int hd=1; jArrI(hrly,"is_day",h0+i,hd); wxd.hourIsDay[i]=hd;
            int hh=0; jArrTime(hrly,"time",h0+i,hh); wxd.hourLabel[i]=hh;
        }
        // Pressure trend: 6 values spaced ~4h apart starting from h0-20
        for(int i=0;i<6;i++){
            int idx=h0-20+i*4; if(idx<0)idx=0;
            jArrF(hrly,"surface_pressure",idx,wxd.pressureTrend[i]);
        }
        wxd.pressureTrend[5]=wxd.pressureHpa; // last point = current
    }

    // AQI from separate API
    static char aqiBuf[1024];
    char aqiPath[256];
    snprintf(aqiPath,sizeof(aqiPath),
        "/v1/air-quality?latitude=%.4f&longitude=%.4f"
        "&current=us_aqi,pm2_5&timezone=auto",
        wxLat,wxLon);
    if(wxGetS("air-quality-api.open-meteo.com",aqiPath,aqiBuf,sizeof(aqiBuf))){
        const char* ac=strstr(aqiBuf,"\"current\"");
        if(ac){ jInt(ac,"us_aqi",wxd.usAqi); jFloat(ac,"pm2_5",wxd.pm25); }
    }

    wxOk=true; wxFetchedAt=millis();
    return true;
}

// ── Colors ────────────────────────────────────────────────────────────────────
static uint16_t tempCol(float f){
    if(f>=95) return rgb565(220,40,0);
    if(f>=80) return rgb565(220,140,0);
    if(f>=65) return LIME;
    if(f>=50) return TEAL_BRIGHT;
    if(f>=32) return rgb565(100,180,255);
    return LIME_PALE;
}
static uint16_t aqiCol(int a){
    if(a<=50)  return LIME;
    if(a<=100) return rgb565(220,200,0);
    if(a<=150) return rgb565(220,120,0);
    return rgb565(200,40,40);
}
static const char* aqiLabel(int a){
    if(a<=50)  return "GOOD";
    if(a<=100) return "MODERATE";
    if(a<=150) return "SENS.GRPS";
    return "UNHEALTHY";
}
static const char* windDirStr(int d){
    static const char* D[]={"N","NE","E","SE","S","SW","W","NW"};
    return D[((d+22)/45)&7];
}

// ── WMO code → short label ────────────────────────────────────────────────────
static const char* wmoShort(int c){
    if(c==0)  return "CLEAR";
    if(c<=2)  return "P.CLOUDY";
    if(c==3)  return "OVERCAST";
    if(c<=48) return "FOGGY";
    if(c<=55) return "DRIZZLE";
    if(c<=67) return "RAIN";
    if(c<=77) return "SNOW";
    if(c<=82) return "SHOWERS";
    if(c<=99) return "TSTORM";
    return "UNKNOWN";
}

// ── Tiny weather icon (pixel art) ─────────────────────────────────────────────
// draws into sFb at (x,y), icon is 14×10 px
static void wxIcon(int x, int y, int code, bool day, int scale=1) {
    uint16_t S=day?LIME:TEAL;        // sun/moon
    uint16_t C=TEAL_BRIGHT;          // cloud
    uint16_t R=rgb565(80,160,255);   // rain
    uint16_t W=LIME_PALE;            // snow
    uint16_t T=PURPLE_LIGHT;         // thunder
    auto px=[&](int dx,int dy,uint16_t c){
        sFb->fillRect(x+dx*scale,y+dy*scale,scale,scale,c);
    };
    // Clear sky
    if(code==0){
        if(day){
            for(int dx=-2;dx<=2;dx++)for(int dy=-2;dy<=2;dy++)
                if(dx*dx+dy*dy<=5)px(dx+4,dy+5,S);
            px(4,0,S);px(4,9,S);px(0,5,S);px(8,5,S);
            px(1,1,S);px(7,1,S);px(1,8,S);px(7,8,S);
        } else {
            px(4,1,S);px(5,1,S);px(3,2,S);px(6,2,S);
            px(3,3,S);px(6,3,S);px(7,4,S);
            px(3,4,S);px(4,5,S);px(5,5,S);px(6,5,S);
            px(4,4,S);px(5,3,S);
        }
        return;
    }
    // Cloud base shared by code 1-3, rain, snow
    bool hasCloud=(code<=3||code>=45);
    if(hasCloud){
        uint16_t cc=(code==0)?TEAL_DARK:C;
        for(int dx=1;dx<=7;dx++)for(int dy=1;dy<=5;dy++){
            int rx=dx-4,ry=dy-3;
            if(rx*rx*3+ry*ry*5<=22)px(dx,dy,cc);
        }
        if(code<=2&&day){px(0,5,S);px(1,5,S);} // sun peek
    }
    if(code>=51&&code<=82){
        px(2,7,R);px(4,6,R);px(6,7,R);px(3,8,R);px(5,8,R);
    } else if(code>=71&&code<=77){
        px(2,7,W);px(4,6,W);px(6,7,W);px(3,8,W);px(5,8,W);
    } else if(code>=95){
        px(4,5,T);px(3,6,T);px(4,7,T);px(3,8,T);
        px(2,7,R);px(6,7,R);
    }
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
static void wStr(const char* s, int x, int y, int font, uint16_t c,
                 uint8_t datum=TL_DATUM){
    sFb->setTextColor(c); sFb->setTextDatum(datum);
    sFb->drawString(s,x,y,font);
}
static void wNum(float v, int x, int y, int font, uint16_t c,
                 int decimals=0, uint8_t datum=TL_DATUM){
    char buf[16];
    if(decimals==0) snprintf(buf,sizeof(buf),"%.0f",v);
    else            snprintf(buf,sizeof(buf),"%.*f",decimals,v);
    sFb->setTextColor(c); sFb->setTextDatum(datum);
    sFb->drawString(buf,x,y,font);
}
static void wBox(int x,int y,int w,int h,uint16_t border){
    sFb->drawRect(x,y,w,h,border);
}
static void wLabel(const char* s, int x, int y){
    sFb->setTextColor(TEAL_DARK); sFb->setTextDatum(TL_DATUM);
    sFb->drawString(s,x,y,1);
}

// ── Portal ────────────────────────────────────────────────────────────────────
static void wxStartPortal(){
    WiFi.disconnect(true); delay(100);
    WiFi.mode(WIFI_AP); WiFi.softAP(WX_AP); delay(100);
    wxDns=new DNSServer();
    wxDns->setErrorReplyCode(DNSReplyCode::NoError);
    wxDns->start(53,"*",WiFi.softAPIP());
    wxSrv=new WebServer(80);
    wxSrv->on("/",[]{
        String pg=FPSTR(WX_HTML);
        pg.replace("%SSID%",wxSsid);
        pg.replace("%PASS%",wxPass[0]?"••••••••":"");
        pg.replace("%ZIP%",wxZip);
        wxSrv->send(200,"text/html",pg);
    });
    wxSrv->onNotFound([]{
        wxSrv->sendHeader("Location","http://192.168.4.1/"); wxSrv->send(302,"","");
    });
    wxSrv->on("/wx",HTTP_POST,[]{
        String s=wxSrv->arg("s");
        String p=wxSrv->arg("p");
        String z=wxSrv->arg("z");
        wxSaveCfg(s.c_str(),p.c_str(),z.c_str());
        wxSrv->send(200,"text/plain","OK");
        delay(300);
        wxPhase=WX_WIFI; wxPhaseAt=millis();
        snprintf(wxStatus,sizeof(wxStatus),"CONNECTING");
    });
    wxSrv->begin();
    wxPhase=WX_SETUP; wxPhaseAt=millis();
}
static void wxStopPortal(){
    if(wxSrv){wxSrv->stop();delete wxSrv;wxSrv=nullptr;}
    if(wxDns){wxDns->stop();delete wxDns;wxDns=nullptr;}
    WiFi.softAPdisconnect(true);
}

// ── DRAW SCREENS ──────────────────────────────────────────────────────────────

static void wxDrawSetup(){
    sFb->fillSprite(TEAL_DEEP);
    sFb->setTextDatum(MC_DATUM);
    wStr("WEATHER SETUP",LW/2,18,2,LIME,MC_DATUM);
    wStr("Connect phone to WiFi:",LW/2,42,2,TEAL_BRIGHT,MC_DATUM);
    wStr(WX_AP,LW/2,58,2,LIME_PALE,MC_DATUM);
    wStr("then open  192.168.4.1",LW/2,74,2,TEAL_BRIGHT,MC_DATUM);
    wStr("Enter ZIP code for local weather.",LW/2,94,1,TEAL,MC_DATUM);
    wStr("WiFi creds pre-filled from miner.",LW/2,108,1,TEAL_DARK,MC_DATUM);
    uint32_t left=(WX_PORTAL_TIMEOUT-(millis()-wxPhaseAt))/1000;
    char tb[24]; snprintf(tb,sizeof(tb),"Timeout in %lus",(unsigned long)left);
    wStr(tb,LW/2,122,1,TEAL_DARK,MC_DATUM);
    int bw=(int)((float)(WX_PORTAL_TIMEOUT-(millis()-wxPhaseAt))/WX_PORTAL_TIMEOUT*(LW-20));
    if(bw<0)bw=0;
    sFb->drawRect(10,134,LW-20,8,TEAL_DARK);
    sFb->fillRect(10,134,bw,8,TEAL);
    sFb->pushSprite(0,0);
}

static void wxDrawLoading(){
    sFb->fillSprite(TEAL_DEEP);
    sFb->setTextDatum(MC_DATUM);
    wStr("WEATHER REPORT",LW/2,50,4,LIME,MC_DATUM);
    wStr(wxStatus,LW/2,90,2,TEAL_BRIGHT,MC_DATUM);
    uint32_t t=millis()/350;
    for(int i=0;i<5;i++){
        uint16_t c=((t%5)==(uint32_t)i)?LIME:TEAL_DARK;
        sFb->fillCircle(LW/2-32+i*16,115,5,c);
    }
    sFb->pushSprite(0,0);
}

static void wxDrawError(){
    sFb->fillSprite(TEAL_DEEP);
    sFb->setTextDatum(MC_DATUM);
    wStr("WEATHER ERROR",LW/2,60,2,PURPLE_BRIGHT,MC_DATUM);
    wStr(wxStatus,LW/2,85,1,TEAL_BRIGHT,MC_DATUM);
    uint32_t ret=(WX_RETRY_MS-(millis()-wxPhaseAt))/1000;
    char rb[24]; snprintf(rb,sizeof(rb),"Retry in %lus",(unsigned long)ret);
    wStr(rb,LW/2,105,1,TEAL,MC_DATUM);
    sFb->pushSprite(0,0);
}

// ── Main weather display (landscape 320×170) ──────────────────────────────────
//
//  Layout (inspired by reference image):
//
//  ┌──────────────────────┬─────────────────────┬──────────────────┐  y=0
//  │  CURRENT  (col 0)    │  HOURLY STRIP        │  DAILY (col 2)   │
//  │  big temp            │  next 6h             │  7-day forecast  │
//  │  condition           │  tiny icons + temps  │                  │
//  │  humidity/wind       │                      │                  │
//  │  pressure/UV/solar   │  AQI panel           │                  │
//  └──────────────────────┴─────────────────────┴──────────────────┘  y=170
//
//  Column widths: 114 | 118 | 88  = 320
//
static void wxDrawDisplay(){
    sFb->fillSprite(TEAL_DEEP);

    // ── Header bar ─────────────────────────────────────────────────────────
    sFb->fillRect(0,0,LW,14,TEAL_DARK);
    char hdr[64];
    uint32_t ageSec=(millis()-wxFetchedAt)/1000;
    snprintf(hdr,sizeof(hdr),"%s   updated %lus ago",
             wxCity[0]?wxCity:wxZip, (unsigned long)ageSec);
    wStr(hdr,4,3,1,LIME,TL_DATUM);
    // Refresh timer on right
    uint32_t nextMin=(WX_REFRESH_MS-(millis()-wxFetchedAt))/60000;
    char rtb[16]; snprintf(rtb,sizeof(rtb),"refresh %ldm",(long)nextMin);
    wStr(rtb,LW-4,3,1,TEAL,TR_DATUM);

    sFb->drawFastVLine(114,14,LH-14,TEAL_DARK);
    sFb->drawFastVLine(232,14,LH-14,TEAL_DARK);

    // ══════════════════════════════════════════════════════════════════════
    // COL 0  (x=0..113)  CURRENT CONDITIONS
    // ══════════════════════════════════════════════════════════════════════
    int cx=0, cy=16;

    // Big temperature
    char tb[8]; snprintf(tb,sizeof(tb),"%.0f°",wxd.tempF);
    sFb->setTextColor(tempCol(wxd.tempF)); sFb->setTextDatum(TL_DATUM);
    sFb->drawString(tb,cx+4,cy,6);

    // Icon (2× scale)
    wxIcon(cx+76,cy+2,wxd.wmoCode,wxd.isDay,2);

    // Feels like + condition
    char fl[16]; snprintf(fl,sizeof(fl),"feels %.0f°",wxd.feelsF);
    wStr(fl,cx+4,cy+42,1,TEAL_BRIGHT,TL_DATUM);
    wStr(wmoShort(wxd.wmoCode),cx+4,cy+54,2,TEAL_BRIGHT,TL_DATUM);

    sFb->drawFastHLine(cx+2,cy+70,108,TEAL_DARK);

    // Details grid (2 columns)
    int dy=cy+75;
    auto row=[&](const char* lbl, const char* val, uint16_t vc, int col){
        int bx=cx+2+col*54;
        wStr(lbl,bx,dy,1,TEAL_DARK,TL_DATUM);
        wStr(val,bx+52,dy,1,vc,TR_DATUM);
        dy+=12;
    };
    int savedDy=dy;
    // Left column
    char wbuf[16];
    snprintf(wbuf,sizeof(wbuf),"%.0f%%",wxd.humidity);  wStr("HUM",cx+2,dy,1,TEAL_DARK); wStr(wbuf,cx+54,dy,1,TEAL_BRIGHT,TR_DATUM); dy+=12;
    snprintf(wbuf,sizeof(wbuf),"%.0f",wxd.pressureHpa); wStr("HPA",cx+2,dy,1,TEAL_DARK); wStr(wbuf,cx+54,dy,1,TEAL_BRIGHT,TR_DATUM); dy+=12;
    snprintf(wbuf,sizeof(wbuf),"UV %d",wxd.uvIndex);    wStr(wbuf,cx+4,dy,1,wxd.uvIndex>=8?PURPLE_LIGHT:TEAL_BRIGHT); dy+=12;

    // Right column (reset dy)
    dy=savedDy;
    snprintf(wbuf,sizeof(wbuf),"%.0f %s",wxd.windMph,windDirStr(wxd.windDir));
    wStr("WIND",cx+58,dy,1,TEAL_DARK); wStr(wbuf,cx+112,dy,1,LIME,TR_DATUM); dy+=12;
    snprintf(wbuf,sizeof(wbuf),"G %.0f",wxd.gustMph);
    wStr("GUST",cx+58,dy,1,TEAL_DARK); wStr(wbuf,cx+112,dy,1,TEAL_BRIGHT,TR_DATUM); dy+=12;
    snprintf(wbuf,sizeof(wbuf),"%.0f W",wxd.solarWm2);
    wStr("SOL",cx+58,dy,1,TEAL_DARK); wStr(wbuf,cx+112,dy,1,LIME_PALE,TR_DATUM); dy+=12;

    // Sunrise/Sunset
    sFb->drawFastHLine(cx+2,dy+2,108,TEAL_DARK); dy+=6;
    char ssbuf[24]; snprintf(ssbuf,sizeof(ssbuf),"\x18%s  \x19%s",wxd.sunrise,wxd.sunset);
    wStr(ssbuf,cx+4,dy,1,LIME_MED,TL_DATUM);

    // ══════════════════════════════════════════════════════════════════════
    // COL 1  (x=115..231)  HOURLY + AQI
    // ══════════════════════════════════════════════════════════════════════
    int hx=116;

    // Section label
    wStr("NEXT 12 HOURS",hx+2,16,1,TEAL,TL_DATUM);

    // Hourly strip — 12 columns each 9px wide
    int colW=9, hy=26;
    for(int i=0;i<12;i++){
        int bx=hx+2+i*colW;
        // Hour label
        char hlb[4]; snprintf(hlb,sizeof(hlb),"%d",wxd.hourLabel[i]%12);
        if(wxd.hourLabel[i]%12==0) strncpy(hlb,"12",4);
        wStr(hlb,bx+colW/2,hy,1,TEAL_DARK,TC_DATUM);
        // Tiny icon (1× scale)
        wxIcon(bx+1,hy+10,wxd.hourCode[i],wxd.hourIsDay[i],1);
        // Temp
        char ht[5]; snprintf(ht,sizeof(ht),"%d",(int)roundf(wxd.hourTempF[i]));
        sFb->setTextColor(tempCol(wxd.hourTempF[i])); sFb->setTextDatum(TC_DATUM);
        sFb->drawString(ht,bx+colW/2,hy+22,1);
        // Precip probability bar
        int ph=(int)(wxd.hourPrecip[i]/100.0f*16.0f);
        if(ph>0){
            sFb->fillRect(bx+3,hy+34-ph,3,ph,
                          wxd.hourPrecip[i]>60?rgb565(80,160,255):TEAL_DARK);
        }
    }

    // Pressure trend mini-graph
    sFb->drawFastHLine(hx+2,72,114,TEAL_DARK);
    wStr("PRESSURE TREND",hx+2,75,1,TEAL,TL_DATUM);

    // Find min/max of trend
    float pMin=wxd.pressureTrend[0],pMax=wxd.pressureTrend[0];
    for(int i=1;i<6;i++){
        if(wxd.pressureTrend[i]<pMin)pMin=wxd.pressureTrend[i];
        if(wxd.pressureTrend[i]>pMax)pMax=wxd.pressureTrend[i];
    }
    if(pMax-pMin<1.0f){pMin-=0.5f;pMax+=0.5f;}
    // Draw graph (84px wide, 28px tall, at hx+30, y=84)
    int gx=hx+30, gy=84, gw=84, gh=28;
    sFb->drawRect(gx,gy,gw,gh,TEAL_DARK);
    // Grid lines
    sFb->drawFastHLine(gx,gy+gh/2,gw,TEAL_DEEP);
    // Line graph
    for(int i=0;i<5;i++){
        float y1=gy+gh-1-(wxd.pressureTrend[i]-pMin)/(pMax-pMin)*(gh-2);
        float y2=gy+gh-1-(wxd.pressureTrend[i+1]-pMin)/(pMax-pMin)*(gh-2);
        float x1=gx+i*(gw-1)/5;
        float x2=gx+(i+1)*(gw-1)/5;
        // Draw line segment
        int dx=(int)(x2-x1), dy=(int)(y2-y1);
        int steps=abs(dx)>abs(dy)?abs(dx):abs(dy); if(steps==0)steps=1;
        for(int s=0;s<=steps;s++){
            int px2=(int)(x1+dx*s/steps);
            int py2=(int)(y1+dy*s/steps);
            sFb->drawPixel(px2,py2,LIME);
        }
    }
    // Current value and trend arrow
    float pDelta=wxd.pressureTrend[5]-wxd.pressureTrend[0];
    char pbuf[16]; snprintf(pbuf,sizeof(pbuf),"%.1f",wxd.pressureHpa);
    wStr(pbuf,hx+2,84+10,1,TEAL_BRIGHT,TL_DATUM);
    wStr(pDelta>0.3f?"^":pDelta<-0.3f?"v":"=",hx+2,84+22,1,
         pDelta>0.3f?LIME:pDelta<-0.3f?PURPLE_BRIGHT:TEAL_BRIGHT,TL_DATUM);
    wStr("hPa",hx+2,84,1,TEAL_DARK,TL_DATUM);

    // AQI panel
    sFb->drawFastHLine(hx+2,118,114,TEAL_DARK);
    wStr("AIR QUALITY",hx+2,121,1,TEAL,TL_DATUM);
    int aqi=wxd.usAqi;
    uint16_t ac=aqiCol(aqi);
    char aqb[8]; snprintf(aqb,sizeof(aqb),"%d",aqi);
    wStr(aqb,hx+40,130,4,ac,TC_DATUM);
    wStr(aqiLabel(aqi),hx+40,152,1,ac,TC_DATUM);
    char pmb[16]; snprintf(pmb,sizeof(pmb),"PM2.5: %.1f",wxd.pm25);
    wStr(pmb,hx+40,162,1,TEAL_DARK,TC_DATUM);

    // ══════════════════════════════════════════════════════════════════════
    // COL 2  (x=233..319)  7-DAY FORECAST
    // ══════════════════════════════════════════════════════════════════════
    int dx2=234;
    wStr("7-DAY",dx2+4,16,1,TEAL,TL_DATUM);

    int rowH=(LH-26)/7;
    for(int i=0;i<7;i++){
        int ry=26+i*rowH;
        // Day label
        uint16_t dlc=(i==0)?LIME:TEAL_BRIGHT;
        wStr(wxd.dayLabel[i],dx2+2,ry+rowH/2-4,1,dlc,TL_DATUM);
        // Icon
        wxIcon(dx2+20,ry,wxd.dayCode[i],true,1);
        // Hi
        char hib[6]; snprintf(hib,sizeof(hib),"%.0f",wxd.dayMaxF[i]);
        sFb->setTextColor(tempCol(wxd.dayMaxF[i]));
        sFb->setTextDatum(TR_DATUM);
        sFb->drawString(hib,dx2+86,ry+rowH/2-8,1);
        // Lo (muted)
        char lob[6]; snprintf(lob,sizeof(lob),"%.0f",wxd.dayMinF[i]);
        sFb->setTextColor(TEAL);
        sFb->drawString(lob,dx2+86,ry+rowH/2+2,1);
        // Precip if meaningful
        if(wxd.dayPrecipMm[i]>0.5f){
            char ppb[8]; snprintf(ppb,sizeof(ppb),"%.0fmm",wxd.dayPrecipMm[i]);
            wStr(ppb,dx2+36,ry+rowH/2-2,1,rgb565(80,160,255),TL_DATUM);
        }
        if(i<6) sFb->drawFastHLine(dx2+2,ry+rowH-1,82,TEAL_DARK);
    }

    // ── Reset zip overlay (left long-press in WX_DISPLAY) ─────────────────
    if(wxResetArmed){
        uint32_t held=millis()-wxResetHoldStart;
        int barW=(int)((float)(held-1500)/1500.0f*(LW-20));
        if(barW>LW-20) barW=LW-20; if(barW<0) barW=0;
        sFb->fillRect(60,LH/2-20,LW-120,44,PURPLE_DARK);
        sFb->drawRect(60,LH/2-20,LW-120,44,PURPLE_BRIGHT);
        sFb->setTextDatum(MC_DATUM);
        sFb->setTextColor(PURPLE_BRIGHT);
        sFb->drawString("RESET ZIP?",LW/2,LH/2-8,2);
        sFb->setTextColor(PURPLE_LIGHT);
        sFb->drawString("hold to confirm",LW/2,LH/2+6,1);
        sFb->drawRect(65,LH/2+16,LW-130,6,PURPLE);
        if(barW>10) sFb->fillRect(65,LH/2+16,barW-10,6,PURPLE_BRIGHT);
    }
    sFb->pushSprite(0,0);
}

// ── Public API ────────────────────────────────────────────────────────────────
void weatherEnter(){
    wxOk=false; wxd={};
    wxLoadCfg();

    // Acquire landscape sprite (same pattern as miner)
    fbReady=false;
    if(sFb==nullptr){
        sSavedRotation=tft.getRotation();
        tft.setRotation(3);   // landscape - matches miner rotation
        tft.fillScreen(TEAL_DEEP);
        fb.deleteSprite();
        sFb=&fb;
        sFb->setColorDepth(16);
        if(!sFb->createSprite(LW,LH)){
            sFb->setColorDepth(8);
            sFb->createSprite(LW,LH);
        }
    }

    if(wxZip[0]=='\0'||wxSsid[0]=='\0'){
        wxStartPortal();
    } else {
        wxPhase=WX_WIFI; wxPhaseAt=millis();
        snprintf(wxStatus,sizeof(wxStatus),"CONNECTING: %s",wxSsid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(wxSsid,wxPass);
    }
}

void weatherShutdown(){
    if(wxPhase==WX_SETUP) wxStopPortal();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Restore portrait sprite (same pattern as miner)
    if(sFb){
        sFb->deleteSprite();
        sFb=nullptr;
        tft.setRotation(sSavedRotation);
        tft.fillScreen(TEAL_DEEP);
        fb.setColorDepth(16);
        if(!fb.createSprite(170,320)){
            fb.setColorDepth(8);
            fb.createSprite(170,320);
        }
    }
    fbReady=true;
    delay(50);
}

void weatherTick(){
    uint32_t now=millis();
    switch(wxPhase){

        case WX_SETUP:
            if(wxDns) wxDns->processNextRequest();
            if(wxSrv) wxSrv->handleClient();
            if(now-wxPhaseAt>WX_PORTAL_TIMEOUT){
                wxStopPortal();
                if(wxZip[0]&&wxSsid[0]){
                    wxPhase=WX_WIFI; wxPhaseAt=now;
                    WiFi.mode(WIFI_STA); WiFi.begin(wxSsid,wxPass);
                } else {
                    wxPhase=WX_ERROR; wxPhaseAt=now;
                    snprintf(wxStatus,sizeof(wxStatus),"NO CONFIG - DOUBLE CLICK TO EXIT");
                }
            }
            break;

        case WX_WIFI:
            if(WiFi.status()==WL_CONNECTED){
                sysclockSyncNTP(1200);   // brief, opportunistic; no-op if already set
                snprintf(wxStatus,sizeof(wxStatus),"FETCHING WEATHER DATA");
                wxPhase=WX_FETCH; wxPhaseAt=now;
            } else if(now-wxPhaseAt>WX_WIFI_MS){
                wxPhase=WX_ERROR; wxPhaseAt=now;
                snprintf(wxStatus,sizeof(wxStatus),"WIFI TIMEOUT - CHECK CREDENTIALS");
            }
            break;

        case WX_FETCH:
            if(!wxHasCoords){
                snprintf(wxStatus,sizeof(wxStatus),"LOCATING %s",wxZip);
                if(!wxGeocode()){
                    wxPhase=WX_ERROR; wxPhaseAt=now;
                    snprintf(wxStatus,sizeof(wxStatus),"GEOCODE FAILED - CHECK ZIP");
                    break;
                }
            }
            snprintf(wxStatus,sizeof(wxStatus),"DOWNLOADING FORECAST");
            if(!wxFetch()){
                wxPhase=WX_ERROR; wxPhaseAt=now;
                snprintf(wxStatus,sizeof(wxStatus),"FETCH FAILED - CHECK WIFI");
                break;
            }
            wxPhase=WX_DISPLAY; wxPhaseAt=now;
            break;

        case WX_DISPLAY:
            // Long-press LEFT = reset zip code (purple bar + hold-to-confirm)
            // Does NOT touch the btcminer NVS namespace
            if(btnL && !btnR){
                if(wxResetHoldStart==0) wxResetHoldStart=now;
                uint32_t held=now-wxResetHoldStart;
                if(!wxResetArmed && held>=1500) wxResetArmed=true;
                if(wxResetArmed && !wxResetPending && held>=3000){
                    wxResetPending=true;
                    // Wipe only the pinball namespace zip/coords — miner untouched
                    { Preferences p; if(p.begin(NVS_GAME,false)){
                        p.remove("wx_zip"); p.remove("wx_lat");
                        p.remove("wx_lon"); p.remove("wx_city"); p.end();
                    }}
                    wxZip[0]=0; wxLat=0; wxLon=0; wxCity[0]=0; wxHasCoords=false;
                    wxResetHoldStart=0; wxResetArmed=false; wxResetPending=false;
                    wxStopPortal();
                    wxStartPortal();
                }
            } else {
                wxResetHoldStart=0; wxResetArmed=false; wxResetPending=false;
            }
            // Auto-refresh every 10 minutes
            if(now-wxFetchedAt>=WX_REFRESH_MS){
                snprintf(wxStatus,sizeof(wxStatus),"REFRESHING");
                wxPhase=WX_FETCH; wxPhaseAt=now;
            }
            break;

        case WX_ERROR:
            if(now-wxPhaseAt>=WX_RETRY_MS){
                wxPhase=WX_WIFI; wxPhaseAt=now;
                snprintf(wxStatus,sizeof(wxStatus),"RECONNECTING");
                WiFi.mode(WIFI_STA); WiFi.begin(wxSsid,wxPass);
            }
            break;
    }
}

void weatherDraw(){
    if(!sFb) return;
    switch(wxPhase){
        case WX_SETUP:   wxDrawSetup();   break;
        case WX_WIFI:    wxDrawLoading(); break;
        case WX_FETCH:   wxDrawLoading(); break;
        case WX_DISPLAY: wxDrawDisplay(); break;
        case WX_ERROR:   wxDrawError();   break;
    }
}
