/*
 * AirPlay2 : Client to control an AirPlay device
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
 * This code has been derived from from https://github.com/music-assistant/libairplay2
 * Earlier works:
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Philippe <philippe_44@outlook.com>
 *
 */
 
#ifndef __AIRPLAY2_CLIENT_H_
#define __AIRPLAY2_CLIENT_H_

/*--------------- SYNCHRO logic explanation ----------------*/

/*
 The logic is that, using one user-provided function
 - get_ntp(struct ntp_t *) returns a NTP time as a flat 64 bits value and in the
 structure passed (if NULL, just return the value)

 The AIRPLAY2 client automatically binds the NTP time of the player to the NTP time
 provided by get_ntp. AIRPLAY2 players measure also audio with timestamps, one per
 frame (sample = 44100 per seconds normally). There are few macros to move from
 NTP to TS values.

 AIRPLAY2 players have a latency which is usually 11025 frames. It's possible to set
 another value at the player creation, but always use the airplay2cl_latency()
 accessor to obtain the real value - in frames.

 The precise time at the DAC is the time at the client plus the latency, so when
 setting a start time, we must anticipate by the latency if we want the first
 frame to be *exactly* played at that NTP value.

 There are two ways to calculate the duration of what has been played
 1- Based on time: if pause has never been made, simply make the difference
 between the NTP start time and the current NTP time, minus the latency (in NTP)
 2- Based on sent frames: this is the only reliable method if pause has been
 used ==> substract airplay2cl_latency() to the number of frames sent . Any other
 method based on taking local time at pause and substracting local paused tme is
 not as accurate.
 */

 /*--------------- USAGE ----------------*/

 /*
 To play, call airplay2cl_accept_frames. When true is return, one frame can be sent,
 so just use airplay2cl_send_chunk - ONE AT A TIME. The pacing is handled by the
 calls to airplay2cl_accept_frames. To send in burst, send at least airplay2cl_latency
 frames, sleep a while and then do as before

 To start at a precise time, just use airplay2cl_set_start() after having flushed
 the player and give the desired start time in local gettime() time, minus
 latency.

 To pause, stop calling airplay2cl_accept_frames and airplay2cl_send_chunk (obviously),
 call airplay2cl_pause then airplay2cl_flush. To stop call airplay2cl_stop instead of
 airplay2cl_pause

 To resume, optionally call airplay2cl_set_start to restart at a given time or just
 start calling airplay2cl_accept_frames and send airplay2cl_send_chunk
*/

// #include "platform.h"

#define DEFAULT_FRAMES_PER_CHUNK 352
#define MAX_FRAMES_PER_CHUNK 4096 // must match alac_wrapper.h ALAC_MAX_FRAMES
#define AIRPLAY2_LATENCY_MIN 11025
#define SECRET_SIZE	64

typedef struct ntp_s {
	uint32_t seconds;
	uint32_t fraction;
} ntp_t;

#define NTP2MS(ntp) ((((ntp) >> 10) * 1000L) >> 22)
#define MS2NTP(ms) (((((uint64_t) (ms)) << 22) / 1000) << 10)
#define TIME_MS2NTP(time) airplay2cl_time32_to_ntp(time)
#define NTP2TS(ntp, rate) ((((ntp) >> 16) * (rate)) >> 16)
#define TS2NTP(ts, rate)  (((((uint64_t) (ts)) << 16) / (rate)) << 16)
#define MS2TS(ms, rate) ((((uint64_t) (ms)) * (rate)) / 1000)
#define TS2MS(ts, rate) NTP2MS(TS2NTP(ts,rate))

#define AIRPLAY2_OPTION_NONE 0x00
#define AIRPLAY2_OPTION_ANNOUNCE 1 << 1
#define AIRPLAY2_OPTION_SETUP 1 << 2
#define AIRPLAY2_OPTION_RECORD 1 << 3
#define AIRPLAY2_OPTION_PAUSE 1 << 4
#define AIRPLAY2_OPTION_FLUSH 1 << 5
#define AIRPLAY2_OPTION_FLUSHBUFFERED 1 << 6
#define AIRPLAY2_OPTION_TEARDOWN 1 << 7
#define AIRPLAY2_OPTION_OPTIONS 1 << 8
#define AIRPLAY2_OPTION_POST 1 << 9
#define AIRPLAY2_OPTION_GET 1 << 10
#define AIRPLAY2_OPTION_PUT 1 << 11

typedef struct airplay2cl_t {uint32_t dummy;} airplay2cl_t;

struct airplay2cl_s;

typedef enum airplay2_codec_s { AIRPLAY2_PCM = 0, AIRPLAY2_ALAC_RAW, AIRPLAY2_ALAC, AIRPLAY2_AAC,
							AIRPLAY2_AAL_ELC } airplay2_codec_t;
