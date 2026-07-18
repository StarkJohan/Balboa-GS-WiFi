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
