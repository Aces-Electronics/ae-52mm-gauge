# AE 52mm Gauge Firmware
Firmware for the Aces Electronics 52mm OLED Gauge, utilizing LVGL for the UI and ESP-NOW for low-latency communication with the Smart Shunt.

## Features
- **Live Monitoring**: Displays Voltage, Current, Power, and SOC.
- **Starter Battery Status**: Monitors a second voltage input.
    - **Animation**: Cycles through Voltage (3s) -> Status (3s) -> Run Flat Time (56s).
- **Wireless Pairing**: Securely pairs with the Smart Shunt via ESP-NOW using QR Code exchange.
- **Error Indication**:
    - **Red Heartbeat**: The Gauge's main UI elements will flash **RED** if the Shunt reports a critical error (Load Off, Over Current, E-Fuse Trip).
- **OTA Updates**: Wireless updates via WiFi with enhanced safety features:
    - **Safe Mode**: Freezes UI drawing during flash parsing to prevent memory corruption.
    - **Visual Feedback**: Displays "Updating..." in RED and turns off the screen during the critical write phase.
    - **Loop Protection**: Rejects updates if the version matches the currently installed firmware.

## Communication Protocol (ESP-NOW)
The Gauge listens for a compact **224-byte Mesh Struct** (`struct_message_ae_smart_shunt_mesh`) broadcast by the Smart Shunt. This ensures reliable data delivery within the standard ESP-NOW frame limits while providing real-time updates for:
- Main Battery Voltage/Current/SOC
- Starter Battery Voltage
- Relayed Temperature Sensors
- TPMS Pressures
