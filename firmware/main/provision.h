// firmware/main/provision.h
//
// Captive-portal provisioning: WPA2 SoftAP + HTTP form. On submit the values
// are written to NVS (config_store) and the device reboots into normal (STA)
// operation. The SoftAP SSID/password are shown on the TFT.
//
// Mutually exclusive with net_wifi: app_main runs exactly one of them.
#pragma once

#include <stdbool.h>

// Starts AP + HTTP server + captive DNS, pushes the SSID/pass to the UI, then
// returns (device idles in AP mode until the form is submitted → reboot).
//
// `upstash_already_set` == true  → WiFi-ONLY form (just SSID/pass); the saved
//   Upstash URL/key/token are kept and NEVER re-rendered into the HTML. This
//   is the "add a network, keep everything" path (triple-tap / self-heal).
// `upstash_already_set` == false → full first-boot form (WiFi + Upstash
//   URL/key/token), as on a fresh device.
void provision_start(bool upstash_already_set);
