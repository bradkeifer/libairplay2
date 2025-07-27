/*
 * AirPlay2 : Client to control an AirPlay2 device, cli simple example
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * This code has been derived from from https://github.com/music-assistant/libraop
 * Earlier works:
 * 	Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * 	Philippe <philippe_44@outlook.com>
 *
 */
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "platform.h"
#include <assert.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <event2/event.h>
#include <event2/thread.h>

#if WIN
#include <conio.h>
#include <time.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/param.h>
#include <sys/time.h>
#if OSX || FREEBDS
#include <sys/resource.h>
#endif
#endif

// from cliraop - try to eliminate
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_util.h"
#include "cross_log.h"
#include "http_fetcher.h"

// from owntones
#include "logger.h"
#include "outputs.h"
#include "airplay.h"
#include "mdns.h"


// try to remove as much of below as possible
#define AIRPLAY2_SEC(ntp) ((uint32_t)((ntp) >> 32))
#define AIRPLAY2_FRAC(ntp) ((uint32_t)(ntp))
#define AIRPLAY2_SECNTP(ntp) AIRPLAY2_SEC(ntp), AIRPLAY2_FRAC(ntp)

bool startsWith(const char *pre, const char *str)
{
	size_t lenpre = strlen(pre),
		   lenstr = strlen(str);
	return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}

/* ----------------------- From owntones/main.c ------------------------------*/
struct event_base *evbase_main;

static struct output_device ap_device; // Holds the Airplay device info
static struct airplay_extra ap_extra; // Hold the extra Airplay device info

// locals
static bool glMainRunning = true;
static pthread_t glCmdPipeReaderThread;
char cmdPipeName[32];
int cmdPipeFd;
char cmdPipeBuf[512];
int latency = MS2TS(1000, 44100);
struct airplay2cl_s *airplay2cl;
enum
{
	STOPPED,
	PAUSED,
	PLAYING
} status;

// seek to delete this cliraop log_level stuff and use owntones logging solution
// debug level from tools & other elements
log_level util_loglevel;
log_level airplay2_loglevel;
log_level main_log;

// our debug level
log_level *loglevel = &main_log;

// different combination of debug levels per channel
struct debug_s
{
	int main, airplay2, util;
} debug[] = {
	{lSILENCE, lSILENCE, lSILENCE},
	{lERROR, lERROR, lERROR},
	{lINFO, lERROR, lERROR},
	{lINFO, lINFO, lERROR},
	{lDEBUG, lERROR, lERROR},
	{lDEBUG, lINFO, lERROR},
	{lDEBUG, lDEBUG, lERROR},
	{lSDEBUG, lINFO, lERROR},
	{lSDEBUG, lDEBUG, lERROR},
	{lSDEBUG, lSDEBUG, lERROR},
};

static int platform_init(void) {
	int ret;
  /* Initialize event base */
  CHECK_NULL(L_MAIN, evbase_main = event_base_new());

  CHECK_ERR(L_MAIN, evthread_use_pthreads());

  ret = mdns_init();
  if (ret != 0) 
    {
      DPRINTF(E_FATAL, L_MAIN, "mDNS init failed\n");

      ret = EXIT_FAILURE;
      goto mdns_fail;
    }

	// TODO <@bradkeifer> - signal handling

mdns_fail:
  event_base_free(evbase_main);

  return ret;
}	

static void platform_deinit(void) {
  DPRINTF(E_DBG, L_MAIN, "Deinitializing platform\n");

  mdns_deinit();

  event_base_free(evbase_main);
}

