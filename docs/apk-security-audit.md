# APK Security Audit — Diesel Heater BLE Apps

Audited 2026-05-07. Both APKs decompiled with jadx.

## Summary

| Category | airHeaterByBLE (BYD) | AirHeaterCC (CC) |
|----------|---------------------|------------------|
| **Rating** | SUSPICIOUS | SUSPICIOUS |
| **Package** | `com.clj.airheater` | `com.chchi.app.zcjr` |
| **Version** | 2.0.0 | 1.0.1 |
| **Framework** | DCloud UniApp | DCloud UniApp |
| **Signer** | CN=zhouxiangyang | CN=Luo, Wenzhou Chengchen Electronics |
| **Permissions** | 34 | 30 |
| **Device fingerprinting** | IMEI, IMSI, MAC, OAID | Same |
| **Phone-home telemetry** | DCloud + Booyood API | DCloud only |
| **SSL validation** | Disabled | Disabled |
| **Ad SDK** | Wanka, Youdao | Same |
| **Hidden .dex loading** | Yes (39285EFA) | Yes (same) |
| **Crypto mining** | No | No |

Both apps are thin Vue.js web apps inside the DCloud UniApp Android container. The suspicious behavior comes from the framework, not necessarily the heater developers.

---

## Why Google Flags These

1. **OAID SDK** loads a hidden `.dex` file at runtime via obfuscated native code (`lib39285EFA.so` / `39285EFA.dex`) — classic packer/loader behavior that triggers heuristic malware detection
2. `REQUEST_INSTALL_PACKAGES` — ability to install arbitrary APKs
3. Disabled SSL certificate validation — known vulnerability flagged by Play Protect
4. IMEI/IMSI collection via reflection — increasingly flagged by Android
5. Base64-encoded URLs hiding phone-home endpoints

---

## Excessive Permissions

### BYD App (34 permissions)

Dangerous/unnecessary for a BLE heater controller:
- `CAMERA`
- `RECORD_AUDIO`
- `READ_PHONE_STATE` (used to collect IMEI)
- `ACCESS_BACKGROUND_LOCATION`
- `BLUETOOTH_PRIVILEGED` (system-level, should not be in a user app)
- `INSTALL_PACKAGES` / `REQUEST_INSTALL_PACKAGES`
- `WRITE_SETTINGS`
- `READ_LOGS` (can read other apps' log data)
- `MOUNT_UNMOUNT_FILESYSTEMS`
- `MODIFY_AUDIO_SETTINGS`
- `READ_EXTERNAL_STORAGE` / `WRITE_EXTERNAL_STORAGE`
- `com.asus.msa.SupplementaryDID.ACCESS` (ASUS device ID tracking)

### CC App (30 permissions)

Same set minus: `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS`, `ACCESS_BACKGROUND_LOCATION`, `BLUETOOTH_PRIVILEGED`, `BLUETOOTH_ADVERTISE`. Adds `GET_ACCOUNTS`.

---

## Data Exfiltration

### DCloud Framework Telemetry (both apps)

- Crash reporting: `https://cr.dcloud.net.cn/collect/crash`
- Statistics: `https://er.dcloud.net.cn/rv`, `https://er.dcloud.net.cn/sc`
- International: `https://er.dcloud.io/rv`, `https://er.dcloud.io/sc`
- URLs are base64-encoded to avoid detection

### Device Fingerprinting (both apps)

`TelephonyUtil` class collects:
- Device IMEI (`getDeviceId()`, `getImei()`, `getMultiIMEI()`)
- IMSI (subscriber identity)
- MAC address
- Android advertising ID
- Random persistent ID stored in hidden file `.DC4278477faeb9.txt`
- All data stored in SharedPreferences and base64-encoded

### OAID SDK (both apps)

China's MIIT Open Anonymous Device Identifier SDK (`com.bun.miitmdid`):
- Collects device IDs across Huawei, OPPO, Vivo, Samsung, Meizu, ASUS
- Loads native library `lib39285EFA.so` with obfuscated loading code
- Contains hidden `.dex` file (`39285EFA.dex`) loaded at runtime

### BYD-Specific Endpoints

- API: `https://www.booyoodapp.com/api`
- OTA updates: `https://www.booyood.com/node1`
- Local WiFi AP: `http://192.168.4.1`
- Static assets: `https://bydpic.oss-cn-shanghai.aliyuncs.com/` (Alibaba Cloud, Shanghai)

---

## SSL Certificate Validation — Disabled (Both Apps)

`CustomTrustMgr` implements `X509TrustManager` with empty `checkServerTrusted()` and `checkClientTrusted()` methods. Accepts any SSL certificate. All HTTPS traffic is interceptable via MITM.

---

## Ad System (Both Apps)

Full DCloud ad SDK:
- `ADHandler.java` — main ad orchestrator
- `ADHandler_wanka.java` — Wanka ad network
- `ADHandler_youdao.java` — Youdao ad network (NetEase)
- Splash screen ads, background ad download service
- Ad system uses collected IMEIs/IMSI for targeted ads

---

## Native Libraries

Both apps share essentially the same set (from DCloud framework):
- `lib39285EFA.so` — **Suspicious**: hex-named OAID/JDog library, loads hidden .dex
- `libweexjss.so` (13.5 MB) — Weex JavaScript engine (Alibaba/Taobao)
- `libweexcore.so` — Weex rendering core
- `libbreakpad-core.so` — Google Breakpad crash reporter
- Others: image processing (Fresco), GIF, WebP, MP3 encoding, blur effects

---

## Third-Party SDKs (Both Apps)

- DCloud UniApp — app framework
- Alibaba Weex — UI rendering
- Alibaba FastJSON — JSON parser
- Facebook Fresco — image loading
- OkHttp3 — HTTP client
- BUN MIIT OAID — Chinese government device ID SDK
- Huawei HMS PPS — Huawei ad/push services
- Meizu Flyme / HeyTap OPPO / Samsung / ASUS — manufacturer device ID SDKs
- Google Breakpad — crash reporting

---

## Recommendation

Neither app should be considered safe for a personal phone. They are not overtly malicious (no rootkits, no SMS abuse, no crypto mining), but they collect excessive device data, phone home to Chinese servers, have broken SSL, and load obfuscated native code at runtime.

The ESP32 BLE implementation in this project will replace both apps entirely.
