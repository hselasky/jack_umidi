/*-
 * Copyright (c) 2011-2018 Hans Petter Selasky <hselasky@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * A few parts of this file has been copied from Edward Tomasz
 * Napierala's jack-keyboard sources.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <err.h>
#include <sysexits.h>
#include <sys/errno.h>
#ifdef HAVE_SYSCTL
#include <sys/sysctl.h>
#endif
#include <pwd.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#define	PACKAGE_NAME		"jack_umidi"
#define	PACKAGE_VERSION		"1.0.9"

#define	JACK_OUT_MAX	17		/* units */

static jack_port_t *output_port[JACK_OUT_MAX];
static jack_port_t *input_port;
static jack_client_t *jack_client;
static uint8_t read_buffer[16];
static int read_offset;
static int read_len;
static int read_fd = -1;
static int write_fd = -1;
static int kill_on_close;
static int background;
static int create_sub;
static char *read_name;
static char *write_name;
static char *port_name;
static pthread_mutex_t umidi_mtx;
static uid_t uid;
static int uid_found;

struct midi_parse {
	uint8_t *temp_cmd;
	uint8_t	temp_0[4];
	uint8_t	temp_1[4];
	uint8_t	state;
#define	UMIDI_ST_UNKNOWN   0		/* scan for command */
#define	UMIDI_ST_1PARAM    1
#define	UMIDI_ST_2PARAM_1  2
#define	UMIDI_ST_2PARAM_2  3
#define	UMIDI_ST_SYSEX_0   4
#define	UMIDI_ST_SYSEX_1   5
#define	UMIDI_ST_SYSEX_2   6
};

static struct midi_parse midi_parse;

#ifdef HAVE_DEBUG
#define	DPRINTF(fmt, ...) printf("%s:%d: " fmt, __FUNCTION__, __LINE__,## __VA_ARGS__)
#else
#define	DPRINTF(fmt, ...) do { } while (0)
#endif

static void
umidi_lock()
{
	pthread_mutex_lock(&umidi_mtx);
}

static void
umidi_unlock()
{
	pthread_mutex_unlock(&umidi_mtx);
}

static uid_t
umidi_id(const char *name, const char *type)
{
	uid_t val;
	char *ep;

	val = strtoul(name, &ep, 10);
	if (*ep != '\0')
		errx(EX_USAGE, "%s: illegal %s name", name, type);
	return (val);
}

static void
umidi_uid(const char *s)
{
	struct passwd *pw;

	uid = ((pw = getpwnam(s)) != NULL) ? pw->pw_uid : umidi_id(s, "user");
	uid_found = 1;
}

static void
umidi_write(jack_nframes_t nframes)
{
	int error;
	int events;
	int i;
	void *buf;
	jack_midi_event_t event;

	if (input_port == NULL)
		return;

	buf = jack_port_get_buffer(input_port, nframes);
	if (buf == NULL) {
		DPRINTF("jack_port_get_buffer() failed, cannot receive anything.\n");
		return;
	}
#ifdef JACK_MIDI_NEEDS_NFRAMES
	events = jack_midi_get_event_count(buf, nframes);
#else
	events = jack_midi_get_event_count(buf);
#endif

	for (i = 0; i < events; i++) {
#ifdef JACK_MIDI_NEEDS_NFRAMES
		error = jack_midi_event_get(&event, buf, i, nframes);
#else
		error = jack_midi_event_get(&event, buf, i);
#endif
		if (error) {
			DPRINTF("jack_midi_event_get() failed, lost MIDI event.\n");
			continue;
		}
		umidi_lock();
		if (write_fd > -1) {
			if (write(write_fd, event.buffer, event.size) != event.size) {
				DPRINTF("write() failed.\n");
			}
		}
		umidi_unlock();
	}
}

static const uint8_t umidi_cmd_to_len[16] = {
	[0x0] = 0,			/* reserved */
	[0x1] = 0,			/* reserved */
	[0x2] = 2,			/* bytes */
	[0x3] = 3,			/* bytes */
	[0x4] = 3,			/* bytes */
	[0x5] = 1,			/* bytes */
	[0x6] = 2,			/* bytes */
	[0x7] = 3,			/* bytes */
	[0x8] = 3,			/* bytes */
	[0x9] = 3,			/* bytes */
	[0xA] = 3,			/* bytes */
	[0xB] = 3,			/* bytes */
	[0xC] = 2,			/* bytes */
	[0xD] = 2,			/* bytes */
	[0xE] = 3,			/* bytes */
	[0xF] = 1,			/* bytes */
};

