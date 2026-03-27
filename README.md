# BoWWClient
Wakeword client for BoWWServer

`./BoWWClient -d plughw:3 -m ../models/hey_jarvis_int8.tflite -t 0.75 -D`  
`./boww_server --debug`
```
BoWWClient - Edge Smart Speaker Engine
Usage: ./BoWWClient [OPTIONS]

Options:
  -c <dir>       Path to config dir for client_guid.txt (default: ./)
  -d <device>    ALSA KWS Mono Input (default: plughw:Loopback,1,0)
  -A <device>    ALSA Multi-Mic Array Input (Streaming Source)
  -s <uri>       Manual Server URI override (e.g., ws://192.168.1.50:9002)
  -p <float>     Pre-roll buffer duration in seconds (default: 3.0)
  -m <filepath>  Path to trained .tflite model file
  -t <string>    KWS Params: Threshold,Decay,WindowSec (default: 0.75,0.1,0.6)
  -D             Enable Debug Mode (Live VU and logs)
  -h             Show this help message and exit
```
Still to create a BT mobile app as local BT is sceure by nature.

On the server when 1st enrolling a client it gains a temp_id  
A mobile app will add that temp_id and guid_id to add that client to a group.  
Currently will have to be done by hand client_guid.txt containing.  
`871fb1f9-7ecf-426e-a5ce-a6ad67cfdd72`

Server  
clients.yaml  
```
groups:
  - name: "bedroom"
    sample_rate: 16000
    channels: 1
    arbitration_timeout_ms: 200
    vad_threshold: 0.75
    vad_no_voice_ms: 2000
    output: "file"       # C++ expects string: "file" or "alsa"
    device: ""           # Only used if output is "alsa" (e.g., "hw:0,0")

clients:
  - guid: "871fb1f9-7ecf-426e-a5ce-a6ad67cfdd72"
    group: "bedroom"
    # Update this TempID to whatever the server prints in the log when you connect
```
You can have multiple clients in a group so you can have multiple mics in a zone.  
The mics that pick up a wakeword transmit the wakeword threshold and best threshold is chosen as best stream  

-d <device>    ALSA KWS Mono Input (default: plughw:Loopback,1,0)  
This is the 16khz 16bit mono audio stream for the KWS  
-A <device>    ALSA Multi-Mic Array Input (Streaming Source)  
If not defined the -d <device> stream will be streamed to the server.  
If defined the wakeword will use -d <device> but stream the -A <device>

# Hardware
Any single mic or channel from 2/4 mic hat boards can be used.
A recomendation is for the [MAX9814](https://learn.adafruit.com/adafruit-agc-electret-microphone-amplifier-max9814/overview)
![max9814](https://cdn-learn.adafruit.com/assets/assets/000/014/304/small360/adafruit_products_1713_LRG.jpg?1392325003)
The preamp and analogue agc of the max9814 helps much with farfield over standard mics and outputs a line voltage less sussceptable to noise for further distance.
You can use as is, a 1uf film capacitor on the output with block dc noise, having a 3.3v LDO at the mic end of the cable will also reduce noise even further as the Pi5v is not the cleanest.
The CM108 board type USB soundcards are good, cheap and you know what your getting. 
