# Balboa GS WiFi Controller

<p align="center">
<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/balboa_gs_unit.jpg" width="450"><br />
<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/panel_ns.jpg" width="300"><img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/panel_vl406u.jpg" width="300">
</p>


## What's this?
This repo aims to provide a general **hardware and software solution** to add WiFi control to the Balbo GS line of SPA controllers using the 8-pin RJ45 connected displays.<br />
The PCB is designed to be installed directly on the RJ45 port of the controller PCB. Remote install using a RJ45 cable is also possible if that is preferred.<br />
The software support beyond the GS500Z/GS501Z will be heavily based on user contribution as the different controller displays have slightly different 7 segment displays and button functions.<br />
Please note that the only hardware I personally have access to is the VL406U 4 button control panel and the GS500Z/GS501Z controller to fully confirm functionality.
The initial POC software framework is based on <a href="https://github.com/MagnusPer/Balboa-GS510SZ">this excellent repo by MagnusPer</a>. For more detailed information on the protocol than I'm providing here, please visit his repo.<br />

### Current state and support
The PCB is currently in a protoype stage but has been working well for me for roughly two years. It is tested and confirmed to support the following controllers:
- GS500Z
- GS501Z

The PCB should thus be compatible with the following control displays (Z suffix controllers):
- VL200, VL240, VL260, VL400, VL401, VL402, VL403, VL404, VL406T, VL406U (these are compatible with the Z suffix main controllers)

The PCB has solder jumpers that can be set the pinout to be compatible with either the pinout of the panels above or the SZ suffix controllers using the following displays:
- VL600S, VL700S, VL701S, VL702S

It's very likely that the PCB can be used with the VL801D and VL802D control displays as well but that is yet to be tested and confirmed.

## Hardware
### Control display data and pin functions
<table align="center">
  <tr>
    <th colspan="2">Z suffix, 4 button control displays</th>
    <th colspan="2">SZ/D suffix, 6+ button control displays</th>
  </tr>
  <tr>
    <td align="center">Pin 1</td>
    <td align="center">5 VDC</td>
    <td align="center">Pin 1</td>
    <td align="center">?</td>
  </tr>
  <tr>
    <td align="center">Pin 2</td>
    <td align="center">Button: Up / Warm</td>
    <td align="center">Pin 2</td>
    <td align="center">?</td>
  </tr>
  <tr>
    <td align="center">Pin 3</td>
    <td align="center">Button: Light</td>
    <td align="center">Pin 3</td>
    <td align="center">Button data line</td>
  </tr>
  <tr>
    <td align="center">Pin 4</td>
    <td align="center">GND</td>
    <td align="center">Pin 4</td>
    <td align="center">GND</td>
  </tr>
  <tr>
    <td align="center">Pin 5</td>
    <td align="center">Display data line</td>
    <td align="center">Pin 5</td>
    <td align="center">Display data line</td>
  </tr>
  <tr>
    <td align="center">Pin 6</td>
    <td align="center">Clock line</td>
    <td align="center">Pin 6</td>
    <td align="center">Clock line</td>
  </tr>
  <tr>
    <td align="center">Pin 7</td>
    <td align="center">Button: Pump / Jets</td>
    <td align="center">Pin 7</td>
    <td align="center">5 VDC</td>
  </tr>
  <tr>
    <td align="center">Pin 8</td>
    <td align="center">Button: Down / Cool</td>
    <td align="center">Pin 8</td>
    <td align="center">?</td>
  </tr>
</table>

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/RJ45.jpg" width="200">

For more info on the SZ suffix controller pinout and data format, see the <a href="https://github.com/MagnusPer/Balboa-GS510SZ">MagnusPer</a> repo and for D suffix controllers the <a href="https://github.com/Shuraxxx/-Balboa-GS523DZ-with-panel-VL801D-DeluxeSerie--MQTT">Shuraxxx</a> repo.

### Button data
The four buttons of the Z suffix displays each produce a short high pulse on the respective pins as noted in the table.

