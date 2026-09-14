// device_identity.h -- the stable id reported as DeviceInfo.deviceId (HTTP)
// and over kBleCharDeviceIdentityUuid (BLE).
//
// Derived from the factory-programmed EFUSE MAC, so it survives a reflash or
// a factory reset without needing its own NVS entry.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>

namespace drivers {

constexpr uint32_t kDeviceIdChars = 12;  // 6 MAC bytes, hex-encoded

inline void DeviceId(char* out, uint32_t capacity) {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(out, capacity, "%012llx", static_cast<unsigned long long>(mac));
}

}  // namespace drivers
