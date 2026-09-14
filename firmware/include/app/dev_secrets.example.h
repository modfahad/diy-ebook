#pragma once
// Template for the DEV_AUTOPROVISION build's credentials.
//
// Copy to dev_secrets.h and fill in. **dev_secrets.h is gitignored and must
// stay that way** -- it holds a real Wi-Fi passphrase and a real pairing
// token, and the pairing token is the bearer credential for every destructive
// endpoint in the HTTP API (upload, delete, and restore when it exists).
//
//   cp firmware/include/app/dev_secrets.example.h \
//      firmware/include/app/dev_secrets.h
//
// Nothing includes this file unless the build sets -DDEV_AUTOPROVISION=1, so
// the product build cannot carry these values even by accident:
//
//   PLATFORMIO_BUILD_FLAGS="-DDEV_AUTOPROVISION=1" pio run -d firmware \
//       -e crowpanel_579 -t upload
//
// This exists for one reason: the Windows BLE client cannot complete the
// provisioning GATT flow (docs/pending.md section 3), and re-writing
// /DEVICE/wifi.json onto the card by hand before every test is slow. It is a
// development shortcut with a real cost, not a feature. Provision over BLE
// before a device matters.

#define DEV_WIFI_SSID "your-network-name"
#define DEV_WIFI_PASSPHRASE "your-network-password"

// <= 64 characters (net::kTokenMaxChars). Generate a fresh one, do not reuse
// an example: anything published here is public.
//   node -e "console.log(require('crypto').randomBytes(24).toString('base64url'))"
#define DEV_PAIRING_TOKEN "replace-me-with-a-random-token"
