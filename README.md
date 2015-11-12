# jackPiledmeter
jackPiledmeter is a basic DPM (Digital Peak Meter)
for the Jack Audio Connection Kit on the Raspberry Pi.
It uses LEDs connected to GPIO pins to show  audio levels.

Based on jackmeter - http://www.aelius.com/njh/jackmeter/ - by Nicholas J. Humfrey

This program uses the WiringPi library - http://wiringpi.com/ - by Gordon Henderson

More info on how to run it and how to connect the LEDs to the RPi can be found on the Wiki
https://github.com/RagnarJensen/jackPiledmeter/wiki
Example
-------

    ./jackPiledmeter system:capture_1
    Registering as 'ledmeter'.
    Connecting 'system:capture_1' to 'ledmeter:in'...
    


    
