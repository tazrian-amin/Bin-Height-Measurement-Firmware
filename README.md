# Bin Height Measurement Firmware

A smart IoT firmware for measuring bin heights using an ADC sensor and communicating via multiple channels: USB (wired), Bluetooth Low Energy (wireless), and cloud connectivity through Notehub.

## Overview

This firmware runs on a **Blues Swan R5** microcontroller board and reads analog sensor data (bin height) from an ADC pin. It sends the measurements through three independent channels:

1. **USB Serial (Wired)** - Direct connection to Android app via Swan's micro-USB port
2. **Bluetooth Low Energy (BLE)** - Wireless connection to Android app via HM-10 module
3. **Notehub Cloud** - IoT cloud storage and synchronization via Notecard

## Hardware Setup

### MCU & Development Board
- **Board**: Blues Swan R5 (STM32L4 series)
- **USB**: Micro-USB port for wired communication and power

### Communication Modules

#### 1. Notecard (UART on Notecarrier-F)
- **Purpose**: Cloud connectivity and data sync
- **Wiring**:
  - `F_TX` → `N_RX`
  - `F_RX` → `N_TX`
  - `F_D5` → `N_ATTN` (attention interrupt)
- **Code Name**: `Serial1` (aliased as `notecardUart`)
- **Baud Rate**: 9600 bps

#### 2. HM-10 BLE Module (UART on Notecarrier-F)
- **Purpose**: Wireless Bluetooth communication to Android
- **Wiring**:
  - `F_A0` (MCU RX) ← `HM-10 TXD`
  - `F_A3` (MCU TX) → `HM-10 RXD`
  - `3V3` → `HM-10 VCC`
  - `GND` → `HM-10 GND`
- **Code Name**: `bleUart`
- **Baud Rate**: 9600 bps (default, configurable via AT commands)
- **Note**: Do not connect a USB-Serial adapter to F_A0/F_A3 while HM-10 is connected

#### 3. ADC Sensor
- **Pin**: PA1
- **Resolution**: 12-bit
- **Purpose**: Reads analog voltage proportional to bin height

### Android Communication
- **Wired Mode**: USB micro-connector → Android USB-Serial adapter
- **Wireless Mode**: HM-10 BLE module → Android BLE app
- Choose either wired OR wireless at runtime; they don't interfere

## Firmware Behavior

### Initialization (Setup Phase)
1. Initialize all three serial connections (9600 bps)
2. Configure ADC pin for analog input (12-bit resolution)
3. Set Notecard attention pin as input and attach interrupt handler
4. Configure Notecard with response validation:
   - Set **ProductUID**: `com.gmail.amin.tazrian1979:binheightv2` (with timeout and error checking)
   - Set **Mode**: Continuous sync enabled (with timeout and error checking)
5. Arm the Notecard ATTN (attention) interrupt for inbound notes
6. Start main loop with optimized timing

### Main Loop

The firmware repeats these tasks every cycle:

#### 1. **Handle Inbound Cloud Notifications** (ATTN Interrupt-Driven)
When the ATTN pin goes HIGH (interrupt triggered):
- Set flag to indicate ATTN event (non-blocking)
- Initiate sync with Notehub (with 5-second timeout)
- After sync completes, fetch the note from the inbound file (`data.qi`)
- Delete it after reading
- Log the received content with error handling
- Re-arm the ATTN interrupt for next event

**Features**: 
- Interrupt-driven (no polling overhead)
- Non-blocking with timeout protection
- Response validation and error logging

**Purpose**: Allows remote commands/configuration from the cloud

#### 2. **Monitor HM-10 BLE UART** (if available)
- Read any incoming bytes from the HM-10 module
- Log them for debugging (useful when HM-10 is in AT command mode)

#### 3. **Periodic ADC Sampling** (every 5 seconds)
On the configured sample interval:
- Read 10 ADC samples from PA1 and average them (noise filtering)
- Send averaged value to all three channels:
  - **USB**: `ADC value:XXXX\r\n`
  - **HM-10 BLE**: `ADC value:XXXX\r\n`
  - **Notehub Cloud**: JSON note `{"adc":XXXX}` added to `data.qi` file (with response validation)
- Log the averaged ADC value
- Validate cloud responses and log errors if any

## Configuration

Key configurable constants in `main.cpp`:

```cpp
constexpr bool kDebugLogEnabled = true;                    // Enable/disable debug logging
constexpr unsigned long kSamplePeriodMs = 5000;            // ADC sample interval (ms)
constexpr unsigned long kSyncTimeoutMs = 5000;             // Max wait time for Notecard sync after ATTN (ms)
constexpr unsigned long kNotecardResponseTimeoutMs = 5000;  // Max wait time for Notecard responses (ms)
constexpr unsigned long kAdcAverageSamples = 10;            // Number of ADC samples to average per measurement
constexpr const char* kProductUid = "...";                 // Notehub product UID
constexpr const char* kInboundNotefile = "data.qi";         // Inbound note file
```

