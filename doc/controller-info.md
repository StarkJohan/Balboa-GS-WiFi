# Balboa GS Controller - General Info

General reference info on the GS500Z-series controller/panel combo (VL200-VL406, Z-suffix, 4-button), gathered from Balboa's official documentation. Not specific to this project's firmware - just useful background on what the controller itself can do from the physical panel.

Source: [Balboa 500Z-Series Operation Guide](https://www.bluewhalespa.com/wp-content/uploads/2017/11/Blue-Whale-Spa-Balboa-Hot-Tub-GS500Z-Series-VL401-User-Guide-series.pdf) (Balboa Water Group, covers VS/GS systems 500Z through 521Z with panels VL200 through VL406).

## Heating modes

Three modes are available: **Standard**, **Economy**, and **Sleep**.

| Mode | Behavior |
| :--- | :--- |
| Standard | Maintains the set temperature continuously. Display briefly shows `St` when switched in. |
| Economy | Heats to the set temperature only during filter cycles. Display shows `Ec`, alternating with water temp while the pump runs. |
| Sleep | Heats to within 10°C (20°F) of the set temperature, only during filter cycles. Display shows `SL`, alternating with water temp while the pump runs. |

**To switch modes:** press "Temp" then "Light". On 4-button panels without a dedicated Temp button (like ours), whichever button serves that role - Warm/Up or Cool/Down - works the same way.

**Important caveat, directly from the manual:** *"Depending on system configuration, mode changing may not be available and will be locked in Standard Mode."* Whether this actually does anything depends on how the specific board was configured (jumpers/DIP switches) at install time - it's entirely possible mode switching is locked out and Standard is the only mode available from the panel, regardless of the button sequence being correct.

## Filter cycles

**Timing:**
- The first filter cycle starts 6 minutes after the spa is powered on.
- The second filter cycle starts 12 hours after the first.

**Duration:** programmable to 2, 4, 6, or 8 hours, or continuous (`FC` on the display). Default is 2 hours for non-circ systems, 4 hours for circ systems.

**To program:** press "Temp" then "Jets 1" to enter programming, press "Temp" to cycle through the duration options, then press "Jets 1" again to exit.

**What runs during a filter cycle:**
- Non-circ systems: low-speed Pump 1 (+ ozone generator, if installed).
- 24-hour circulation systems: the circ pump + ozone run continuously anyway (may pause for 30-minute periods in hot environments, except during filter cycles).
- Non-24-hour circulation systems: circ pump + ozone run during filter cycles (and possibly at other automatic times too).
- At the start of every filter cycle, all other equipment runs briefly first to purge the plumbing.

Unlike heating mode, filter cycle programming is described as a normal, always-available panel function - not subject to the "may be locked by system configuration" caveat above.

## Related protocol references

- [kgstorm/Balboa-GS100-with-VL260-topside](https://github.com/kgstorm/Balboa-GS100-with-VL260-topside) - independently reverse-engineered the same clock/data protocol on a VL260 panel; their mode-switch button sequence (Cool, then Light) matches the official manual's "Temp, then Light" exactly.

## Boot sequence (confirmed live, 2026-08-06)

Captured twice independently (a full spa power-off, and a display-only reconnect with the ESP8266 bridge board left powered throughout) via a live MQTT capture of `_raw_frame`/`_display`/`_heater`/`_pump1`, cross-referenced against the GS500Z service manual's own "Power Up Display Sequence" section. Both captures showed the same sequence:

1. **Display self-test ("three horizontal bars" on both digit positions), ~2s.** Not visible anywhere in the captured bus data (bus is genuinely idle - no clock activity at all during this window). This is the display unit's own local power-on acknowledgment, generated independently of the main controller - the protocol this firmware decodes only observes clock/data bus traffic driven by the controller, so this phase is fundamentally outside what's capturable here.
2. **"100", ~3s.** Chunk 2/3 decode normally as "00" (`B1111110` = "0", the standard all-segments-except-middle shape). The leading "1" comes from Chunk 1, which is *not* a full 7-segment digit - the physical hardware only wires up the two segments needed to show a bare "1" there (this position can only ever be blank or "1", since a spa temperature display never needs a hundreds digit above 1). Those same two bits are unused during normal 2-digit operation, when Chunk 1's bit 4 instead carries the heater flag - the two roles share the same 7-bit transmission slot but are mutually exclusive in practice, not decoded through the same lookup table as Chunk 2/3.
3. **Blank**, then three transitional 2-digit readings (`61`, `43`, `12`) - reproduced identically across both independent captures, so genuinely part of the sequence rather than capture noise. Likely the SSID/software-version digits and heater-wattage config screen the manual describes, though the exact mapping to those specific manual sections isn't confirmed.
4. **Priming Mode**, ~1-2 minutes, Pump 1 running. Two distinct display patterns seen here: literal text `"Pr"` (Chunk 2/3 = `B1100111`/`B0000101`, i.e. "P"/"r" - already correctly handled by the existing lookup table) and a dashes pattern `"--"` (Chunk 2/3 = `B0000001`/`B0000001`, only the middle segment lit on each digit - previously mis-decoded as `"jj"` by `lookup_LCD_character()`'s generic alphanumeric font, since that pattern coincidentally matches a stylized lowercase "j" in a full A-Z/a-z mapping that was never meant to represent this protocol's own status codes; fixed to return `"-"`). Whether these are two sequential priming phases or an alternating blink wasn't pinned down - both were observed, in both orders, across the two capture sessions.
5. Priming ends, real water temperature appears, heater engages and normal operation resumes. Right at that transition, the heater bit was observed rapidly toggling true/false (~50-150ms period) for about 90 seconds before settling to steady `true` - every other bit in the frame stayed perfectly stable throughout, which rules out capture noise as the explanation (real decode corruption scatters across random bit positions, not one isolated bit cleanly alternating for 90s straight). Most likely a genuine soft-start/duty-cycled heater ramp-up, silent if the heating element is SSR/triac-driven rather than a mechanical relay.

Also confirmed: the ESP8266 bridge board draws its own power from the same RJ45 5V pin as the display, so a full spa power-off reboots the bridge too (`availability` drops, `_boottime` resets) - but reconnecting only the physical display panel does not power-cycle the bridge (`availability` stays `online`, `_boottime` unchanged throughout), which is why capture #2 could observe the controller's boot sequence without an MQTT/WiFi reconnect gap at the very start.

## Firmware TODO / open items

- **Bit 23 / the 24th clock pulse** - parked, not actively being worked. See the "A note on bit 23 / the last clock pulse" section in the main README for the full writeup: the true 24-bit protocol format is confirmed (oscilloscope), but the ESP8266 capture can't reliably read the last bit without stalling entirely, likely ESP8266 WiFi-induced ISR timing marginality (same class of issue kgstorm's own README documents hitting on ESP8266, which is why that project moved to ESP32). Two live attempts both required a rollback to the working 23-bit capture.
- **SPA boot cycle handling** - resolved/documented above (see "Boot sequence"). The `Pr`/`--` priming-mode decode is fixed; the SSID/version/heater-config screens are seen but not yet mapped to specific manual sections.
- **Heater rapid-flash signal** - confirmed (see "Boot sequence" step 5): happens right as priming ends and real heating begins, lasts ~90s, and the clean single-bit-toggle signature rules out decode noise - most likely a genuine soft-start/duty-cycle ramp. Exact meaning (vs. a formal Balboa term for this phase) still unconfirmed. Currently exposed raw/real-time (undebounced) so it stays observable in HA history; once confirmed, consider a dedicated derived entity (e.g. "Heater warming up", distinct from steady "Heating") instead of just the raw diagnostic bit - see the three-state sensor design discussed but not yet implemented.
