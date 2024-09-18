# Balboa GS WiFi Controller

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/balboa_gs_unit.jpg" width="450"><br />
<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/panel_ns.jpg" width="300"><img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/panel_vl406u.jpg" width="300">


## What's this?
This repo aims to provide a general **hardware solution** to add WiFi control to the Balbo GS line of SPA controllers using the 8-pin RJ45 connected displays.<br />
The PCB is designed to be installed directly on the RJ45 port of the controller PCB. Remote install using a RJ45 cable is also possible if that is preferred.<br />
The software will be heavily based on user contribution as the different controller displays have slightly different 7 segment displays and button functions.<br />
Please note that the only hardware I personally have access to is the VL406U 4 button control panel and the GS500Z/GS501Z controller.
The POC software framework is based on <a href="https://github.com/MagnusPer/Balboa-GS510SZ">this excellent repo by MagnusPer</a>. For more detailed information on the protocol please visit his repo.<br />

### Current state and support
The PCB is currently in a protoype stage. It is tested and confirmed to support the following controllers:
- GS500Z
- GS501Z

The PCB should thus be compatible with the following control displays (Z suffix controllers):
- VL200, VL240, VL260, VL400, VL401, VL402, VL403, VL404, VL406T, VL406U (these are compatible with the Z suffix main controllers)

The PCB has solder jumpers that can be set the pinout to be compatible with either the pinout of the panels above or the SZ suffix controllers using the following displays:
- VL600S, VL700S, VL701S, VL702S

It's very likely that the PCB can be used with the VL801D and VL802D control displays as well.

## Hardware
### Control display data and pin functions
#### Z suffix, 4 button control displays

| PIN           | Description             | 
| ------------- | ------------------------|
| PIN 1         | 5 VDC                   |
| PIN 2         | Button: Up / Warm       |
| PIN 3         | Button: Light           |  
| PIN 4         | GND                     |
| PIN 5         | Display data            | 
| PIN 6         | Clock                   |
| PIN 7         | Button: Pump / Jets     |  
| PIN 8         | Button: Down / Cool     |

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/RJ45.jpg" width="200">

For more info on the SZ suffix controller pinout and data format, see the <a href="https://github.com/MagnusPer/Balboa-GS510SZ">MagnusPer</a> repo and for D suffix controllers the <a href="https://github.com/Shuraxxx/-Balboa-GS523DZ-with-panel-VL801D-DeluxeSerie--MQTT">Shuraxxx</a> repo.

### Button data
The four buttons of the Z suffix displays are indicated by a short high pulse on the respective pins as noted in the table.

### Display data
The display signal is made up of a clock  and a data line. The complete data set is made up of three 7 bit chunks and one 3 bit chunk.<br />
The first chunk is mostly unused except for the fifth bit that indicates if the heater is active.<br />
The second and third chunk represents the two digits of the seven segment display. 

The second and third chunks are coded in BCD to represent a 7 segment LCD layout. The first bit is always 0. <br /><br />
If the display shows **36**: <br />
3 = (0)1111001 = 0x79  (Chunk 2) <br />
6 = (0)1011111 = 0x5F  (Chunk 3) <br /><br />
36 is also the temperature set on the example oscilloscope image below where yellow is the clock and blue is the encoded data.<br /><br />


| Chunk 1 - bit 0-6 | Chunk 2 - bit 7-13 | Chunk 3 - bit 14-20 | Chunk 4 - bit 21-23   | 
| :---:             | :---:              | :---:               | :---:                 |                   
| ?                 | LCD segment 1      | LCD Segment 2       |   21: Pump 1          |                    
| ?                 |                    |                     |   22: Lights          |        
| ?                 |                    |                     |   23: Pump 2, Blower? | 
| ?                 |                    |                     |                       | 
| 4: Heater         |                    |                     |                       | 
| ?                 |                    |                     |                       | 
| ?                 |                    |                     |                       | 

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/scope_data.png">


## PCB basics

The PCB is connected in paralell to the existing controller display. A bidirectional levelshifter is connected to clock, data, and button lines to allow for reading the button inputs as well as sending pulses on any line. As all button data is indicated with a high pulse in some shape or form, both Z and SZ compatible displays should work with this setup. <br />
To accommodate for the difference in pinout between Z and SZ controllers the PCB has three solder jumpers. Their default state is not connected which means a selection needs to be made before the PCB will accept power from the RJ45 connector. <br />

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/jumpers.png" width="200"> <img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/levelshifter.png" width="300">

### Installation notes
- When connecting the PCB the SPA controller will reboot and go through the normal boot process. This is expected and also happens when an original display controller is connected.
- The male RJ45 connector soldered to the PCB fits very loosely in the female connector of the main controller board. The PCB has a small rectangular opening in which a small plastic pin can be inserted to friction fit the PCB in place. If this pin is the correct size, the mounting will be very reliable.
- As an alternative to the above install position an RJ45 cable can be used to connect the PCB.<br /><br />

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/pcb_example.jpg" width="300">
<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/pcb_installed.jpg" width="300">

## Software
### Version 0.1 
- Basic functionality to read and set status using a **Balboa_GS** developed library. Two examples are provided, first with simple read and set functionality and the second using MQTT for remote access.

## References
- https://github.com/MagnusPer/Balboa-GS510SZ
- https://github.com/NickB1/OpenSpa/blob/master/documents/Balboa/Balboa_Display_Measurements.pdf
- https://www.olivierhill.ca/archives/72-The-Internet-of-Spas.html
- https://www.electronics-tutorials.ws/combination/comb_6.html

## Other Balboa projects 
- GL2000 Series https://github.com/netmindz/balboa_GL_ML_spa_control
- BP Series https://github.com/ccutrer/balboa_worldwide_app
- GS523SZ https://github.com/Shuraxxx/-Balboa-GS523SZ-with-panel-VL801D-DeluxeSerie--MQTT
