# Standby indoor temperature screen

The remote AIoT MQTT connection also subscribes at QoS 1 to
`aiot/device/ENV-MONITOR-001/report`. It reuses the configured TLS CA
validation, broker endpoint, credentials and reconnect path; no broker secret
is added to source or logs.

Only the Idle screen is updated. Under normal conditions it shows only
`室内温度: xx.x ℃`. After three minutes without a valid report, it retains the
last temperature and adds `室内数据已过期`. Other application states continue to
own the display.

The cache retains humidity, pressure, illuminance, signal strength and status
for a future voice/tool answer integration. Pressure and illuminance may be
absent, and are also read from legacy JSON carried by `message`.

MQTT callbacks only validate, reassemble and cache data. The refresh is queued
onto `Application::Run`, so no callback directly calls LVGL or display hardware.

The built-in deterministic parser check is `MqttLogClient::RunEnvironmentParserSelfTest()`.
It covers the current payload, legacy `message` payload, and a missing-temperature
rejection case.

Build for the configured ESP32-S3 `bread-compact-wifi` board:

```powershell
& 'D:\Espressif\v5.5.4\esp-idf\export.ps1'
idf.py build
```

The verified build produced `build/xiaozhi.bin` (2,715,440 bytes,
SHA-256 `AA274600E96FD08E301682AB648B16D0D288E62D972C1107A05B8A25BCB809BB`).
On 2026-08-05 it was flashed to the ESP32-S3 test board on COM7 using the
existing non-destructive procedure: only `ota_data_initial.bin` at `0xD000` and
`xiaozhi.bin` at `0x20000` were written, both with esptool hash verification.
NVS (including the existing Wi-Fi and remote MQTT configuration) was not
erased or rewritten. Post-flash logs confirmed normal Wi-Fi/TLS startup, Idle,
and all three MQTT subscriptions (announcement command, announcement audio,
and environment report).
