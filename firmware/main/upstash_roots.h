// Trusted public CA root for the Upstash HTTPS client.
//
// ESP-IDF v5.3.2's compact `esp_crt_bundle` callback rejects the legitimate
// 2026 Let's Encrypt chain used by Upstash:
//   *.upstash.io -> YE1 -> ISRG Root YE -> ISRG Root X2
// The server also sends an X2 certificate cross-signed by X1. Trusting only
// X2 makes mbedTLS select the self-signed EC X2 anchor, avoiding that old
// RSA-4096 cross-sign path. Normal hostname and chain validation remain on;
// this is deliberately NOT a leaf-certificate pin or a no-verify workaround.
//
// Source: ESP-IDF v5.3.2 cacrt_all.pem (Mozilla CA bundle), extracted 2026-07-24.
// SHA-256 fingerprint (ISRG Root X2):
// 69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:
// 64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70
#pragma once

static const char s_upstash_trusted_root_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQswCQYDVQQGEwJV\n"
    "UzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElT\n"
    "UkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVT\n"
    "MSkwJwYDVQQKEyBJbnRlcm5ldCBTZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNS\n"
    "RyBSb290IFgyMHYwEAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0H\n"
    "ttwW+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9ItgKbppb\n"
    "d9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNV\n"
    "HQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZIzj0EAwMDaAAwZQIwe3lORlCEwkSHRhtF\n"
    "cP9Ymd70/aTSVaYgLXTWNLxBo1BfASdWtL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5\n"
    "U6VR5CmD1/iQMVtCnwr1/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";