/*
 * The following statemachine, that converts MIDI commands to
 * USB MIDI packets, derives from Linux's usbmidi.c, which
 * was written by "Clemens Ladisch":
 *
 * Returns:
 *    0: No command
 * Else: Command is complete
 */
static uint8_t
umidi_convert_to_usb(uint8_t cn, uint8_t b)
{
	uint8_t p0 = (cn << 4);

	if (b >= 0xf8) {
		midi_parse.temp_0[0] = p0 | 0x0f;
		midi_parse.temp_0[1] = b;
		midi_parse.temp_0[2] = 0;
		midi_parse.temp_0[3] = 0;
		midi_parse.temp_cmd = midi_parse.temp_0;
		return (1);

	} else if (b >= 0xf0) {
		switch (b) {
		case 0xf0:		/* system exclusive begin */
			midi_parse.temp_1[1] = b;
			midi_parse.state = UMIDI_ST_SYSEX_1;
			break;
		case 0xf1:		/* MIDI time code */
		case 0xf3:		/* song select */
			midi_parse.temp_1[1] = b;
			midi_parse.state = UMIDI_ST_1PARAM;
			break;
		case 0xf2:		/* song position pointer */
			midi_parse.temp_1[1] = b;
			midi_parse.state = UMIDI_ST_2PARAM_1;
			break;
		case 0xf4:		/* unknown */
		case 0xf5:		/* unknown */
			midi_parse.state = UMIDI_ST_UNKNOWN;
			break;
		case 0xf6:		/* tune request */
			midi_parse.temp_1[0] = p0 | 0x05;
			midi_parse.temp_1[1] = 0xf6;
			midi_parse.temp_1[2] = 0;
			midi_parse.temp_1[3] = 0;
			midi_parse.temp_cmd = midi_parse.temp_1;
			midi_parse.state = UMIDI_ST_UNKNOWN;
			return (1);

		case 0xf7:		/* system exclusive end */
			switch (midi_parse.state) {
			case UMIDI_ST_SYSEX_0:
				midi_parse.temp_1[0] = p0 | 0x05;
				midi_parse.temp_1[1] = 0xf7;
				midi_parse.temp_1[2] = 0;
				midi_parse.temp_1[3] = 0;
				midi_parse.temp_cmd = midi_parse.temp_1;
				midi_parse.state = UMIDI_ST_UNKNOWN;
				return (1);
			case UMIDI_ST_SYSEX_1:
				midi_parse.temp_1[0] = p0 | 0x06;
				midi_parse.temp_1[2] = 0xf7;
				midi_parse.temp_1[3] = 0;
				midi_parse.temp_cmd = midi_parse.temp_1;
				midi_parse.state = UMIDI_ST_UNKNOWN;
				return (1);
			case UMIDI_ST_SYSEX_2:
				midi_parse.temp_1[0] = p0 | 0x07;
				midi_parse.temp_1[3] = 0xf7;
				midi_parse.temp_cmd = midi_parse.temp_1;
				midi_parse.state = UMIDI_ST_UNKNOWN;
				return (1);
			}
			midi_parse.state = UMIDI_ST_UNKNOWN;
			break;
		}
	} else if (b >= 0x80) {
		midi_parse.temp_1[1] = b;
		if ((b >= 0xc0) && (b <= 0xdf)) {
			midi_parse.state = UMIDI_ST_1PARAM;
		} else {
			midi_parse.state = UMIDI_ST_2PARAM_1;
		}
	} else {			/* b < 0x80 */
		switch (midi_parse.state) {
		case UMIDI_ST_1PARAM:
			if (midi_parse.temp_1[1] < 0xf0) {
				p0 |= midi_parse.temp_1[1] >> 4;
			} else {
				p0 |= 0x02;
				midi_parse.state = UMIDI_ST_UNKNOWN;
			}
			midi_parse.temp_1[0] = p0;
			midi_parse.temp_1[2] = b;
			midi_parse.temp_1[3] = 0;
			midi_parse.temp_cmd = midi_parse.temp_1;
			return (1);
		case UMIDI_ST_2PARAM_1:
			midi_parse.temp_1[2] = b;
			midi_parse.state = UMIDI_ST_2PARAM_2;
			break;
		case UMIDI_ST_2PARAM_2:
			if (midi_parse.temp_1[1] < 0xf0) {
				p0 |= midi_parse.temp_1[1] >> 4;
				midi_parse.state = UMIDI_ST_2PARAM_1;
			} else {
				p0 |= 0x03;
				midi_parse.state = UMIDI_ST_UNKNOWN;
			}
			midi_parse.temp_1[0] = p0;
			midi_parse.temp_1[3] = b;
			midi_parse.temp_cmd = midi_parse.temp_1;
			return (1);
		case UMIDI_ST_SYSEX_0:
			midi_parse.temp_1[1] = b;
			midi_parse.state = UMIDI_ST_SYSEX_1;
			break;
		case UMIDI_ST_SYSEX_1:
			midi_parse.temp_1[2] = b;
			midi_parse.state = UMIDI_ST_SYSEX_2;
			break;
		case UMIDI_ST_SYSEX_2:
			midi_parse.temp_1[0] = p0 | 0x04;
			midi_parse.temp_1[3] = b;
			midi_parse.temp_cmd = midi_parse.temp_1;
			midi_parse.state = UMIDI_ST_SYSEX_0;
			return (1);
		default:
			break;
		}
	}
	return (0);
}

