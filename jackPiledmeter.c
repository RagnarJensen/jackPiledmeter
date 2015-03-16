/*

	jackPiledmeter.c
	Simple LED peak meter for JACK on the Raspberry Pi.
	Copyright Ragnar Jensen 2014

	Based on jackmeter - http://www.aelius.com/njh/jackmeter/ - by Nicholas J. Humfrey
	This program uses the WiringPi library by Gordon Henderson - http://wiringpi.com/ - Thanks, Gordon!
	
	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <wiringPi.h>
#include <sr595.h>

#include <jack/jack.h>
#include <getopt.h>
#include "config.h"


float bias = 1.0f;
float peak = 0.0f;
int o_stdin = 0;		// Don't connect to a Jack server to read dB values, read dB values from stdin instead.
int o_stdout = 0;		// Don't display dB values on the LEDs, print them to stdout instead.
int o_shift_register = 0;	// LEDs are connected to one or more 74x595 shift registers.
int dtime = 0;
int decay_len;			// Peak hold time unit. 1.6 sec / (1 / refresh rate)
int decay_led;			// Decay time unit.
int o_decay = 4;		// Decay rate - dB per decay_led time unit.

int o_decay_bias = 2;		// Experimental - Is used, but doesn't do anything meaningful yet. For 2.0... ;-)

int o_peak_hold = 0;
int peak_hold_db = 0;
int peak_hold_decay = 0;

int rr_too_high = 0;
int single_light = 0;
static volatile sig_atomic_t running = 1;

jack_port_t *input_port = NULL;
jack_client_t *client = NULL;

int first_led = 0; // WiringPi GPIO0 - BCM GPIO 17 - physical pin 11
int number_of_leds = 8;

/* Signal handler -- break out of the main loop */
void graceful_exit(int sig) {
	fprintf(stderr,"%s: Caught signal %i, shutting down\n", PACKAGE_NAME, sig);
	running = 0; 
}

/* Read and reset the recent peak sample */
static float read_peak() {
	float tmp = peak;
	peak = 0.0f;

	return tmp;
}

/* Callback called by JACK when audio is available.
   Stores value of peak sample */
static int process_peak(jack_nframes_t nframes, void *arg) {
	jack_default_audio_sample_t *in;
	unsigned int i;

	/* just incase the port isn't registered yet */
	if (input_port == NULL) {
		return 0;
	}

	/* get the audio samples, and find the peak sample */
	in = (jack_default_audio_sample_t *) jack_port_get_buffer(input_port, nframes);
	for (i = 0; i < nframes; i++) {
		const float s = fabs(in[i]);
		if (s > peak) {
			peak = s;
		}
	}

	return 0;
}

/* Close down JACK and turn off LEDs when exiting */
static void cleanup() {
	const char **all_ports;
	unsigned int i;

	fprintf(stderr,"cleanup()\n");
	if(!o_stdin) {
		if (input_port != NULL ) {
			all_ports = jack_port_get_all_connections(client, input_port);
			for (i=0; all_ports && all_ports[i]; i++) {
				jack_disconnect(client, all_ports[i], jack_port_name(input_port));
			}
		}
		jack_client_close(client);
	}
    /* Turn off all LEDs  */
	if(!o_stdout) {
		for (i=first_led; i < (first_led + number_of_leds); i++) {
			if (o_shift_register) {
				digitalWrite (100 + i, 0);
			} else {
				digitalWrite(i, 0);
			}
		}
	}
}

/* Connect the chosen port to ours */
static void connect_port(jack_client_t *client, char *port_name) {
	jack_port_t *port;

	// Get the port we are connecting to
	port = jack_port_by_name(client, port_name);
	if (port == NULL) {
		fprintf(stderr, "Can't find port '%s'\n", port_name);
		exit(1);
	}

	// Connect the port to our input port
	fprintf(stderr,"Connecting '%s' to '%s'...\n", jack_port_name(port), jack_port_name(input_port));
	if (jack_connect(client, jack_port_name(port), jack_port_name(input_port))) {
		fprintf(stderr, "Cannot connect port '%s' to '%s'\n", jack_port_name(port), jack_port_name(input_port));
		exit(1);
	}
}

/* Sleep for a fraction of a second */
static int fsleep( float secs ) {

//#ifdef HAVE_USLEEP
	return usleep( secs * 1000000 );
//#endif
}


