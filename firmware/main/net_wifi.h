// firmware/main/net_wifi.h
//
// Minimal WiFi STA: connect to the provisioned AP and auto-reconnect with
// capped backoff (5 -> 15 -> 60 s). Connection state is queried by the UI/
// fetch task; we never block app logic on the link.
#pragma once

#include <stdbool.h>

// Bring up WiFi STA and start (re)connecting in the background. Safe to call
// once from app_main after credentials are known.
void net_wifi_start(const char *ssid, const char *pass);

// true once an IP has been acquired (DHCP done); false while down/reconnecting.
bool net_wifi_is_connected(void);