/*
 * By using a read buffer we avoid multiple sys-calls for a single
 * MIDI command.
 */
static int
read_byte(int fd, uint8_t *buf)
{
top:
	if (read_offset < read_len) {
		*buf = read_buffer[read_offset];
		read_offset++;
		return (1);
	}
	read_len = read(fd, read_buffer, sizeof(read_buffer));
	read_offset = 0;
	if (read_len > 0) {
		DPRINTF("got %d MIDI bytes\n", read_len);
		goto top;
	}
	*buf = 0;
	read_len = 0;
	return (-1);
}

static void
umidi_read(jack_nframes_t nframes)
{
	uint8_t *buffer;
	void *buf[JACK_OUT_MAX];
	jack_nframes_t t[JACK_OUT_MAX];
	uint32_t x;
	uint8_t data;
	uint8_t len;

	for (x = 0; x != JACK_OUT_MAX; x++) {
		if (output_port[x] == NULL) {
			buf[x] = NULL;
			continue;
		}
		buf[x] = jack_port_get_buffer(output_port[x], nframes);
		if (buf[x] != NULL) {
#ifdef JACK_MIDI_NEEDS_NFRAMES
			jack_midi_clear_buffer(buf[x], nframes);
#else
			jack_midi_clear_buffer(buf[x]);
#endif
		} else {
			DPRINTF("jack_port_get_buffer() failed, "
			    "cannot send anything on unit %d.\n", (int)x);
		}
	}

	memset(t, 0, sizeof(t));

	umidi_lock();
	if (read_fd > -1) {
		while ((t[0] < nframes) &&
		    (read_byte(read_fd, &data) == 1)) {
			if (umidi_convert_to_usb(0, data)) {

				len = umidi_cmd_to_len[midi_parse.temp_cmd[0] & 0xF];
				if (len == 0)
					continue;

				for (x = 0; x != JACK_OUT_MAX; x++) {

					if (buf[x] == NULL ||
					    t[x] >= nframes) {
						DPRINTF("Buffer full. "
						    "MIDI event lost\n");
						continue;
					}
					if (x == 0) {
						/* pass all */
					} else if (create_sub == 0) {
						break;
					} else if (midi_parse.temp_cmd[1] >= 0x80 &&
					    midi_parse.temp_cmd[1] <= 0xEF) {
						/* filter based on channel */
						if ((midi_parse.temp_cmd[1] & 0x0F) != (x - 1))
							continue;
					} else {
						/*
						 * non-channel events are
						 * dropped
						 */
						continue;
					}
#ifdef JACK_MIDI_NEEDS_NFRAMES
					buffer = jack_midi_event_reserve(buf[x], t[x], len, nframes);
#else
					buffer = jack_midi_event_reserve(buf[x], t[x], len);
#endif
					if (buffer == NULL) {
						DPRINTF("jack_midi_event_reserve() failed, "
						    "MIDI event lost\n");
						continue;
					}
					memcpy(buffer, &midi_parse.temp_cmd[1], len);
					t[x]++;
				}
			}
		}
	}
	umidi_unlock();
}