/*----------------------------------------------------------------------------*/
static int print_usage(char *argv[])
{
	char *name = strrchr(argv[0], '\\');

	name = (name) ? name + 1 : argv[0];

	printf("usage: %s <options> <player_ip> <filename ('-' for stdin)>\n"
		   "\t[-ntp print current NTP and exit\n"
		   "\t[-check print check info and exit\n"
		   "\t[-port <port number>] (defaults to 5000)\n"
		   "\t[-volume <volume> (0-100)]\n"
		   "\t[-latency <latency> (frames]\n"
		   "\t[-wait <wait>]  (start after <wait> milliseconds)\n"
		   "\t[-ntpstart <start>] (start at NTP <start> + <wait>)\n"
		   "\t[-encrypt] audio payload encryption\n"
		   "\t[-dacp <dacp_id>] (DACP id)\n"
		   "\t[-activeremote <activeremote_id>] (Active Remote id)\n"
		   "\t[-alac] send ALAC compressed audio\n"

		   "\t[-et <value>] (et in mDNS: 4 for airport-express and used to detect MFi)\n"
		   "\t[-md <value>] (md in mDNS: metadata capabilties 0=text, 1=artwork, 2=progress)\n"
		   "\t[-am <value>] (am in mDNS: modelname)\n"
		   "\t[-pk <value>] (pk in mDNS: pairing key info)\n"
		   "\t[-pw <value>] (pw in mDNS: password info)\n"

		   "\t[-secret <secret>] (valid secret for AppleTV)\n"
		   "\t[-password <password>] (device password)\n"
		   "\t[-udn <UDN>] (UDN name in mdns, required for password)\n"

		   "\t[-if <ipaddress>] (IP of the interface to bind to)\n"

		   "\t[-debug <debug level>] (0 = FATAL, 5 = SPAM)\n",
		   name);
	return -1;
}