typedef enum airplay2_crypto_s { AIRPLAY2_CLEAR = 0, AIRPLAY2_RSA, AIRPLAY2_FAIRPLAY, AIRPLAY2_MFISAP,
							 AIRPLAY2_FAIRPLAYSAP } airplay2_crypto_t;
typedef enum airplay2_states_s { AIRPLAY2_DOWN = 0, AIRPLAY2_FLUSHING, AIRPLAY2_FLUSHED,
							 AIRPLAY2_STREAMING } airplay2_state_t;

typedef struct {
	int channels;
	int	sample_size;
	int	sample_rate;
	airplay2_codec_t codec;
	airplay2_crypto_t crypto;
} airplay2_settings_t;

typedef struct {
	uint8_t proto;
	uint8_t type;
	uint8_t seq[2];
} __attribute__ ((packed)) rtp_header_t;

typedef struct {
	rtp_header_t hdr;
	uint32_t 	rtp_timestamp_latency;
	ntp_t   curr_time;
	uint32_t   rtp_timestamp;
} __attribute__ ((packed)) rtp_sync_pkt_t;

typedef struct {
	rtp_header_t hdr;
	uint32_t timestamp;
	uint32_t ssrc;
} __attribute__ ((packed)) rtp_audio_pkt_t;

uint64_t airplay2cl_get_ntp(struct ntp_s* ntp);

// if volume < -30 and not -144 or volume > 0, then not "initial set volume" will be done
struct airplay2cl_s *airplay2cl_create(struct in_addr host, uint16_t port_base, uint16_t port_range,
							   char *DACP_id, char *active_remote,
							   airplay2_codec_t codec, int frame_len, int latency_frames,
							   airplay2_crypto_t crypto, bool auth, char *secret, char *passwd,
							   char *et, char *md,
							   int sample_rate, int sample_size, int channels, float volume,
							   int airplay_version);

bool	airplay2cl_destroy(struct airplay2cl_s *p);
bool	airplay2cl_connect(struct airplay2cl_s *p, struct in_addr host, uint16_t destport, bool set_volume);
bool 	airplay2cl_repair(struct airplay2cl_s *p, bool set_volume);
bool 	airplay2cl_disconnect(struct airplay2cl_s *p);
bool    airplay2cl_flush(struct airplay2cl_s *p);
bool 	airplay2cl_keepalive(struct airplay2cl_s *p);

bool 	 airplay2cl_set_progress(struct airplay2cl_s *p, uint64_t elapsed, uint64_t end);
bool 	 airplay2cl_set_progress_ms(struct airplay2cl_s *p, uint32_t elapsed, uint32_t duration);
uint64_t airplay2cl_get_progress_ms(struct airplay2cl_s* p);
bool 	 airplay2cl_set_volume(struct airplay2cl_s *p, float vol);
float 	 airplay2cl_float_volume(int vol);
bool 	 airplay2cl_set_daap(struct airplay2cl_s *p, int count, ...);
bool 	 airplay2cl_set_artwork(struct airplay2cl_s *p, char *content_type, int size, char *image);

bool 	airplay2cl_accept_frames(struct airplay2cl_s *p);
bool	airplay2cl_send_chunk(struct airplay2cl_s *p, uint8_t *sample, int size, uint64_t *playtime);

bool 	airplay2cl_start_at(struct airplay2cl_s *p, uint64_t start_time);
void 	airplay2cl_pause(struct airplay2cl_s *p);
void 	airplay2cl_stop(struct airplay2cl_s *p);

/*
	These are thread safe
*/
uint32_t 	airplay2cl_latency(struct airplay2cl_s *p);
uint32_t 	airplay2cl_sample_rate(struct airplay2cl_s *p);
airplay2_state_t airplay2cl_state(struct airplay2cl_s *p);
uint32_t 	airplay2cl_queue_len(struct airplay2cl_s *p);

uint32_t 	airplay2cl_queued_frames(struct airplay2cl_s *p);

bool 	airplay2cl_is_sane(struct airplay2cl_s *p);
bool 	airplay2cl_is_connected(struct airplay2cl_s *p);
bool 	airplay2cl_is_playing(struct airplay2cl_s *p);
bool 	airplay2cl_sanitize(struct airplay2cl_s *p);

uint64_t 	airplay2cl_time32_to_ntp(uint32_t time);

struct mdnssd_handle_s;

bool AppleTVpairing(struct mdnssd_handle_s* mDNShandle, char** pUDN, char** pSecret);
bool AirPlayPassword(struct mdnssd_handle_s* mDNShandle, bool (*excluded)(char* model, char* name), char** UDN, char** passwd);

#endif