static int
umidi_process_callback(jack_nframes_t nframes, void *reserved)
{
	/*
	 * Check for impossible condition that actually happened to me,
	 * caused by some problem between jackd and OSS4.
	 */
	if (nframes <= 0) {
		DPRINTF("Process callback called with nframes = 0\n");
		return (0);
	}
	umidi_read(nframes);
	umidi_write(nframes);

	return (0);
}

static void
umidi_jack_shutdown(void *arg)
{
	exit(0);
}

static void
umidi_watchdog_sub(void)
{
	int fd;

	if (read_name == NULL) {
		/* do nothing */
	} else if (read_fd < 0) {
		fd = open(read_name, O_RDONLY | O_NONBLOCK);
		if (fd > -1) {
			umidi_lock();
			read_fd = fd;
			fcntl(read_fd, F_SETFL, (int)O_NONBLOCK);
			umidi_unlock();
		}
	} else {
		umidi_lock();
		if (fcntl(read_fd, F_SETFL, (int)O_NONBLOCK) == -1) {
			DPRINTF("Close read\n");
			close(read_fd);
			read_fd = -1;
			read_offset = 0;
			read_len = 0;
			memset(read_buffer, 0, sizeof(read_buffer));
		}
		umidi_unlock();
	}

	if (write_name == NULL) {
		/* do nothing */
	} else if (write_fd < 0) {
		fd = open(write_name, O_WRONLY);
		if (fd > -1) {
			umidi_lock();
			write_fd = fd;
			fcntl(write_fd, F_SETFL, (int)0);
			umidi_unlock();
		}
	} else {
		umidi_lock();
		if (fcntl(write_fd, F_SETFL, (int)0) == -1) {
			DPRINTF("Close write\n");
			close(write_fd);
			write_fd = -1;
		}
		umidi_unlock();
	}
}

static void *
umidi_watchdog(void *arg)
{
	while (1) {
		/* try to open MIDI device */
		umidi_watchdog_sub();

		if (kill_on_close != 0) {
			if (write_name != NULL && write_fd == -1)
				exit(0);
			if (read_name != NULL && read_fd == -1)
				exit(0);
		}
		usleep(1000000);
	}
}

static void
usage()
{
	fprintf(stderr,
	    "jack_umidi - RAW USB/socket MIDI client v" PACKAGE_VERSION "\n"
	    "	-d /dev/umidi0.0 (set capture and playback device)\n"
	    "	-C /dev/umidi0.0 (set capture device)\n"
	    "	-P /dev/umidi0.0 (set playback device)\n"
	    "	-S (create per channel subdevices for capture device)\n"
	    "	-U <username> (attach to this JACK user)\n"
	    "	-B (run in background)\n"
	    "	-k (terminate client if a device goes away)\n"
	    "	-n jack_umidi (specify port name)\n"
	    "	-h (show help)\n");
	exit(0);
}

static void
umidi_pipe(int dummy)
{

}