/*----------------------------------------------------------------------------*/
static void *CmdPipeReaderThread(void *args)
{
	cmdPipeFd = open(cmdPipeName, O_RDONLY);
	struct
	{
		char *title;
		char *artist;
		char *album;
		int duration;
		int progress;
	} metadata = {"", "", "", 0, 0};

	// Read and print line from named pipe
	while (glMainRunning)
	{
		if (!glMainRunning)
			break;

		if (read(cmdPipeFd, cmdPipeBuf, 512) > 0)
		{
			// read lines
			char *save_ptr1, *save_ptr2;
			char *line = strtok_r(cmdPipeBuf, "\n", &save_ptr1);
			// loop through the string to extract all other tokens
			while (line != NULL)
			{
				if (!glMainRunning)
					break;

				DPRINTF(E_DBG, L_MAIN, "Received line on named pipe %s\n", line);
				// extract key-value pair within line
				char *key = strtok_r(line, "=", &save_ptr2);
				if (strlen(key) == 0)
					continue;
				char *value = strtok_r(NULL, "", &save_ptr2);
				if (value == NULL)
					value = "";

				if (strcmp(key, "TITLE") == 0)
				{
					metadata.title = value ? value : "";
				}
				else if (strcmp(key, "ARTIST") == 0)
				{
					metadata.artist = value ? value : "";
				}
				else if (strcmp(key, "ALBUM") == 0)
				{
					metadata.album = value ? value : "";
				}
				else if (strcmp(key, "DURATION") == 0)
				{
					metadata.duration = atoi(value);
				}
				else if (strcmp(key, "PROGRESS") == 0)
				{
					metadata.progress = atoi(value);
					// airplay2cl_set_progress_ms(airplay2cl, metadata.progress * 1000, metadata.duration * 1000);
				}
				else if (strcmp(key, "ARTWORK") == 0)
				{
					if (startsWith("http://", value))
					{
						DPRINTF(E_DBG, L_MAIN, "Downloading artwork from URL: %s\n", value);
						char *contentType;
						char *content;
						int size = http_fetch(value, &contentType, &content);
						if (size > 0 && glMainRunning)
						{
							DPRINTF(E_INFO, L_MAIN, "Sending artwork to player...\n");
							// airplay2cl_set_artwork(airplay2cl, contentType, size, content);
							free(content);
						}
						else
						{
							DPRINTF(E_WARN, L_MAIN, "Unable to download artwork %s\n", value);
						}
					}
					else if (access(value, F_OK) == 0)
					{
						// local file
						DPRINTF(E_DBG, L_MAIN, "Setting artwork from file: %s\n", value);
						FILE *infile;
						char *buffer;
						long numbytes;
						infile = fopen(value, "r");
						fseek(infile, 0L, SEEK_END);
						numbytes = ftell(infile);
						fseek(infile, 0L, SEEK_SET);
						buffer = (char *)calloc(numbytes, sizeof(char));
						fread(buffer, sizeof(char), numbytes, infile);
						fclose(infile);
						// airplay2cl_set_artwork(airplay2cl, "image/jpg", numbytes, buffer);
						free(buffer);
					}
					else
					{
						DPRINTF(E_WARN, L_MAIN, "Unable to process artwork path: %s\n", value);
					}
				}
				else if (strcmp(key, "VOLUME") == 0)
				{
					DPRINTF(E_INFO, L_MAIN, "Setting volume to: %s\n", value);
					// airplay2cl_set_volume(airplay2cl, airplay2cl_float_volume(atoi(value)));
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PAUSE") == 0)
				{
					if (status == PLAYING)
					{
						// airplay2cl_pause(airplay2cl);
						// airplay2cl_flush(airplay2cl);
						status = PAUSED;
						// LOG_INFO("Pause at : %u.%u", AIRPLAY2_SECNTP(airplay_get_ntp(NULL)));
						DPRINTF(E_INFO, L_MAIN, "Paused\n");
					}
					else
					{
						DPRINTF(E_WARN, L_MAIN, "Pause requested but player is already paused\n");
					}
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PLAY") == 0)
				{
					// uint64_t now = airplay_get_ntp(NULL);
					// uint64_t start_at = now + MS2NTP(200) - TS2NTP(latency, airplay2cl_sample_rate(airplay2cl));
					status = PLAYING;
					// airplay2cl_start_at(airplay2cl, start_at);
					// LOG_INFO("Re-started at : %u.%u", AIRPLAY2_SECNTP(start_at));
					DPRINTF(E_INFO, L_MAIN, "Re-started\n");
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "STOP") == 0)
				{
					status = STOPPED;
					// LOG_INFO("Stopped at : %u.%u", AIRPLAY2_SECNTP(airplay_get_ntp(NULL)));
					DPRINTF(E_INFO, L_MAIN, "Stopped\n");
					// airplay2cl_stop(airplay2cl);
					break;
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "SENDMETA") == 0)
				{
					// LOG_INFO("Sending metadata: %p", metadata);
					DPRINTF(E_INFO, L_MAIN, "Sending metadata\n");
					// airplay2cl_set_daap(airplay2cl, 4, "minm", 's', metadata.title,
									// "asar", 's', metadata.artist,
									// "asal", 's', metadata.album,
									// "astn", 'i', 1);
				}

				// read next line in cmdPipeBuf
				line = strtok_r(NULL, "\n", &save_ptr1);
			}

			// clear cmdPipeBuf
			memset(cmdPipeBuf, 0, sizeof cmdPipeBuf);
		}
		else
		{
			usleep(250 * 1000);
		}
	}

	return NULL;
}
/*																		  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	char *glDACPid = "1A2B3D4EA1B2C3D4";
	char *activeRemote = "ap5918800d";
	char *fname = NULL;
	int volume = 0, wait = 0;
	struct
	{
		struct hostent *hostent;
		char *hostname;
		int port;
		char *udn;
		struct in_addr addr;
	} player = {0};
	player.port = 5000;

	int infile;
	uint8_t *buf;
	int i, n = -1, level = 3;
	// airplay2_crypto_t crypto = AIRPLAY2_CLEAR;
	uint64_t start = 0, start_at = 0, last = 0, frames = 0;
	bool alac = false, encryption = false, auth = false;
	char *passwd = "", *secret = "", *md = "0,1,2", *et = "0,4", *am = "", *pk = "", *pw = "";
	char *iface = NULL;
	uint32_t glNetmask;
	char glInterface[16] = "?";
	static struct in_addr glHost;
	int ret;

	// TODO: <@bradkeifer>  refactor this to same method used in owntones (getopt)
	// parse arguments
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-ntp"))
		{
			uint64_t t = airplay_get_ntp(NULL);
			printf("%" PRIu64 "\n", t);
			exit(0);
		}
		if (!strcmp(argv[i], "-check"))
		{
			printf("cliairplay2 check\n");
			exit(0);
		}
		if (!strcmp(argv[i], "-port"))
		{
			player.port = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-volume"))
		{
			volume = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-latency"))
		{
			latency = MS2TS(atoi(argv[++i]), 44100);
		}
		else if (!strcmp(argv[i], "-wait"))
		{
			wait = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-ntpstart"))
		{
			sscanf(argv[++i], "%" PRIu64, &start);
		}
		else if (!strcmp(argv[i], "-encrypt"))
		{
			encryption = true;
		}
		else if (!strcmp(argv[i], "-dacp"))
		{
			glDACPid = argv[++i];
		}
		else if (!strcmp(argv[i], "-activeremote"))
		{
			activeRemote = argv[++i];
		}
		else if (!strcmp(argv[i], "-alac"))
		{
			alac = true;
		}
		else if (!strcmp(argv[i], "-et"))
		{
			et = argv[++i];
		}
		else if (!strcmp(argv[i], "-md"))
		{
			md = argv[++i];
		}
		else if (!strcmp(argv[i], "-am"))
		{
			am = argv[++i];
		}
		else if (!strcmp(argv[i], "-pk"))
		{
			pk = argv[++i];
		}
		else if (!strcmp(argv[i], "-pw"))
		{
			pw = argv[++i];
		}
		else if (!strcmp(argv[i], "-if"))
		{
			strcpy(glInterface, argv[++i]);
		}
		else if (!strcmp(argv[i], "-secret"))
		{
			secret = argv[++i];
		}
		else if (!strcmp(argv[i], "-udn"))
		{
			player.udn = argv[++i];
		}
		else if (!strcmp(argv[i], "-debug"))
		{
			level = atoi(argv[++i]);
			if (level >= sizeof(debug) / sizeof(struct debug_s))
			{
				level = sizeof(debug) / sizeof(struct debug_s) - 1;
			}
		}
		else if (!strcmp(argv[i], "-password"))
		{
			passwd = argv[++i];
		}
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
		{
			return print_usage(argv);
		}
		else if (!player.hostname)
		{
			player.hostname = argv[i];
		}
		else if (!fname)
		{
			fname = argv[i];
		}
	}

	ret = logger_init(NULL, NULL, (level < 0) ? E_LOG : level, NULL);
	if (ret != 0)
		{
		fprintf(stderr, "Could not initialize log facility\n");

		return EXIT_FAILURE;
		}

	ret = platform_init();
	if (ret < 0) {
		DPRINTF(E_FATAL, L_MAIN, "Platform init failed\n");
		return ret;
	}

	DPRINTF(E_LOG, L_MAIN, "player.hostname: %s, fname: %s\n", player.hostname, fname);

	// This obtains host (192.168.4.64), interface (eth0) and netmask (0xffffff00)
	// for the host that is running MASS. This call creates dependency for cross_net.[c,h]
	// and it would be good to get rid of it if possible.
	glHost = get_interface(!strchr(glInterface, '?') ? glInterface : NULL, &iface, &glNetmask);
	DPRINTF(E_INFO, L_MAIN,"Binding to %s [%s] with mask 0x%08x\n", inet_ntoa(glHost), iface, ntohl(glNetmask));
	NFREE(iface);

	if (!player.hostname)
		return print_usage(argv);
	if (!fname)
		return print_usage(argv);

	if (!strcmp(fname, "-"))
	{
		infile = fileno(stdin);
	}
	else if ((infile = open(fname, O_RDONLY)) == -1)
	{
		DPRINTF(E_FATAL, L_MAIN, "cannot open file %s\n", fname);
		exit(1);
	}

	// get player's address
	player.hostent = gethostbyname(player.hostname);
	if (!player.hostent)
	{
		DPRINTF(E_FATAL, L_MAIN, "Cannot resolve name %s\n", player.hostname);
		exit(1);
	}
	memcpy(&player.addr.s_addr, player.hostent->h_addr_list[0], player.hostent->h_length);

	DPRINTF(E_DBG, L_MAIN, "am=%s\n", am);

	if (am && strcasestr(am, "appletv") && pk && *pk && !secret)
	{
		DPRINTF(E_FATAL, L_MAIN, "AppleTV requires authentication (need to send secret field)\n");
		exit(1);
	}

	// setup named pipe for metadata/commands
	// TODO: <@bradkeifer> make named pipe filename an argument to reduce coupling
	snprintf(cmdPipeName, sizeof(cmdPipeName), "/tmp/raop-%s", activeRemote);
	DPRINTF(E_INFO, L_MAIN, "Listening for commands on named pipe %s\n", cmdPipeName);
	mkfifo(cmdPipeName, 0666);

	// if ((encryption || auth) && strchr(et, '1'))
	// 	crypto = AIRPLAY2_RSA;
	// else
	// 	crypto = AIRPLAY2_CLEAR;

	// if airport express, force auth
	if (am && strcasestr(am, "airport"))
	{
		auth = true;
	}

	// handle device password
	char *password = NULL;
	if (*passwd && pw && !strcasecmp(pw, "true"))
	{
		char *encrypted;
		// add up to 2 trailing '=' and adjust size
		asprintf(&encrypted, "%s==", passwd);
		encrypted[strlen(passwd) + strlen(passwd) % 4] = '\0';
		password = malloc(strlen(encrypted));
		size_t len = base64_decode(encrypted, password);
		free(encrypted);
		// xor with UDN
		for (size_t i = 0; i < len; i++)
			password[i] ^= player.udn[i];
		password[len] = '\0';
	}

	// create the airplay context
	// if ((airplay2cl = airplay2cl_create(glHost, 0, 0, glDACPid, activeRemote, alac ? AIRPLAY2_ALAC : AIRPLAY2_ALAC_RAW, DEFAULT_FRAMES_PER_CHUNK,
	// 							latency, crypto, auth, secret, password, et, md,
	// 							44100, 16, 2,
	// 							volume > 0 ? airplay2cl_float_volume(volume) : -144.0)) == NULL)

	// below works, but should try to use outputs interface and emulate an owntones player in this code
	// if (airplay_create(glHost, glDACPid) < 0)
	// {
	// 	DPRINTF(E_FATAL, L_MAIN, "Cannot create airplay context %p", airplay2cl);
	// 	exit(1);
	// }

	// connect to player
	DPRINTF(E_INFO, L_MAIN, "Connecting to player: %s (%s:%hu)\n", 
		player.udn ? player.udn : player.hostname, 
		inet_ntoa(player.addr), player.port
	);

	ap_device.id = 0; // We need to pass the MAC address of the device from MASS
	ap_device.name = player.hostname;
	ap_device.password = password;
	ap_device.type = OUTPUT_TYPE_AIRPLAY;
	ap_device.type_name = outputs_name(ap_device.type);
	ap_device.extra_device_info = &ap_extra;
	ap_device.volume = volume;
	ap_device.quality.sample_rate = AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT;
	ap_device.quality.bits_per_sample = AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT;
	ap_device.quality.channels = AIRPLAY_QUALITY_CHANNELS_DEFAULT;

	ap_extra.mdns_name = player.hostname;
	ap_extra.devtype = AIRPLAY_DEV_OTHER;
	if (!am)
		ap_extra.devtype = AIRPLAY_DEV_OTHER;
	else if (strncmp(am, "AirPort4", strlen("AirPort4")) == 0)
		ap_extra.devtype = AIRPLAY_DEV_APEX2_80211N; // Second generation
	else if (strncmp(am, "AirPort", strlen("AirPort")) == 0)
		ap_extra.devtype = AIRPLAY_DEV_APEX3_80211N; // Third generation and newer
	else if (strncmp(am, "AppleTV5,3", strlen("AppleTV5,3")) == 0)
		ap_extra.devtype = AIRPLAY_DEV_APPLETV4; // Stream to ATV with tvOS 10 needs to be kept alive
	else if (strncmp(am, "AppleTV", strlen("AppleTV")) == 0)
		ap_extra.devtype = AIRPLAY_DEV_APPLETV;
	else if (strncmp(am, "AudioAccessory", strlen("AudioAccessory")) == 0)
		ap_extra.devtype = AIRPLAY_DEV_HOMEPOD;
	else if (*am == '\0')
		DPRINTF(E_WARN, L_MAIN, "AirPlay device '%s': am has no value\n", ap_device.name);

	switch (player.hostent->h_addrtype)
	{
	case AF_INET:
		ap_device.v4_address = strdup(inet_ntoa(player.addr));
		ap_device.v4_port = player.port;
		DPRINTF(E_INFO, L_MAIN, "Adding AirPlay device '%s': features , type %s, address %s:%d\n", 
	  		ap_device.name, airplay_devtype[ap_extra.devtype], ap_device.v4_address, ap_device.v4_port);
		break;
	case AF_INET6:
		ap_device.v6_address = strdup(inet_ntoa(player.addr));
		ap_device.v6_port = player.port;
		DPRINTF(E_INFO, L_MAIN, "Adding AirPlay device '%s': features , type %s, address %s:%d\n", 
	  		ap_device.name, airplay_devtype[ap_extra.devtype], ap_device.v6_address, ap_device.v6_port);
		break;
	default:
		DPRINTF(E_FATAL, L_MAIN, "Error: AirPlay device '%s' has neither ipv4 og ipv6 address\n", ap_device.name);
		goto exit;
	}

	airplay_create(&ap_device, glDACPid);
	
	/*
	if (!airplay2cl_connect(airplay2cl, player.addr, player.port, volume > 0))
	{
		LOG_ERROR("Cannot connect to AirPlay device %s:%hu, check firewall & port", inet_ntoa(player.addr), player.port);
		goto exit;
	}

	latency = airplay2cl_latency(airplay2cl);

	LOG_INFO("connected to %s on port %d, player latency is %d ms", inet_ntoa(player.addr),
			 player.port, (int)TS2MS(latency, airplay2cl_sample_rate(airplay2cl)));

	if (start || wait)
	{
		uint64_t now = airplay_get_ntp(NULL);

		start_at = (start ? start : now) + MS2NTP(wait) -
				   TS2NTP(latency, airplay2cl_sample_rate(airplay2cl));

		LOG_INFO("now %u.%u, audio starts at NTP %u.%u (in %u ms)", AIRPLAY2_SECNTP(now), AIRPLAY2_SECNTP(start_at),
				 (start_at + TS2NTP(latency, airplay2cl_sample_rate(airplay2cl)) > now) ? (uint32_t)NTP2MS(start_at - now + TS2NTP(latency, airplay2cl_sample_rate(airplay2cl))) : 0);

		airplay2cl_start_at(airplay2cl, start_at);
	}
	*/

	// start the command/metadata reader thread
	pthread_create(&glCmdPipeReaderThread, NULL, CmdPipeReaderThread, NULL);

	// start = airplay_get_ntp(NULL);
	status = PLAYING;

	buf = malloc(DEFAULT_FRAMES_PER_CHUNK * 4);
	uint32_t KeepAlive = 0;

	// keep reading audio from stdin until exit/EOF
	// while (n || airplay2cl_is_playing(airplay2cl))
	while (n)
	{
		uint64_t playtime, now;

		if (status == STOPPED)
			break;

		now = airplay_get_ntp(NULL);

		// execute every second
		if (now - last > MS2NTP(1000))
		{
			last = now;
			// uint32_t elapsed = TS2MS(frames - airplay2cl_latency(airplay2cl), airplay2cl_sample_rate(airplay2cl));
			// if (frames && frames > airplay2cl_latency(airplay2cl))
			// {
			// 	LOG_INFO("elapsed milliseconds: %" PRIu64, elapsed);
			// }

			// // send keepalive when needed (to prevent stop playback on homepods)
			// if (!(KeepAlive++ & 0x0f))
			// 	airplay2cl_keepalive(airplay2cl);
		}

		// send chunk if needed
		// if (status == PLAYING && airplay2cl_accept_frames(airplay2cl))
		if (status == PLAYING)
		{
			n = read(infile, buf, DEFAULT_FRAMES_PER_CHUNK * 4);
			if (!n)
				continue;

			// airplay2cl_send_chunk(airplay2cl, buf, n / 4, &playtime);
			frames += n / 4;
		}
		else
		{
			// prevent full cpu usage if we're waiting on data
			usleep(1000);
		}
	}
	DPRINTF(E_INFO, L_MAIN, "end of stream reached\n");

	glMainRunning = false;
	free(buf);
	airplay_destroy();
	platform_deinit();
	pthread_join(glCmdPipeReaderThread, NULL);
	goto exit;

exit:
	DPRINTF(E_INFO, L_MAIN, "exiting...\n");
	close(cmdPipeFd);
	unlink(cmdPipeName);
	airplay_destroy();
	platform_deinit();
	return 0;
}