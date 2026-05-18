// firmware/main/provision.h
//
// First-boot provisioning: bring up a WPA2 SoftAP + captive HTTP form so the
// user enters home-WiFi creds + Upstash URL + READ-ONLY token. On submit the
// values are written to NVS (config_store) and the device reboots into normal
// (STA) operation. The SoftAP SSID/password are shown on the TFT.
//
// Mutually exclusive with net_wifi: app_main runs exactly one of them.
#pragma once

// Starts AP + HTTP server + captive DNS, pushes the SSID/pass to the UI, then
// returns (the device idles in AP mode until the form is submitted, which
// triggers a reboot). Never returns control to a normal app flow.
void provision_start(void);