int
main(int argc, char **argv)
{
	int error;
	int c;
	int unit = -1;
	int subunit = -1;
	unsigned int x;
	const char *pname;
	char devname[128];

	while ((c = getopt(argc, argv, "U:kBd:hP:SC:n:")) != -1) {
		switch (c) {
		case 'k':
			kill_on_close = 1;
			break;
		case 'B':
			background = 1;
			break;
		case 'd':
			free(read_name);
			free(write_name);
			read_name = strdup(optarg);
			write_name = strdup(optarg);
			break;
		case 'P':
			free(write_name);
			write_name = strdup(optarg);
			break;
		case 'C':
			free(read_name);
			read_name = strdup(optarg);
			break;
		case 'n':
			free(port_name);
			port_name = strdup(optarg);
			break;
		case 'S':
			create_sub = 1;
			break;
		case 'U':
			umidi_uid(optarg);
			break;
		case 'h':
		default:
			usage();
		}
	}

	if (read_name == NULL && write_name == NULL)
		usage();

	if (background) {
		if (daemon(0, 0))
			errx(EX_UNAVAILABLE, "Could not become daemon");
	}
	signal(SIGPIPE, umidi_pipe);

	pthread_mutex_init(&umidi_mtx, NULL);

	/* try to open MIDI device early on */
	umidi_watchdog_sub();

	if (uid_found != 0 && setuid(uid) != 0)
		errx(EX_UNAVAILABLE, "Could not set user ID");

	if (read_name != NULL) {
		if (strncmp(read_name, "/dev/", 5) == 0) {
			pname = read_name + 5;
			sscanf(read_name, "/dev/umidi%d.%d", &unit, &subunit);
		} else {
			pname = read_name;
		}
	} else {
		if (strncmp(write_name, "/dev/", 5) == 0) {
			pname = write_name + 5;
			sscanf(write_name, "/dev/umidi%d.%d", &unit, &subunit);
		} else {
			pname = write_name;
		}
	}

#ifdef HAVE_SYSCTL
	if (unit > -1 && port_name == NULL) {
		size_t size = sizeof(devname);

		/* create sysctl name */
		snprintf(devname, sizeof(devname),
		    "dev.uaudio.%d.%%desc", unit);

		/* lookup sysctl */
		if (sysctlbyname(devname, devname, &size, NULL, 0) == 0 ||
		    (errno == ENOMEM)) {
			char *ptr;

			/* check string length */
			if (size > sizeof(devname) - 1)
				size = sizeof(devname) - 1;

			/* zero terminate */
			devname[size] = 0;

			/* split */
			ptr = strchr(devname, ',');
			if (ptr != NULL) {
				size = ptr - devname;
				*ptr = 0;
			}
			/* limit the string length */
			if (strlen(devname) > 16) {
				devname[16] = 0;
				size = 16;
			}
			/* append port number */
			snprintf(devname + size, sizeof(devname) - size,
			    " #%d", subunit);
		} else {
			snprintf(devname, sizeof(devname), "%s-%s",
			    PACKAGE_NAME, pname);
		}
	} else {
#endif
		snprintf(devname, sizeof(devname), "%s-%s",
		    (port_name != NULL) ? port_name : PACKAGE_NAME, pname);
#ifdef HAVE_SYSCTL
	}
#endif

	jack_client = jack_client_open(devname,
	    JackNoStartServer, NULL);
	if (jack_client == NULL) {
		errx(EX_UNAVAILABLE, "Could not connect "
		    "to the JACK server. Run jackd first?");
	}
	error = jack_set_process_callback(jack_client,
	    umidi_process_callback, 0);
	if (error) {
		errx(EX_UNAVAILABLE, "Could not register "
		    "JACK process callback.");
	}
	jack_on_shutdown(jack_client, umidi_jack_shutdown, 0);

	if (read_name != NULL) {
		output_port[0] = jack_port_register(
		    jack_client, "midi.TX", JACK_DEFAULT_MIDI_TYPE,
		    JackPortIsOutput | JackPortIsPhysical |
		    JackPortIsTerminal, 0);

		if (output_port[0] == NULL) {
			errx(EX_UNAVAILABLE, "Could not "
			    "register JACK output port.");
		}
		if (create_sub != 0) {
			char name[16];

			for (x = 1; x != JACK_OUT_MAX; x++) {
				snprintf(name, sizeof(name),
				    "midi.TX.CH%X", x - 1);

				output_port[x] = jack_port_register(
				    jack_client, name, JACK_DEFAULT_MIDI_TYPE,
				    JackPortIsPhysical | JackPortIsTerminal |
				    JackPortIsOutput, 0);

				if (output_port[x] == NULL) {
					errx(EX_UNAVAILABLE, "Could not "
					    "register JACK output port,"
					    " %s.", name);
				}
			}
		}
	}
	if (write_name != NULL) {

		input_port = jack_port_register(
		    jack_client, "midi.RX", JACK_DEFAULT_MIDI_TYPE,
		    JackPortIsInput | JackPortIsPhysical |
		    JackPortIsTerminal, 0);

		if (input_port == NULL) {
			errx(EX_UNAVAILABLE, "Could not "
			    "register JACK input port.");
		}
	}
	if (jack_activate(jack_client))
		errx(EX_UNAVAILABLE, "Cannot activate JACK client.");

	umidi_watchdog(NULL);

	return (0);
}
