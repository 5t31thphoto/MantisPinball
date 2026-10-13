#pragma once
#include "Preferences.h"
#define HTTP_GET 1
#define HTTP_POST 2
#define HTTP_ANY 3
struct WebServer { template<class... A> WebServer(A...){} template<class... A> void on(A...){} template<class... A> void onNotFound(A...){} void begin(){} void stop(){} void close(){} void handleClient(){}
  template<class... A> void send(A...){} template<class... A> void sendHeader(A...){} template<class... A> String arg(A...){return String();} template<class... A> bool hasArg(A...){return false;}
  String uri(){return String();} int args(){return 0;} template<class... A> String argName(A...){return String();} template<class... A> void send_P(A...){} template<class... A> void setContentLength(A...){} template<class... A> void sendContent(A...){} };