/* Display how to use this program */
static int usage( const char * progname ) {
	fprintf(stderr, "jackPiledmeter version %s\n\n", VERSION);
	fprintf(stderr, "Usage %s [ -1 first_led ] [ -5 ] [ -c decay_speed ] [ -d ] [ -f freqency ] [ -i ] [ -n number_of_leds ] [ -o ] [ -p ] [ -r ref_level ] [ -s ] [ <port>, ... ]\n\n", progname);
	fprintf(stderr, "       -1      GPIO pin where the first LED (or shift register data pin) is connected, using wiringPi numbering scheme [0]\n");
	fprintf(stderr, "       -5      use 74×595 shift register(s) instead of discrete GPIO pins\n");
	fprintf(stderr, "       -c      decay speed (0 to disable) [4]\n");
	fprintf(stderr, "       -d      detach from the terminal and become a daemon\n");
	fprintf(stderr, "       -f      how often to update the meter per second [40]\n");
	fprintf(stderr, "       -i      read db values from standard input, not from a Jack server\n");
	fprintf(stderr, "       -o      send db values to standard output, not to the LEDs\n");
	fprintf(stderr, "       -n      number of connected LEDs [8]\n");
	fprintf(stderr, "       -p      peak hold\n");
	fprintf(stderr, "       -r      reference signal level for 0dB on the meter\n");
	fprintf(stderr, "       -s      single light, only one wandering light instead of a growing/shrinking bar of lights\n");
	fprintf(stderr, "       <port>  the JACK port(s) to monitor (multiple ports are mixed)\n");
	exit(1);
}

void display_meter( int db) {
	int i, j;
	int gpio_pin;
	int have_pin = 0;
	int current_state;
	int new_state;

/* Only the first <number_of_leds> values in the array are used.
   If you have more than 16 LEDs connected, the array has to be expanded. */
	//  int dbvalues[]={-1,-2,-3,-4, -5,-6,-8,-10,-12,-14,-16,-18,-20,-22,-24,-26}; // 1dB steps down to -6, then 2dB steps.
	int dbvalues[]={-1,-2,-3,-4, -5,-6,-7,-8,-9,-10,-11,-12,-13,-14,-15,-16}; // 1dB steps.
	//int dbvalues[]={-1,-3,-5,-7, -9,-11,-13,-15,-17,-19,-21,-23,-25,-27,-29,-31}; // 2dB steps. With 8 LEDs, will cover down to -15 dB.
	//int dbvalues[]={-1,-3,-6,-9,-12,-15,-18,-21,-24,-27,-30,-33,-36,-39,-42,-45}; // 3dB steps. With 8 LEDs, will cover down to -21 dB.
	//int dbvalues[]={-1,-3,-5,-7,-10,-15,-20,-25,-30,-35,-40,-50,-60,-70,-80,-90}; // Pseudo-logarithmic. With 8 LEDs, will cover down to -30 dB.
	static int lastLEDlevel;
	static int newLEDlevel;

	if (( db >= lastLEDlevel) || (o_decay == 0)) {
		dtime = 0;
		newLEDlevel = db; // Never miss a new peak
		peak_hold_db = db;
		peak_hold_decay = decay_len;
	} else {
		if (dtime++ > decay_led) {
			dtime = 0;
			newLEDlevel -= o_decay ;
			if (newLEDlevel < -144) newLEDlevel = -144;
		}
	}
	lastLEDlevel = newLEDlevel;
	if ( o_stdout) {
		printf("%i\n", db);
		fflush(stdout);
		return;
	}

	if (single_light) {
		// A single "wandering" light. First, turn all LEDs off ...
		for (i = first_led; i <= (first_led + number_of_leds); i++) {
			if (o_shift_register) {
				digitalWrite (100 + i, 0);
			} else {
				digitalWrite(i, 0);
			}
		}
		// ... then, turn one single LED on.
		for (i = first_led, j = (number_of_leds - 1 ); i < (first_led + number_of_leds); i++, j--) {
			if (newLEDlevel >= dbvalues[j]) {
				gpio_pin=i;	
				have_pin=1;
			}
		}
        	if (have_pin) {
			if (o_shift_register) {
				digitalWrite (100 + gpio_pin, 1);
			} else {
				digitalWrite(gpio_pin, 1);
			}
		}
	} else {
		// Turn several LEDs on.
		for (i = (first_led + number_of_leds - 1), j = 0; i >= first_led; i--, j++ ) {
			if (o_shift_register) {
				new_state = (newLEDlevel >= dbvalues[j])  ? 1 : 0;
				digitalWrite (100 + i, new_state);
			} else {
				//  To minimise flicker at high refresh rates, only write to a GPIO pin if its state should change.
				current_state = digitalRead(i);
				new_state = (newLEDlevel >= dbvalues[j])  ? 1 : 0;
				if (current_state != new_state) {
					digitalWrite(i, new_state);
				}
			}
		}
	}
	if (o_peak_hold && (peak_hold_decay-- > 0)) {
		for (i = (first_led + number_of_leds - 1), j = 0; i >= first_led; i--, j++ ) {
			if (peak_hold_db >= dbvalues[j]) {
				if (o_shift_register) {
					digitalWrite (100 + i, 1);
				} else {
					digitalWrite(i, 1);
				}
				break;
			}
		}
	}
}

