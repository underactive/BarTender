// firmware/main/net_wifi.h
//
// Multi-network WiFi STA: a background manager task scans on every
// (re)connect, autoconnects to the strongest remembered network in range
// (8 dB stickiness, MRU tie-break), and promotes it to MRU on success.
// Scans run ONLY while disconnected (the device polls every 300 s, so it
// never needs to roam mid-connection — relocating produces a real
// disconnect, then a scan). App logic never blocks on the link.
#pragma once

#include <stdbool.h>

// Bring up WiFi STA + the manager task. Reads the remembered list from
// config_store itself. Call once from app_main when wifi_count() > 0.
void net_wifi_start_multi(void);

// true once an IP has been acquired (DHCP done); false while down/reconnecting.
bool net_wifi_is_connected(void);

// true once >= 2 consecutive scan sweeps have found ZERO remembered networks
// in range — i.e. the toy was relocated somewhere it knows nothing about.
// fetch.c gates its non-destructive self-heal on this so a slow sweep or an
// auth-retry loop can never spuriously trigger the add-network portal.
bool net_wifi_no_known_network(void);
