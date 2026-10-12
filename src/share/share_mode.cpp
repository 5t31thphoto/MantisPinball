// share_mode.cpp - "Pinball Sharing" AP that serves a playable HTML pinball.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "share_mode.h"
#include "../theme_colors.h"

extern TFT_eSprite fb;
extern bool btnL, btnR, btnL_prev, btnR_prev;
static constexpr int W = 170;
static constexpr int H = 320;

static constexpr const char* SH_AP = "pinball-sharing";
static WebServer* shSrv = nullptr;
static DNSServer* shDns = nullptr;
static bool       shOn  = false;
static uint32_t   shHits = 0;   // count of page loads (players who joined)
static uint32_t   shAllTime = 0;      // best score EVER posted by any player
                                      // on any device that joined this server
                                      // (persisted to flash, NVS "share")
static uint32_t   shSessionBest = 0;  // best score reported this session
static bool       shNewRecord = false;// a record fell this session (banner)

static void shLoadAllTime() {
    Preferences pr;
    if (pr.begin("share", true)) { shAllTime = pr.getULong("alltime", 0); pr.end(); }
}
static void shSaveAllTime() {
    Preferences pr;
    if (pr.begin("share", false)) { pr.putULong("alltime", shAllTime); pr.end(); }
}

// ── The hosted game ───────────────────────────────────────────────────────────
// A self-contained HTML5 canvas pinball, themed like Pinball Blaster. Left half
// of the screen = left flipper (tap/hold), right half = right flipper. Physics:
// gravity, wall bounces, bumpers, two flippers, a drain, score + ball count.
// Kept compact but complete; no external assets so it loads instantly over AP.
static const char SH_HTML[] PROGMEM = R"RAW(<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>
<title>PINBALL BLASTER</title>
<style>
html,body{margin:0;height:100%;background:#0a0020;overflow:hidden;
  font-family:monospace;color:#b9f;-webkit-user-select:none;user-select:none;touch-action:none}
#wrap{display:flex;flex-direction:column;align-items:center;height:100%}
#hud{width:100%;max-width:420px;display:flex;justify-content:space-between;
  padding:6px 10px;box-sizing:border-box;font-size:15px;color:#9d6cff}
#hud b{color:#39e0c8}
canvas{background:#120033;border:2px solid #6a2cff;border-radius:8px;
  box-shadow:0 0 18px #6a2cff66;touch-action:none}
#msg{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);
  text-align:center;color:#39e0c8;font-size:20px;text-shadow:0 0 8px #39e0c8;
  pointer-events:none}
#hint{font-size:12px;color:#7a3cff;margin-top:6px;text-align:center}
a{color:#39e0c8}
</style></head><body>
<div id='wrap'>
<div id='hud'><div>SCORE <b id='sc'>0</b></div><div>BEST <b id='hi'>0</b></div><div>BALLS <b id='bl'>3</b></div></div>
<canvas id='c' width='320' height='460'></canvas>
<div id='hint'>LEFT half = left flipper &nbsp;|&nbsp; RIGHT half = right flipper<br>tap to launch &middot; hosted by a Pinball Blaster device</div>
</div>
<div id='msg'>TAP TO LAUNCH</div>
<script>
var cv=document.getElementById('c'),x=cv.getContext('2d');
var Wd=cv.width,Hd=cv.height;
var sc=document.getElementById('sc'),bl=document.getElementById('bl'),msg=document.getElementById('msg');
var score=0,balls=3,state='ready';   // ready|play|over
// ball
var b={x:Wd-14,y:250,vx:0,vy:0,r:7};
// A pinball table is a SLOPED plane (~6-7 deg), not a vertical wall. The ball
// only feels the component of gravity along the slope (g*sin(theta)), which is
// roughly an eighth of full gravity - that's why a real ball can travel most of
// the way up the table and drift gently back toward the flippers. Simulating
// full straight-up gravity made the ball feel like it was launched into the air
// and slammed back down. This is the slope component.
var G=0.12;          // gravity along the table slope (not full vertical g)
var REST=0.72;       // wall/flipper restitution (<1 so bounces LOSE energy)
var DRAG=0.988;      // air/rolling friction: ball keeps 98.8% of speed per frame
var ROLL=0.99;       // extra rolling resistance near the bottom/flippers
// Continuous friction is what keeps a real table stable: gravity + flipper kicks
// add energy, drag bleeds it off every frame, and bounces under 1.0 lose more.
// The ball settles into a natural top speed where the losses balance the gains -
// no clamp needed.
// flippers: pivot near bottom, swing up when active
var fl={x:103,y:Hd-34,len:62,ang:0.5,rest:0.5,up:-0.35,a:false,dir:1};
var fr={x:Wd-103,y:Hd-34,len:62,ang:Math.PI-0.5,rest:Math.PI-0.5,up:Math.PI+0.35,a:false,dir:-1};
// bumpers
var bump=[{x:80,y:130,r:18},{x:Wd-80,y:130,r:18},{x:Wd/2,y:90,r:20},
          {x:60,y:230,r:15},{x:Wd-60,y:230,r:15}];
// destructible TARGETS (like Pinball Blaster): break on hit, and when the
// board is cleared they respawn in the NEXT pattern + a clear bonus.
var tW=34,tH=12,targets=[],patIdx=0;
function pRow(y,n){var a=[],gap=(Wd-n*tW)/(n+1);
  for(var i=0;i<n;i++)a.push({x:gap+i*(tW+gap),y:y});return a;}
var patterns=[
  function(){return pRow(36,4).concat(pRow(54,3),pRow(72,4));},              // grid
  function(){return pRow(30,1).concat(pRow(46,2),pRow(62,3),pRow(78,2),pRow(94,1));}, // diamond
  function(){var a=[];for(var c=0;c<3;c++)for(var r=0;r<4;r++)
      a.push({x:44+c*(Wd-88-tW)/2,y:26+r*17});return a;},                    // columns
  function(){var a=[];for(var r=0;r<5;r++)
      a.push({x:(r%2? Wd*0.62 : Wd*0.18),y:28+r*16},
             {x:(r%2? Wd*0.18 : Wd*0.62),y:28+r*16});return a;}              // zigzag
];
function spawnTargets(){
  var pts=patterns[patIdx%patterns.length]();patIdx++;
  targets=[];for(var i=0;i<pts.length;i++)targets.push({x:pts[i].x,y:pts[i].y,on:true});
}
spawnTargets();
// all-time best (kept on the host device flash, shared by every player ever)
var best=0,hiEl=document.getElementById('hi');
function setBest(v){best=v|0;hiEl.textContent=best;}
fetch('/hi').then(function(r){return r.text();}).then(function(t){setBest(t);}).catch(function(){});
function report(){
  fetch('/score?v='+score).then(function(r){return r.text();})
   .then(function(t){setBest(t);}).catch(function(){});
}
// angled inlane walls: taper from the side walls down to just outside each
// flipper pivot, so a ball coming down the sides is funneled onto the flippers
// instead of draining straight past them. Each is a line segment {ax,ay,bx,by}.
var walls=[
  {ax:0,    ay:300, bx:100,    by:Hd-30},   // left  funnel
  {ax:Wd,   ay:300, bx:Wd-100, by:Hd-30}    // right funnel
];
function reset(full){
  if(full){score=0;balls=3;patIdx=Math.floor(Math.random()*4);spawnTargets();}
  b.x=Wd-14;b.y=250;b.vx=0;b.vy=0;state='ready';
  msg.textContent=balls>0?'TAP TO LAUNCH':'GAME OVER - TAP';msg.style.display='block';
  sc.textContent=score;bl.textContent=balls;
}
function launch(){
  if(state==='ready'){ b.vx=-1.5-Math.random()*1.5; b.vy=-8.5; state='play'; msg.style.display='none'; }
  else if(state==='over'){ reset(true); }
}
// input: which side held
function setFlip(side,on){ if(side==='L')fl.a=on; else fr.a=on; }
function pointerHandler(e,on){
  e.preventDefault();
  var rect=cv.getBoundingClientRect();
  var pts=e.changedTouches?e.changedTouches:[e];
  for(var i=0;i<pts.length;i++){
    var px=pts[i].clientX-rect.left;
    if(px<rect.width/2)setFlip('L',on);else setFlip('R',on);
  }
  if(on){ if(state!=='play')launch(); }
}
cv.addEventListener('touchstart',function(e){pointerHandler(e,true);},{passive:false});
cv.addEventListener('touchend',function(e){pointerHandler(e,false);},{passive:false});
cv.addEventListener('mousedown',function(e){pointerHandler(e,true);});
cv.addEventListener('mouseup',function(e){pointerHandler(e,false);});
window.addEventListener('keydown',function(e){if(e.key==='ArrowLeft')fl.a=true;if(e.key==='ArrowRight')fr.a=true;if(e.key===' ')launch();});
window.addEventListener('keyup',function(e){if(e.key==='ArrowLeft')fl.a=false;if(e.key==='ArrowRight')fr.a=false;});
function flipTip(f){return {x:f.x+Math.cos(f.ang)*f.len, y:f.y+Math.sin(f.ang)*f.len};}
function segDist(px,py,ax,ay,bx,by){
  var dx=bx-ax,dy=by-ay,l2=dx*dx+dy*dy;
  var t=l2?((px-ax)*dx+(py-ay)*dy)/l2:0; t=Math.max(0,Math.min(1,t));
  var cx=ax+t*dx,cy=ay+t*dy; return {d:Math.hypot(px-cx,py-cy),cx:cx,cy:cy};
}
function stepFlip(f){
  var target=f.a?f.up:f.rest;
  f.ang+=(target-f.ang)*0.4;
}
function collideFlip(f){
  var tip=flipTip(f);
  var s=segDist(b.x,b.y,f.x,f.y,tip.x,tip.y);
  if(s.d<b.r+5){
    var nx=(b.x-s.cx)/(s.d||1),ny=(b.y-s.cy)/(s.d||1);
    b.x=s.cx+nx*(b.r+5);b.y=s.cy+ny*(b.r+5);
    // Reflect off the flipper face, then add the kick. An actively-swinging
    // flipper hits HARD - a clean hit sends the ball all the way up to bounce
    // off the top.
    var vn2=b.vx*nx+b.vy*ny;
    if(vn2<0){ b.vx-=(1+REST)*vn2*nx; b.vy-=(1+REST)*vn2*ny; }
    if(f.a){
        b.vx+=nx*11.0 + f.dir*2.0;
        b.vy+=ny*11.0 - 2.0;
    } else {
        b.vx+=nx*1.6; b.vy+=ny*1.6 - 0.4;     // passive flipper: gentle bump
    }
  }
}
function tick(){
  if(state==='play'){
    b.vy+=G;
    // Air friction every frame: the ball always loses a little speed, which is
    // what balances the energy gravity and flippers add (real tables do this).
    b.vx*=DRAG; b.vy*=DRAG;
    // A touch more rolling resistance in the lower playfield near the flippers.
    if(b.y>Hd*0.6){ b.vx*=ROLL; }
    b.x+=b.vx;b.y+=b.vy;
    // walls (restitution < 1 so each bounce loses energy)
    if(b.x<b.r){b.x=b.r;b.vx=-b.vx*REST;}
    if(b.x>Wd-b.r){b.x=Wd-b.r;b.vx=-b.vx*REST;}
    if(b.y<b.r){b.y=b.r;b.vy=-b.vy*REST;}
    // angled inlane walls: bounce the ball back toward the flippers
    for(var w=0;w<walls.length;w++){var wl=walls[w];
      var sw=segDist(b.x,b.y,wl.ax,wl.ay,wl.bx,wl.by);
      if(sw.d<b.r+3){
        var nx=(b.x-sw.cx)/(sw.d||1),ny=(b.y-sw.cy)/(sw.d||1);
        b.x=sw.cx+nx*(b.r+3);b.y=sw.cy+ny*(b.r+3);
        var vn=b.vx*nx+b.vy*ny;
        if(vn<0){b.vx-=(1+REST)*vn*nx;b.vy-=(1+REST)*vn*ny;}
      }
    }
    // bumpers: a lively pop along the normal, but restitution-based so it
    // can't pump the ball to infinite speed.
    for(var i=0;i<bump.length;i++){var p=bump[i];var d=Math.hypot(b.x-p.x,b.y-p.y);
      if(d<b.r+p.r){var nx=(b.x-p.x)/(d||1),ny=(b.y-p.y)/(d||1);
        b.x=p.x+nx*(b.r+p.r);b.y=p.y+ny*(b.r+p.r);
        var vn=b.vx*nx+b.vy*ny;
        if(vn<0){b.vx-=(1+REST)*vn*nx;b.vy-=(1+REST)*vn*ny;}
        b.vx+=nx*3.0;b.vy+=ny*3.0;   // bumper impulse (bled off by drag)
        score+=100;sc.textContent=score;}}
    // destructible targets: circle-vs-rect, reflect off the nearest face
    var alive=0;
    for(var t=0;t<targets.length;t++){var tg=targets[t];if(!tg.on)continue;alive++;
      var cx2=Math.max(tg.x,Math.min(b.x,tg.x+tW));
      var cy2=Math.max(tg.y,Math.min(b.y,tg.y+tH));
      var dx2=b.x-cx2,dy2=b.y-cy2;
      if(dx2*dx2+dy2*dy2<b.r*b.r){
        tg.on=false;alive--;score+=150;sc.textContent=score;
        if(Math.abs(dx2)>Math.abs(dy2)){b.vx=Math.abs(b.vx)*(dx2>0?1:-1)*1;}
        else{b.vy=Math.abs(b.vy)*(dy2>0?1:-1)*1;}
        b.vx*=1.02;b.vy*=1.02;
      }}
    if(alive===0&&targets.length){score+=1000;sc.textContent=score;spawnTargets();}
    stepFlip(fl);stepFlip(fr);collideFlip(fl);collideFlip(fr);
    // drain
    if(b.y>Hd+20){balls--;bl.textContent=balls;
      if(balls<=0){state='over';msg.textContent='GAME OVER - TAP';msg.style.display='block';report();}
      else reset(false);}
  } else { stepFlip(fl);stepFlip(fr); }
  draw();requestAnimationFrame(tick);
}
function draw(){
  x.clearRect(0,0,Wd,Hd);
  // angled inlane walls
  x.strokeStyle='#6a2cff';x.lineWidth=4;x.lineCap='round';
  for(var w=0;w<walls.length;w++){var wl=walls[w];
    x.beginPath();x.moveTo(wl.ax,wl.ay);x.lineTo(wl.bx,wl.by);x.stroke();}
  // bumpers
  for(var i=0;i<bump.length;i++){var p=bump[i];
    x.beginPath();x.arc(p.x,p.y,p.r,0,7);x.fillStyle='#6a2cff';x.fill();
    x.beginPath();x.arc(p.x,p.y,p.r*0.5,0,7);x.fillStyle='#39e0c8';x.fill();}
  // targets
  for(var t=0;t<targets.length;t++){var tg=targets[t];if(!tg.on)continue;
    x.fillStyle='#39e0c8';x.fillRect(tg.x,tg.y,tW,tH);
    x.strokeStyle='#6a2cff';x.lineWidth=2;x.strokeRect(tg.x,tg.y,tW,tH);}
  // flippers
  function dF(f){var t=flipTip(f);x.strokeStyle='#a6ff00';x.lineWidth=9;x.lineCap='round';
    x.beginPath();x.moveTo(f.x,f.y);x.lineTo(t.x,t.y);x.stroke();}
  dF(fl);dF(fr);
  // ball
  x.beginPath();x.arc(b.x,b.y,b.r,0,7);x.fillStyle='#e8e8ff';x.fill();
}
reset(true);tick();
</script></body></html>)RAW";

// ── Portal (same proven pattern as the weather/miner portals) ─────────────────
static void shStartPortal() {
    WiFi.disconnect(true); delay(100);
    WiFi.mode(WIFI_AP); WiFi.softAP(SH_AP); delay(100);
    shDns = new DNSServer();
    shDns->setErrorReplyCode(DNSReplyCode::NoError);
    shDns->start(53, "*", WiFi.softAPIP());
    shSrv = new WebServer(80);
    shSrv->on("/", []{
        shHits++;
        String pg = FPSTR(SH_HTML);
        shSrv->send(200, "text/html", pg);
    });
    // All-time best: read by the game on load...
    shSrv->on("/hi", []{
        shSrv->send(200, "text/plain", String(shAllTime));
    });
    // ...and updated when any player's game ends. The reply is the (possibly
    // new) record so every connected player's HUD stays honest.
    shSrv->on("/score", []{
        uint32_t v = (uint32_t)shSrv->arg("v").toInt();
        if (v > 0 && v < 100000000UL) {
            if (v > shSessionBest) shSessionBest = v;
            if (v > shAllTime) {
                shAllTime = v;
                shNewRecord = true;
                shSaveAllTime();          // straight to flash - survives power loss
            }
        }
        shSrv->send(200, "text/plain", String(shAllTime));
    });
    shSrv->onNotFound([]{
        shSrv->sendHeader("Location", "http://192.168.4.1/"); shSrv->send(302, "", "");
    });
    shSrv->begin();
    shOn = true;
    shHits = 0;
}

static void shStopPortal() {
    if (shSrv) { shSrv->stop(); delete shSrv; shSrv = nullptr; }
    if (shDns) { shDns->stop(); delete shDns; shDns = nullptr; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    shOn = false;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void shareEnter() {
    fb.setRotation(0);
    shLoadAllTime();
    shSessionBest = 0;
    shNewRecord = false;
    shStartPortal();
}

void shareShutdown() {
    if (shOn) shStopPortal();
}

void shareTick() {
    // Service the captive portal + web server every frame.
    if (shOn) {
        if (shDns) shDns->processNextRequest();
        if (shSrv) shSrv->handleClient();
    }
    // (Sleep = hold both, menu = double-click both are handled globally.)
}

// ── Draw: the device shows join instructions, like the weather setup screen ───
void shareDraw() {
    fb.fillSprite(TEAL_DEEP);

    fb.fillRect(0, 0, W, 18, PURPLE_DARK);
    fb.setTextDatum(ML_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("PINBALL SHARING", 4, 9, 1);

    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("Host a game over Wi-Fi", W/2, 48, 1);

    fb.setTextColor(LIME_PALE);
    fb.drawString("1. Join Wi-Fi:", W/2, 84, 1);
    fb.setTextColor(LIME);
    fb.drawString(SH_AP, W/2, 104, 2);

    fb.setTextColor(LIME_PALE);
    fb.drawString("2. Open in browser:", W/2, 140, 1);
    fb.setTextColor(TEAL_BRIGHT);
    fb.drawString("192.168.4.1", W/2, 160, 2);

    fb.setTextColor(LIME_PALE);
    fb.drawString("3. Play Pinball Blaster!", W/2, 196, 1);

    // Scores: this session's best sits right next to the all-time record
    // (all-time = best score any device that ever connected has posted;
    // it lives in flash so it survives reboots).
    {
        char ln[40];
        fb.setTextColor(LIME_PALE);
        snprintf(ln, sizeof(ln), "SESSION %lu", (unsigned long)shSessionBest);
        fb.drawString(ln, W/2 - 40, 218, 1);
        fb.setTextColor((shNewRecord && (millis() % 600 < 300)) ? LIME : TFT_GOLD);
        snprintf(ln, sizeof(ln), "ALL-TIME %lu", (unsigned long)shAllTime);
        fb.drawString(ln, W/2 + 42, 218, 1);
        if (shNewRecord) {
            fb.setTextColor(LIME);
            fb.drawString("NEW RECORD!", W/2, 232, 1);
        }
    }

    // Players-joined counter
    fb.setTextColor(TEAL_LIGHT);
    char hits[28]; snprintf(hits, sizeof(hits), "page loads: %u", (unsigned)shHits);
    fb.drawString(hits, W/2, shNewRecord ? 246 : 240, 1);

    // The intro-screen link, shown as text (clickable on the hosted page).
    fb.setTextColor(TEAL_DARK);
    fb.drawString("more at:", W/2, 256, 1);
    fb.setTextColor(PURPLE_LIGHT);
    fb.drawString(INTRO_URL, W/2, 272, 1);

    fb.fillRect(0, H-14, W, 14, PURPLE_DARK);
    fb.setTextDatum(MC_DATUM); fb.setTextColor(LIME_PALE);
    fb.drawString("dbl-click both = menu", W/2, H-7, 1);

    fb.pushSprite(0, 0);
}
