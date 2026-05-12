# plantsongbiodataNew_littlescale
Plantsongs Biodata Poly 5 midi123 + sample 


Yes, exactly — that makes sense.
You want:
Pot = controls main sensitivity / Channel 1 trigger
Delta spike = controls when MIDI Channel 2 and 3 open
So I made a new full Arduino IDE patch:
Download PlantSong_Chromatic_MiddleC_DeltaSpike_CH2_CH3.ino
This version is based on your uploaded Teensy patch structure.
The important difference:
Channel 1 = normal pot-controlled trigger
Channel 2 = raw delta spike only
Channel 3 = stronger raw delta spike only
So the pot does not control Channel 2 and Channel 3 entry anymore.
The new controls are near the top:
const float CH2_DELTA_SPIKE_RATIO = 2.2f;
const float CH3_DELTA_SPIKE_RATIO = 4.0f;

const unsigned long CH2_MIN_RAW_DELTA = 60;
const unsigned long CH3_MIN_RAW_DELTA = 130;
To make Channel 2 and 3 come in less often, try:
const float CH2_DELTA_SPIKE_RATIO = 3.0f;
const float CH3_DELTA_SPIKE_RATIO = 6.0f;

const unsigned long CH2_MIN_RAW_DELTA = 100;
const unsigned long CH3_MIN_RAW_DELTA = 220;
This patch watches for a delta spike compared to the recent normal delta level, so Channel 2 and 3 should open only when the plant data jumps strongly, not just because the pot is sensitive.

<br>

Select Teensy 4.1 
Select Serial + Midi to uplaod 