## Data Format

### ADC Output Format
All three channels transmit ADC readings in this format:
```
ADC value:2048
```
Where `2048` is the raw 12-bit ADC value (0–4095).

### Cloud (Notehub) Format
Notes are stored as JSON in the `data.qi` file:
```json
{"adc": 2048}
```

## Debug Output

When `kDebugLogEnabled = true`, debug messages are printed to the USB serial port:
- Notecard configuration status with responses and error checking
- Averaged ADC readings
- Inbound note content with timeouts and errors
- ATTN interrupt triggers
- HM-10 UART traffic
- Response validation results

Example debug output:
```
Notecard product set: {...response...}
Notecard mode set: {...response...}
Notecard configured for ProductUID: com.gmail.amin.tazrian1979:binheightv2
Notecard ATTN armed successfully
===Starting main loop===
-- Notecard ATTN HIGH: polling inbound note --
>> Note Received: {...}
Raw ADC (averaged): 2048
ADC value sent to Notehub
```

Connect to the Swan's USB port at **9600 bps** to view debug output.

## Dependencies

- **PlatformIO**: Build and upload framework
- **Arduino Framework**: Core library
- **ArduinoJson**: JSON parsing (included via `lib_deps`)
- **Blues Wireless SDK**: Notecard communication (built into Swan board support)

## Building & Uploading

```bash
# Build the firmware
platformio run

# Upload to Swan R5
platformio run --target upload

# Monitor serial output (9600 bps)
platformio device monitor --baud 9600
```

## Troubleshooting

| Issue | Solution |
|-------|----------|
| No USB serial output | Ensure USB cable is connected; check debug logging is enabled |
| HM-10 not responding | Verify wiring on F_A0/F_A3; ensure no USB-Serial adapter on those pins |
| Notecard not syncing | Check debug output for response errors; verify ProductUID matches Notehub account |
| "Notecard product config timeout" | Check Notecard wiring; ensure Notecard responds within 5 seconds |
| ADC reading stuck at 0 or 4095 | Verify PA1 wiring and analog reference voltage |
| ATTN interrupt never fires | Confirm F_D5 is connected to N_ATTN; verify interrupt is armed (check debug output) |
| Response validation errors in debug | Check Notecard firmware version; ensure commands are properly formatted |

## Product Information

- **Product UID**: `com.gmail.amin.tazrian1979:binheightv2`
- **Inbound Notefile**: `data.qi` (for cloud-to-device commands)
- **Outbound Notefile**: `data.qi` (for device-to-cloud data)

## Architecture Diagram

```
┌────────────────────────────────────────────────┐
│         Blues Swan R5 Microcontroller          │
├────────────────────────────────────────────────┤
│                                                │
│  ┌──────────────────────────────────────────┐  │
│  │         Main Loop (every cycle)          │  │
│  ├──────────────────────────────────────────┤  │
│  │ 1. Check Notecard ATTN (inbound notes)   │  │
│  │ 2. Monitor HM-10 BLE UART                │  │
│  │ 3. Sample ADC every 5 seconds            │  │
│  └──────────────────────────────────────────┘  │
│       ↓               ↓              ↓         │
│     Serial         bleUart      notecardUart   │
│     (USB)           (BLE)        (Notecard)    │
│       ↓               ↓             ↓          │
└───────┼───────────────┼─────────────┼──────────┘
        │               │             │
    ┌───▼─────┐     ┌───▼────┐    ┌───▼─────┐
    │ Android │     │ HM-10  │    │Notehub  │
    │  (USB)  │     │  BLE   │    │  Cloud  │
    └─────────┘     └────────┘    └─────────┘
```

## Power Considerations

- The Swan R5 draws power from the USB connection
- The HM-10 and Notecard require stable 3.3V power
- Ensure adequate current supply when all modules are active (USB + BLE + cellular modem)

## Implemented Enhancements

- ✅ Data filtering/averaging for ADC readings (10-sample averaging)
- ✅ Response validation and error checking for all Notecard commands
- ✅ Non-blocking ATTN interrupt handling with timeout protection
- ✅ Optimized setup timing with reduced delays

## Future Enhancements

- Implement over-the-air firmware updates via Notehub
- Add configurable sample rate via cloud commands
- Support multiple sensor inputs
- Add battery-backed timestamp or RTC module
- Implement adaptive averaging based on signal noise