int main(int argc, char *argv[]) {
	jack_status_t status;
	float ref_lev;
	int rate = 40;
	int opt;
	int i;
	int detach = 0;
	float db = 0;

	while ((opt = getopt(argc, argv, "b:5c:df:n:1:iopr:shv")) != -1) {
		switch (opt) {
			case 'b':
				o_decay_bias = atoi(optarg);
				if (o_decay_bias < 1) o_decay_bias = 1;
				fprintf(stderr,"Decay bias: %i\n", o_decay_bias);
				break;
			case '5':
				o_shift_register = 1;
				break;
			case 'd':
				detach = 1;
				break;
			case 'i':
				o_stdin = 1;
				break;
			case 'o':
				o_stdout = 1;
				break;
			case 'p':
				o_peak_hold = 1;
				break;
			case 'r':
				ref_lev = atof(optarg);
				fprintf(stderr,"Reference level: %.1fdB\n", ref_lev);
				bias = powf(10.0f, ref_lev * -0.05f);
				break;
			case 'f':
				rate = atoi(optarg);
				fprintf(stderr,"Updates per second: %d\n", rate);
				break;
			case 's':
				single_light = 1;
				o_decay = 2; // Single light needs more damping, otherwise it looks "nervous".
				break;
			case '1':
				first_led = atoi(optarg);
				// number_of_leds -= first_led;
				break;
			case 'n':
				number_of_leds = atoi(optarg);
				break;
			case 'c':
				o_decay = atoi(optarg);
				if (o_decay < 0) o_decay = 0;
				if (o_decay > 10) o_decay = 10;
				break;
			case 'h':
			case 'v':
			default:
				/* Show usage/version information */
				usage( argv[0] );
				break;
		}
	}
        /* Detach from terminal? */
        if (detach) {
                pid_t child = fork();
                if (child < 0) {
                        perror("Could not detach from terminal");
                        exit(1);
                }
                if (child) {
                        /* I am the parent */
                        status = EXIT_SUCCESS;
                        exit(status);
                }
        }
	/* We catch these signals so we can clean up */
	{
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_handler = graceful_exit;
		sigemptyset(&action.sa_mask);
		action.sa_flags = 0; /* We block on usleep; don't use SA_RESTART */
		sigaction(SIGHUP, &action, NULL);
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);
	}

	if (! o_stdout) {
		if (wiringPiSetup() == -1) {
			fprintf(stderr, "wiringPiSetup() failed!\n");
			exit(1);
		}
		if (o_shift_register) {
			// Set up wiringPi pins for data, clock and latch.
			 sr595Setup (100, number_of_leds, first_led, first_led + 1, first_led + 2) ;
		}

		// Initalise GPIO pins and turn all LEDs off.
		for (i = first_led; i < (first_led + number_of_leds); i++) {
			if (o_shift_register) {
				digitalWrite (100 + i, 0);
			} else {
				pinMode(i, OUTPUT);
				digitalWrite(i, 0);
			}
		}
		// Spin through all LEDs, to show that they work and are connected in the right order.
		// Spin up
		for (i = first_led; i < (first_led + number_of_leds); i++) {
			if (o_shift_register) {
				digitalWrite (100 + i, 1);
				delay(100);
				digitalWrite (100 + i, 0);
			} else {
				digitalWrite(i, 1);
				delay(100);
				digitalWrite(i, 0);
			}
		}
		// Spin down
		for (i = (first_led + number_of_leds); i > first_led; i--) {
			if (o_shift_register) {
				digitalWrite (100 + i, 1);
				delay(100);
				digitalWrite (100 + i, 0);
			} else {
				digitalWrite(i, 1);
				delay(100);
				digitalWrite(i, 0);
			}
		}
	
	}
	// Register the cleanup function to be called when program exits
	atexit( cleanup );

	// Calculate the decay length (should be 1600ms)
	decay_len = (int)(1.6f / (1.0f/rate));
	decay_led = decay_len / rate * o_decay_bias;

	if (! o_stdin) {
		// Register with Jack
		if ((client = jack_client_open("ledmeter", JackNullOption, &status)) == 0) {
			fprintf(stderr, "Failed to start jack client: %d\n", status);
			exit(1);
		}
		fprintf(stderr,"Registering as '%s'.\n", jack_get_client_name( client ) );
	
		// Create our input port
		if (!(input_port = jack_port_register(client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
			fprintf(stderr, "Cannot register input port 'meter'.\n");
			exit(1);
		}
		
		// Register the peak signal callback
		jack_set_process_callback(client, process_peak, 0);
	
	
		if (jack_activate(client)) {
			fprintf(stderr, "Cannot activate client.\n");
			exit(1);
		}
	
	
		// Connect our port to specified port(s)
		if (argc > optind) {
			while (argc > optind) {
				connect_port( client, argv[ optind ] );
				optind++;
			}
		} else {
			fprintf(stderr,"Meter is not connected to a port.\n");
		}
	
		/* Loop until signal received */
		while (running) {
			db = 20.0f * log10f(read_peak() * bias);
	
		// If db is less than -144 (typically -infinity), it means that no data is available.
			if (db < -144.0f) {
				if (rr_too_high  < 2 ) {	// Show error message once only.
					if (rr_too_high > 0) 	// First buffer is always empty.
						fprintf(stderr, "Empty data buffer detected. Refresh rate too high or Jack latency too large?\n");
					rr_too_high++;
				}
			}
			if (db >= -144.0f)		
				display_meter( db );
			fsleep( 1.0f/rate );
		}
	} else { // ! o_stdin
		while (running) {
			fscanf(stdin, "%f", &db);
			display_meter( db );
		}
	}

	return 0;
}

