# Balboa GS WiFi Controller

<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/panel_ns.jpg" width="350"><img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/panel_vl406u.jpg" width="350">


## What's this?
This repo aims to provide a general hardware solution to add WiFi control to the Balbo GS line of SPA controllers using the 8-pin RJ45 connected displays.<br>
The PCB is designed to be installed directly on the RJ45 port of the controller PCB. Remote install using a RJ45 cable is also possible if that is preferred.<br>
The software will be heavily based on user contribution as the different controller displays have slightly different 7 segment displays and button functions.<br>
Please note that the only hardware I personally have access to is the VL406U 4 button control panel and the GS500Z/GS501Z controller.
The POC software framework is based on <a href="https://github.com/MagnusPer/Balboa-GS510SZ">this excellent repo by MagnusPer</a>.<br>

### Current state and support
The PCB is currently in a protoype stage. It is tested and confirmed to support the following controllers:
- GS500Z
- GS501Z

The PCB should thus be compatible with the following control displays (Z suffix controllers):
- VL200, VL240, VL260, VL400, VL401, VL402, VL403, VL404, VL406T, VL406U (these are compatible with the Z suffix main controllers)

The PCB has solder jumpers that can be set the pinout to be compatible with either the pinout of the panels above or the SZ suffix controllers using the following displays:
- VL600S, VL700S, VL701S, VL702S

## Hardware

### Pinouts
#### Z suffix, 4 button control displays
<img src="https://github.com/StarkJohan/Balboa-GS-WiFi/blob/main/extras/images/RJ45.jpg" width="200">

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




### version 0.1 
- Basic functionality to read and set status using a **Balboa_GS** developed library. Two examples are provided, first with simple read and set functionality and the second using MQTT for remote access.



## Measurements 

### PIN 1 and 2 
PWM output from control system, same on both pins. Unclear functionality for these PINs still! Maybe some internal light supply? <br />  
50Hz, pk-pk=18v, mean 5V

<img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/measurements/PIN_1%262.JPG" width="300">

### PIN 6 (Yellow - clock), PIN 5 (Blue - display data), PIN 3 (Pink - button data) 

<img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/measurements/Multiple_sequences.jpg" width="350">   

<p align="center">
<img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/measurements/Sequence.jpg" width="1000">
</p>

#### Conclusion
- The clock is pulsing 7 times for each chunck of display data, totaly 39 bits and 3 times for button data. A complete cycle 42 pulses. <br />
- Totaly of 6 display data chunks in each cycle <br />
- Each cycle is repeted every 20 ms <br />
- One total cycle duration 1.88 ms
- Bit duration for display data is 10 microseconds for display data <br />
- Bit duration for button data is 60 microseconds <br />

#### Display Data
Display shows **37.5C** - How does that translates into the bitstream? The first 4 chunks is coded in BCD to 7 segment LCD (see reference) but only with 7 bits sinse the LSB always is (0). <br />
3 = 1111001(0) = 0x79  (Chunk 1) <br />
7 = 1110000(0) = 0x70  (Chunk 2) <br />
5 = 1011011(0) = 0x5B  (Chunk 3) <br />
C = 1001110(0) = 0x4E  (Chunk 4) <br />

| Chunk 1 - bit(0-6) | Chunk 2 - bit(7-13) | Chunk 3 - bit(14-20) | Chunk 4 - bit(21-27) | Chunk 5 - bit(28-34) | Chunk 6 - bit(35-38) | 
| ------------------ | ------------------- | ------------------   | -------------------- | -------------------- | ------------------   |                    
| LCD segment 1      |  LCD segment 2      |   LCD Segment 3      |   LCD segment 4      | 28 - Button down/up  | 35 - Heater          |                    
|                    |                     |                      |                      | 29 - ?               | 36 - Pump 1          |        
|                    |                     |                      |                      | 30 - ?               | 37 - Pump 2          |
|                    |                     |                      |                      | 31 - Standard mode   | 38 - Light           |
|                    |                     |                      |                      | 32 - ?               |                      |
|                    |                     |                      |                      | 33 - ?               |                      |
|                    |                     |                      |                      | 34 - ?               |                      |
   
#### Button data 

|  Button   |  Decoding bit(0-2) | 
| --------- | ------------------ |
| Mode      |   0 0 1            |
| Temp up   |   1 0 0            |
| Temp down |   1 0 1            |  
| Light     |   0 1 1            |
| Pump 1    |   1 1 0            | 
| Pump 2    |   0 1 0            |
| Pump 3    |   1 1 1            |  

## Wiring

Since the display and controler unit hardware design is unknown an OR-gate is used to prevent damage if simultaneous HIGH/LOW levels from display and Arduino.

<img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/docs/Balboa_GS_Interface.jpg" width="1000">

## PCB fabrication

<img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/PCB%20layout/PCB_3D_view.jpg" width="303"> <img src="https://github.com/MagnusPer/Balboa-GS510SZ/blob/main/extras/PCB%20layout/PCB_2D_view.jpg" width="400">

Gerber files located here: [PCB Balboa-GS510SZ](https://github.com/MagnusPer/Balboa-GS510SZ/tree/main/extras/PCB%20layout )

## References of great help!
- https://github.com/NickB1/OpenSpa/blob/master/documents/Balboa/Balboa_Display_Measurements.pdf
- https://www.olivierhill.ca/archives/72-The-Internet-of-Spas.html
- https://create.arduino.cc/projecthub/meljr/7-segment-led-displays-102-using-a-shift-register-6b6976

## Other Balboa projects 
- GL2000 Series https://github.com/netmindz/balboa_GL_ML_spa_control
- BP Series https://github.com/ccutrer/balboa_worldwide_app
- GS523SZ https://github.com/Shuraxxx/-Balboa-GS523SZ-with-panel-VL801D-DeluxeSerie--MQTT