### Display data
The display signal is made up of a clock and a data line. The complete data set is made up of three 7 bit chunks and one 3 bit chunk (24 bits total, see [bit 23 note](#a-note-on-bit-23--the-last-clock-pulse) below for why only 23 of those are actually read).<br />
Chunk 2 and chunk 3 are the two digits of the seven segment display, each coded in BCD. Chunk 1 does *not* drive a third 7-segment digit - see the table below for what its bits actually do. Chunk 4 is 3 discrete status bits, not a digit at all.

The second and third chunks are coded in BCD to represent a 7 segment LCD layout. The first bit is always 0. <br /><br />
If the display shows **36**: <br />
3 = (0)1111001 = 0x79  (Chunk 2) <br />
6 = (0)1011111 = 0x5F  (Chunk 3) <br /><br />
36 is also the temperature set on the example oscilloscope image below where yellow is the clock and blue is the encoded data.<br /><br />

| Bit(s)  | Chunk | Meaning |
| :---:   | :---: | :--- |
| 0, 3, 5, 6 | 1 | Unconfirmed / not seen used during normal operation |
| 4       | 1     | **Heater active** (normal 2-digit operation only) |
| 1, 2    | 1     | During the [boot sequence](doc/controller-info.md#boot-sequence-confirmed-live-2026-08-06) only: these two bits double as a bare "1" digit (hundreds place) - the hardware only wires up the two segments needed for a "1" here, not a full 7-segment digit, since a spa temperature reading never needs a hundreds digit above 1. Mutually exclusive with the heater-flag role above; both live in the same 7-bit slot but are never meaningful at the same time. |
| 7-13    | 2     | LCD segment 1 (first digit), 7-segment BCD - see worked example above |
| 14-20   | 3     | LCD segment 2 (second digit), 7-segment BCD - see worked example above |
| 21      | 4     | Pump 1 |
| 22      | 4     | Lights |
| 23      | 4     | Always low / likely a checksum, not a real status flag - see below. Also not reliably captured by this firmware, see [bit 23 note](#a-note-on-bit-23--the-last-clock-pulse) |

Cross-referencing against <a href="https://github.com/kgstorm/Balboa-GS100-with-VL260-topside">kgstorm's Balboa-GS100-with-VL260-topside</a> repo (same protocol, independently reverse-engineered): their equivalent of bit 23 is documented as always-low and used as a frame checksum, not a real status flag - so it's likely not "Pump 2, Blower" as originally guessed.

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/scope_data.png">

### A note on bit 23 / the last clock pulse

The full cycle is genuinely 24 bits (confirmed via oscilloscope: 3x 7-bit chunks + 1x 3-bit chunk, ~33us/bit, ~1080us total, one clock pulse per bit, no hidden framing). However, this firmware's ESP8266 interrupt-driven capture (`clockPinInterrupt()`/`decodeDisplayData()` in `lib/Balboa_GS_Interface`) only reliably reads the first 23 of those 24 bits - it detaches its interrupt one pulse early, before the last bit (chunk 4's 3rd bit) is captured. That last bit is exposed in the firmware as `displayBit23`/HA's `_unknown_flag`, and its value has never been reliable (it's always read as `false`, since the code was silently reading uninitialized memory rather than a real captured bit).

An attempt was made to fix this (bump the capture to a true 24 bits) - it broke the display decode entirely (the interrupt-driven capture never once completed a cycle in a 100+ second live test) rather than just failing to read that one extra bit, and was reverted. The likely cause: at ~33us/bit, this is tight enough timing that ESP8266's WiFi-induced interrupt latency (SDK-internal critical sections can delay ISR entry by well over that) becomes marginal - <a href="https://github.com/kgstorm/Balboa-GS100-with-VL260-topside">kgstorm's own README</a> documents hitting the same class of problem on ESP8266 with this exact kind of interrupt-driven capture, and switched to an ESP32 to get reliable reads. This board is built around an ESP8266, so that option isn't available without a hardware redesign. `WiFi.setSleepMode(WIFI_NONE_SLEEP)` (one of the standard ESP8266 WiFi-latency mitigations) is already applied and didn't help.

Given the first 23 bits have been field-proven reliable for ~2 years, and bit 23's actual meaning was never confirmed to matter functionally, this has been left as-is rather than continuing to chase it live on production hardware.


## PCB basics

The PCB is connected in paralell to the existing controller display. Bidirectional levelshifters are connected to clock, data, and button lines to allow for reading the button inputs as well as sending pulses on any line. As all button data is indicated with a high pulse in some shape or form, both Z and SZ compatible displays should work with this setup. <br />
To accommodate for the difference in pinout between Z and SZ controllers the PCB has three solder jumpers. Their default state is not connected which means a selection needs to be made before the PCB will accept power from the RJ45 connector. <br />

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/jumpers.png" width="200"> <img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/levelshifter.png" width="300">

### Installation notes
- When connecting the PCB the SPA controller will reboot and go through the normal boot process. This is expected and also happens when an original display controller is connected.
- The male RJ45 connector soldered to the PCB fits very loosely in the female connector of the main controller board. The PCB has a small rectangular opening in which a small plastic pin can be inserted to friction fit the PCB in place. If this pin is the correct size, the mounting will be very reliable.
- As an alternative to the above install position an RJ45 cable can be used to connect the PCB.<br /><br />

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/pcb_example.jpg" width="350"> <img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/pcb_installed.jpg" width="350">

## Software
### Version 0.3 
- Basic functionality to read and set status using a **Balboa_GS** developed library and post a selection of buttons, sensors and diagnostics to the Home Assistant MQTT discovery topic.

The device auto-registers in Home Assistant via MQTT discovery - no manual entity configuration needed. Example of the resulting device page (controls, sensors, and diagnostics):

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/doc/images/HA_MQTT_discovery_device.png" width="700">

## References
- https://github.com/MagnusPer/Balboa-GS510SZ
- https://github.com/NickB1/OpenSpa/blob/master/documents/Balboa/Balboa_Display_Measurements.pdf
- https://www.olivierhill.ca/archives/72-The-Internet-of-Spas.html
- https://www.electronics-tutorials.ws/combination/comb_6.html

## Other Balboa projects 
- GL2000 Series https://github.com/netmindz/balboa_GL_ML_spa_control
- BP Series https://github.com/ccutrer/balboa_worldwide_app
- GS523SZ https://github.com/Shuraxxx/-Balboa-GS523SZ-with-panel-VL801D-DeluxeSerie--MQTT
- GS100 with VL260 topside (ESPHome) https://github.com/kgstorm/Balboa-GS100-with-VL260-topside
