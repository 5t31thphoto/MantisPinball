#pragma once
#include "WiFiClient.h"
#include "WiFi.h"
struct WiFiClientSecure : WiFiClient { void setInsecure(){} template<class... A> void setCACert(A...){} template<class... A> void setHandshakeTimeout(A...){} };
