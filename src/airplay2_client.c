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
 * This code has been derived from from https://github.com/music-assistant/libraop
 * Earlier works:
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Philippe <philippe_44@outlook.com>
 */

#include <time.h>
#include <errno.h>
#include <string.h>
#include <gcrypt.h>

#include "airplay2_client.h"
#include "logger.h"
#include "rtp_common.h"
#include "pair_ap/pair.h"
#include "evrtsp/evrtsp.h"
#include "outputs.h"
#include "transcode.h"

/* List of TODO's for AirPlay 2
 *
 * inplace encryption
 * latency needs different handling
 * support ipv6, e.g. in SETPEERS
 *
 */

// Airplay 2 has a gazallion parameters, many of them unknown to us. With the
// below it is possible to easily try different variations.
#define AIRPLAY_USE_STREAMID                 0
#define AIRPLAY_USE_PAIRING_TRANSIENT        1
#define AIRPLAY_USE_AUTH_SETUP               0

// Full traffic dumps in the log in debug mode
#define AIRPLAY_DUMP_TRAFFIC                 0

#define AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT     44100
#define AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT 16
#define AIRPLAY_QUALITY_CHANNELS_DEFAULT        2

// AirTunes v2 number of samples per packet
// Probably using this value because 44100/352 and 48000/352 has good 32 byte
// alignment, which improves performance of some encoders
#define AIRPLAY_SAMPLES_PER_PACKET              352

#define AIRPLAY_RTP_PAYLOADTYPE                 0x60

// For transient pairing the key_len will be 64 bytes, but only 32 are used for
// audio payload encryption. For normal pairing the key is 32 bytes.
#define AIRPLAY_AUDIO_KEY_LEN 32

// How many RTP packets keep in a buffer for retransmission
#define AIRPLAY_PACKET_BUFFER_SIZE    1000

#define AIRPLAY_MD_DELAY_STARTUP      15360
#define AIRPLAY_MD_DELAY_SWITCH       (AIRPLAY_MD_DELAY_STARTUP * 2)
#define AIRPLAY_MD_WANTS_TEXT         (1 << 0)
#define AIRPLAY_MD_WANTS_ARTWORK      (1 << 1)
#define AIRPLAY_MD_WANTS_PROGRESS     (1 << 2)

// ATV4 and Homepod disconnect for reasons that are not clear, but sending them
// progress metadata at regular intervals reduces the problem. The below
// interval was determined via testing, see:
// https://github.com/owntone/owntone-server/issues/734#issuecomment-622959334
#define AIRPLAY_KEEP_ALIVE_INTERVAL   25

// This is an arbitrary value which just needs to be kept in sync with the config
#define AIRPLAY_CONFIG_MAX_VOLUME     11

/* Keep in sync with const char *airplay_devtype */
enum airplay_devtype {
  AIRPLAY_DEV_APEX2_80211N,
  AIRPLAY_DEV_APEX3_80211N,
  AIRPLAY_DEV_APPLETV,
  AIRPLAY_DEV_APPLETV4,
  AIRPLAY_DEV_HOMEPOD,
  AIRPLAY_DEV_OTHER,
};

// Session is starting up
#define AIRPLAY_STATE_F_STARTUP    (1 << 13)
// Streaming is up (connection established)
#define AIRPLAY_STATE_F_CONNECTED  (1 << 14)
// Couldn't start device
#define AIRPLAY_STATE_F_FAILED     (1 << 15)

enum airplay_state {
  // Device is stopped (no session)
  AIRPLAY_STATE_STOPPED   = 0,
  // Session startup
  AIRPLAY_STATE_INFO      = AIRPLAY_STATE_F_STARTUP | 0x01,
  AIRPLAY_STATE_ENCRYPTED = AIRPLAY_STATE_F_STARTUP | 0x02,
  AIRPLAY_STATE_SETUP     = AIRPLAY_STATE_F_STARTUP | 0x03,
  AIRPLAY_STATE_RECORD    = AIRPLAY_STATE_F_STARTUP | 0x04,
  // Session established
  // - streaming ready (RECORD sent and acked, connection established)
  // - commands (SET_PARAMETER) are possible
  AIRPLAY_STATE_CONNECTED = AIRPLAY_STATE_F_CONNECTED | 0x01,
  // Media data is being sent
  AIRPLAY_STATE_STREAMING = AIRPLAY_STATE_F_CONNECTED | 0x02,
  // Session teardown in progress (-> going to STOPPED state)
  AIRPLAY_STATE_TEARDOWN  = AIRPLAY_STATE_F_CONNECTED | 0x03,
  // Session is failed, couldn't startup or error occurred
  AIRPLAY_STATE_FAILED    = AIRPLAY_STATE_F_FAILED | 0x01,
  // Pending PIN or password
  AIRPLAY_STATE_AUTH      = AIRPLAY_STATE_F_FAILED | 0x02,
};

enum airplay_seq_type
{
  AIRPLAY_SEQ_ABORT = -1,
  AIRPLAY_SEQ_START,
  AIRPLAY_SEQ_START_PLAYBACK,
  AIRPLAY_SEQ_PROBE,
  AIRPLAY_SEQ_FLUSH,
  AIRPLAY_SEQ_STOP,
  AIRPLAY_SEQ_FAILURE,
  AIRPLAY_SEQ_PIN_START,
  AIRPLAY_SEQ_SEND_VOLUME,
  AIRPLAY_SEQ_SEND_TEXT,
  AIRPLAY_SEQ_SEND_PROGRESS,
  AIRPLAY_SEQ_SEND_ARTWORK,
  AIRPLAY_SEQ_PAIR_SETUP,
  AIRPLAY_SEQ_PAIR_VERIFY,
  AIRPLAY_SEQ_PAIR_TRANSIENT,
  AIRPLAY_SEQ_FEEDBACK,
  AIRPLAY_SEQ_CONTINUE, // Must be last element
};

// From https://openairplay.github.io/airplay-spec/status_flags.html
enum airplay_status_flags
{
  AIRPLAY_FLAG_PROBLEM_DETECTED               = (1 << 0),
  AIRPLAY_FLAG_NOT_CONFIGURED                 = (1 << 1),
  AIRPLAY_FLAG_AUDIO_CABLE_ATTACHED           = (1 << 2),
  AIRPLAY_FLAG_PIN_REQUIRED                   = (1 << 3),
  AIRPLAY_FLAG_SUPPORTS_FROM_CLOUD            = (1 << 6),
  AIRPLAY_FLAG_PASSWORD_REQUIRED              = (1 << 7),
  AIRPLAY_FLAG_ONE_TIME_PAIRING_REQUIRED      = (1 << 9),
  AIRPLAY_FLAG_SETUP_HK_ACCESS_CTRL           = (1 << 10),
  AIRPLAY_FLAG_SUPPORTS_RELAY                 = (1 << 11),
  AIRPLAY_FLAG_SILENT_PRIMARY                 = (1 << 12),
  AIRPLAY_FLAG_TIGHT_SYNC_IS_GRP_LEADER       = (1 << 13),
  AIRPLAY_FLAG_TIGHT_SYNC_BUDDY_NOT_REACHABLE = (1 << 14),
  AIRPLAY_FLAG_IS_APPLE_MUSIC_SUBSCRIBER      = (1 << 15),
  AIRPLAY_FLAG_CLOUD_LIBRARY_ON               = (1 << 16),
  AIRPLAY_FLAG_RECEIVER_IS_BUSY               = (1 << 17),
};

// Info about the device, which is not required by the player, only internally
struct airplay_extra
{
  enum airplay_devtype devtype;

  char *mdns_name;

  uint16_t wanted_metadata;
  bool supports_auth_setup;
  bool supports_pairing_transient;
};

struct airplay_master_session
{
  struct evbuffer *input_buffer;
  int input_buffer_samples;

  // ALAC encoder and buffer for encoded data
  struct encode_ctx *encode_ctx;
  struct evbuffer *encoded_buffer;

  struct rtp_session *rtp_session;

  struct rtcp_timestamp cur_stamp;

  uint8_t *rawbuf;
  size_t rawbuf_size;
  int samples_per_packet;

  struct media_quality quality;

  // Number of samples that we tell the output to buffer (this will mean that
  // the position that we send in the sync packages are offset by this amount
  // compared to the rtptimes of the corresponding RTP packages we are sending)
  int output_buffer_samples;

  struct airplay_master_session *next;
};

struct airplay_session
{
  uint64_t device_id;
  int callback_id;

  struct airplay_master_session *master_session;

  struct evrtsp_connection *ctrl;

  enum airplay_state state;

  enum airplay_seq_type next_seq;

  uint64_t statusflags;
  uint16_t wanted_metadata;
  bool req_has_auth;
  bool supports_auth_setup;

  struct event *deferredev;

  int reqs_in_flight;
  int cseq;

  uint32_t session_id;
  char session_url[128];
  char session_uuid[37];

  char *realm;
  char *nonce;
  const char *password;

  char *devname;
  char *address;
  int family;

  union net_sockaddr naddr;

  int volume;

  char *local_address;
  unsigned short data_port;
  unsigned short control_port;
  unsigned short events_port;
  unsigned short timing_port; // ATV4 has this set to 0, but it is not used by us anyway

  /* Pairing, see pair.h */
  enum pair_type pair_type;
  struct pair_cipher_context *control_cipher_ctx;
  struct pair_verify_context *pair_verify_ctx;
  struct pair_setup_context *pair_setup_ctx;

  uint8_t shared_secret[64];
  size_t shared_secret_len; // 32 or 64, see AIRPLAY_AUDIO_KEY_LEN for comment

  gcry_cipher_hd_t packet_cipher_hd;

  int server_fd;

  struct airplay_service *timing_svc;
  struct airplay_service *control_svc;

  struct airplay_session *next;
};

struct airplay_metadata
{
  struct evbuffer *metadata;
  struct evbuffer *artwork;
  int artwork_fmt;
};

struct airplay_service
{
  int fd;
  unsigned short port;
  struct event *ev;
};

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

// TODO move to rtp_common
struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};


/* --------------------------- SEQUENCE DEFINITIONS ------------------------- */

struct airplay_seq_definition
{
  enum airplay_seq_type seq_type;

  // Called when a sequence ends, successfully or not. Shoulds also, if
  // required, take care of notifying  player and free the session.
  void (*on_success)(struct airplay_session *rs);
  void (*on_error)(struct airplay_session *rs);
};

struct airplay_seq_request
{
  enum airplay_seq_type seq_type;
  const char *name; // Name of request (for logging)
  enum evrtsp_cmd_type rtsp_type;
  int (*payload_make)(struct evrtsp_request *req, struct airplay_session *rs, void *arg);
  enum airplay_seq_type (*response_handler)(struct evrtsp_request *req, struct airplay_session *rs);
  const char *content_type;
  const char *uri;
  bool proceed_on_rtsp_not_ok; // If true return code != RTSP_OK will not abort the sequence
};

struct airplay_seq_ctx
{
  struct airplay_seq_request *cur_request;
  void (*on_success)(struct airplay_session *rs);
  void (*on_error)(struct airplay_session *rs);
  struct airplay_session *session;
  void *payload_make_arg;
  const char *log_caller;
};


/* ------------------------------ MISC GLOBALS ------------------------------ */

#if AIRPLAY_USE_AUTH_SETUP
static const uint8_t airplay_auth_setup_pubkey[] =
  "\x59\x02\xed\xe9\x0d\x4e\xf2\xbd\x4c\xb6\x8a\x63\x30\x03\x82\x07"
  "\xa9\x4d\xbd\x50\xd8\xaa\x46\x5b\x5d\x8c\x01\x2a\x0c\x7e\x1d\x4e";
#endif

struct features_type_map
{
  uint32_t bit;
  char *name;
};

// List of features announced by AirPlay 2 speakers
// Credit @invano, see https://emanuelecozzi.net/docs/airplay2
static const struct features_type_map features_map[] =
  {
    { 0, "SupportsAirPlayVideoV1" },
    { 1, "SupportsAirPlayPhoto" },
    { 5, "SupportsAirPlaySlideshow" },
    { 7, "SupportsAirPlayScreen" },
    { 9, "SupportsAirPlayAudio" },
    { 11, "AudioRedunant" },
    { 14, "Authentication_4" }, // FairPlay authentication
    { 15, "MetadataFeatures_0" }, // Send artwork image to receiver
    { 16, "MetadataFeatures_1" }, // Send track progress status to receiver
    { 17, "MetadataFeatures_2" }, // Send NowPlaying info via DAAP
    { 18, "AudioFormats_0" },
    { 19, "AudioFormats_1" },
    { 20, "AudioFormats_2" },
    { 21, "AudioFormats_3" },
    { 23, "Authentication_1" }, // RSA authentication (NA)
    { 26, "Authentication_8" }, // 26 || 51, MFi authentication
    { 27, "SupportsLegacyPairing" },
    { 30, "HasUnifiedAdvertiserInfo" },
    { 32, "IsCarPlay" },
    { 32, "SupportsVolume" }, // !32
    { 33, "SupportsAirPlayVideoPlayQueue" },
    { 34, "SupportsAirPlayFromCloud" }, // 34 && flags_6_SupportsAirPlayFromCloud
    { 35, "SupportsTLS_PSK" },
    { 38, "SupportsUnifiedMediaControl" },
    { 40, "SupportsBufferedAudio" }, // srcvers >= 354.54.6 && 40
    { 41, "SupportsPTP" }, // srcvers >= 366 && 41
    { 42, "SupportsScreenMultiCodec" },
    { 43, "SupportsSystemPairing" },
    { 44, "IsAPValeriaScreenSender" },
    { 46, "SupportsHKPairingAndAccessControl" },
    { 48, "SupportsCoreUtilsPairingAndEncryption" }, // 38 || 46 || 43 || 48
    { 49, "SupportsAirPlayVideoV2" },
    { 50, "MetadataFeatures_3" }, // Send NowPlaying info via bplist
    { 51, "SupportsUnifiedPairSetupAndMFi" },
    { 52, "SupportsSetPeersExtendedMessage" },
    { 54, "SupportsAPSync" },
    { 55, "SupportsWoL" }, // 55 || 56
    { 56, "SupportsWoL" }, // 55 || 56
    { 58, "SupportsHangdogRemoteControl" }, // ((isAppleTV || isAppleAudioAccessory) && 58) || (isThirdPartyTV && flags_10)
    { 59, "SupportsAudioStreamConnectionSetup" }, // 59 && !disableStreamConnectionSetup
    { 60, "SupportsAudioMediaDataControl" }, // 59 && 60 && !disableMediaDataControl
    { 61, "SupportsRFC2198Redundancy" },
  };

/* Keep in sync with enum airplay_devtype */
static const char *airplay_devtype[] =
{
  "AirPort Express 2 - 802.11n",
  "AirPort Express 3 - 802.11n",
  "AppleTV",
  "AppleTV4",
  "HomePod",
  "Other",
};

/* Struct with default quality levels */
static struct media_quality airplay_quality_default =
{
  AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT,
  AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT,
  AIRPLAY_QUALITY_CHANNELS_DEFAULT
};

/* From player.c */
extern struct event_base *evbase_player;

/* AirTunes v2 time synchronization */
static struct airplay_service airplay_timing_svc;

/* AirTunes v2 playback synchronization / control */
static struct airplay_service airplay_control_svc;

/* Metadata */
static struct output_metadata *airplay_cur_metadata;

/* Keep-alive timer - hack for ATV's with tvOS 10 */
static struct event *keep_alive_timer;
static struct timeval keep_alive_tv = { AIRPLAY_KEEP_ALIVE_INTERVAL, 0 };

/* Sessions */
static struct airplay_master_session *airplay_master_sessions;
static struct airplay_session *airplay_sessions;

/* Our own device ID */
static uint64_t airplay_device_id;

// Forwards
static int
airplay_device_start(struct output_device *rd, int callback_id);
static void
sequence_start(enum airplay_seq_type seq_type, struct airplay_session *rs, void *arg, const char *log_caller);
static void
sequence_continue(struct airplay_seq_ctx *seq_ctx);


/*----------------------------------------------------------------------------*/
// Use owntones airplay code to emulate libraop raopcl_get_ntp function
uint64_t airplay2cl_get_ntp(struct ntp_timestamp* ntp)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
	{
		DPRINTF(E_LOG, L_AIRPLAY, "clock_gettime failed: %s", strerror(errno));
		return -1;
	}

	if (ntp) {
		ntp->sec = ts.tv_sec + NTP_EPOCH_DELTA;
		ntp->frac = (uint32_t)((double)ts.tv_nsec * 1e-9 * FRAC);
	}

	return ((uint64_t) (ts.tv_sec + NTP_EPOCH_DELTA) << 32) | (uint32_t)((double)ts.tv_nsec * 1e-9 * FRAC);
}

/* ------------------------- Time and control service ----------------------- */

static void
service_stop(struct airplay_service *svc)
{
  if (svc->ev)
    event_free(svc->ev);

  if (svc->fd >= 0)
    close(svc->fd);

  svc->ev = NULL;
  svc->fd = -1;
  svc->port = 0;
}

static int
service_start(struct airplay_service *svc, event_callback_fn cb, unsigned short port, const char *log_service_name)
{
  memset(svc, 0, sizeof(struct airplay_service));

  svc->fd = net_bind(&port, SOCK_DGRAM, log_service_name);
  if (svc->fd < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not start '%s' service\n", log_service_name);
      goto error;
    }

  svc->ev = event_new(evbase_player, svc->fd, EV_READ | EV_PERSIST, cb, svc);
  if (!svc->ev)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create event for '%s' service\n", log_service_name);
      goto error;
    }

  event_add(svc->ev, NULL);

  svc->port = port;

  return 0;

 error:
  service_stop(svc);
  return -1;
}

static void
timing_svc_cb(int fd, short what, void *arg)
{
  struct airplay_service *svc = arg;
  union net_sockaddr peer_addr;
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  uint8_t req[32];
  uint8_t res[32];
  struct ntp_stamp recv_stamp;
  struct ntp_stamp xmit_stamp;
  int ret;

  ret = timing_get_clock_ntp(&recv_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get receive timestamp\n");
      return;
    }

  peer_addrlen = sizeof(peer_addr);
  ret = recvfrom(svc->fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading timing request from %s: %s\n", address, strerror(errno));
      return;
    }

  if (ret != 32)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got timing request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd2))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match timing request (got 0x%02x%02x, expected 0x80d2)\n", address, req[0], req[1]);
      return;
    }

  memset(res, 0, sizeof(res));

  /* Header */
  res[0] = 0x80;
  res[1] = 0xd3;
  res[2] = req[2];

  /* Copy client timestamp */
  memcpy(res + 8, req + 24, 8);

  /* Receive timestamp */
  recv_stamp.sec = htobe32(recv_stamp.sec);
  recv_stamp.frac = htobe32(recv_stamp.frac);
  memcpy(res + 16, &recv_stamp.sec, 4);
  memcpy(res + 20, &recv_stamp.frac, 4);

  /* Transmit timestamp */
  ret = timing_get_clock_ntp(&xmit_stamp);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get transmit timestamp, falling back to receive timestamp\n");

      /* Still better than failing altogether
       * recv/xmit are close enough that it shouldn't matter much
       */
      memcpy(res + 24, &recv_stamp.sec, 4);
      memcpy(res + 28, &recv_stamp.frac, 4);
    }
  else
    {
      xmit_stamp.sec = htobe32(xmit_stamp.sec);
      xmit_stamp.frac = htobe32(xmit_stamp.frac);
      memcpy(res + 24, &xmit_stamp.sec, 4);
      memcpy(res + 28, &xmit_stamp.frac, 4);
    }

  ret = sendto(svc->fd, res, sizeof(res), 0, &peer_addr.sa, peer_addrlen);
  if (ret < 0)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_LOG, L_AIRPLAY, "Could not send timing reply to %s: %s\n", address, strerror(errno));
      return;
    }
}

static void
control_svc_cb(int fd, short what, void *arg)
{
  struct airplay_service *svc = arg;
  union net_sockaddr peer_addr = { 0 };
  socklen_t peer_addrlen = sizeof(peer_addr);
  char address[INET6_ADDRSTRLEN];
  struct airplay_session *rs;
  uint8_t req[8];
  uint16_t seq_start;
  uint16_t seq_len;
  int ret;

  ret = recvfrom(svc->fd, req, sizeof(req), 0, &peer_addr.sa, &peer_addrlen);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Error reading control request: %s\n", strerror(errno));
      return;
    }

  if (ret != 8)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Got control request from %s with size %d\n", address, ret);
      return;
    }

  if ((req[0] != 0x80) || (req[1] != 0xd5))
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Packet header from %s doesn't match retransmit request (got 0x%02x%02x, expected 0x80d5)\n", address, req[0], req[1]);
      return;
    }

  rs = session_find_by_address(&peer_addr);
  if (!rs)
    {
      net_address_get(address, sizeof(address), &peer_addr);
      DPRINTF(E_WARN, L_AIRPLAY, "Control request from %s; not a AirPlay client\n", address);
      return;
    }

  memcpy(&seq_start, req + 4, 2);
  memcpy(&seq_len, req + 6, 2);

  seq_start = be16toh(seq_start);
  seq_len = be16toh(seq_len);

  packets_resend(rs, seq_start, seq_len);
}


/* ------------------------------ Session startup --------------------------- */

static void
start_failure(struct airplay_session *rs)
{
  struct output_device *device;

  device = outputs_device_get(rs->device_id);
  if (!device)
    {
      session_failure(rs);
      return;
    }

  // If our key was incorrect, or the device reset its pairings, then this
  // function was called because the encrypted request (SETUP) timed out
  if (device->auth_key)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Clearing '%s' pairing keys, you need to pair again\n", rs->devname);

      free(device->auth_key);
      device->auth_key = NULL;
      device->requires_auth = 1;
    }

  session_failure(rs);
}

static void
start_retry(struct airplay_session *rs)
{
  struct output_device *device;
  int callback_id = rs->callback_id;

  device = outputs_device_get(rs->device_id);
  if (!device)
    {
      session_failure(rs);
      return;
    }

  // Some devices don't seem to work with ipv6, so if the error wasn't a hard
  // failure (bad password) we fall back to ipv4 and flag device as bad for ipv6
  if (rs->family != AF_INET6 || (rs->state & AIRPLAY_STATE_F_FAILED))
    {
      session_failure(rs);
      return;
    }

  // This flag is permanent and will not be overwritten by mdns advertisements
  device->v6_disabled = 1;

  // Drop session, try again with ipv4
  session_cleanup(rs);
  airplay_device_start(device, callback_id);
}


/* ------------------------------ Session handling -------------------------- */

// Maps our internal state to the generic output state and then makes a callback
// to the player to tell that state
static void
session_status(struct airplay_session *rs)
{
  enum output_device_state state;

  switch (rs->state)
    {
      case AIRPLAY_STATE_AUTH:
	state = OUTPUT_STATE_PASSWORD;
	break;
      case AIRPLAY_STATE_FAILED:
	state = OUTPUT_STATE_FAILED;
	break;
      case AIRPLAY_STATE_STOPPED:
	state = OUTPUT_STATE_STOPPED;
	break;
      case AIRPLAY_STATE_INFO ... AIRPLAY_STATE_RECORD:
	state = OUTPUT_STATE_STARTUP;
	break;
      case AIRPLAY_STATE_CONNECTED:
	state = OUTPUT_STATE_CONNECTED;
	break;
      case AIRPLAY_STATE_STREAMING:
	state = OUTPUT_STATE_STREAMING;
	break;
      case AIRPLAY_STATE_TEARDOWN:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! session_status() called with transitional state (TEARDOWN)\n");
	state = OUTPUT_STATE_STOPPED;
	break;
      default:
	DPRINTF(E_LOG, L_AIRPLAY, "Bug! Unhandled state in session_status(): %d\n", rs->state);
	state = OUTPUT_STATE_FAILED;
    }

  outputs_cb(rs->callback_id, rs->device_id, state);
  rs->callback_id = -1;
}

static void
master_session_free(struct airplay_master_session *rms)
{
  if (!rms)
    return;

  outputs_quality_unsubscribe(&rms->rtp_session->quality);
  rtp_session_free(rms->rtp_session);

  transcode_encode_cleanup(&rms->encode_ctx);

  if (rms->input_buffer)
    evbuffer_free(rms->input_buffer);
  if (rms->encoded_buffer)
    evbuffer_free(rms->encoded_buffer);

  free(rms->rawbuf);
  free(rms);
}

static void
master_session_cleanup(struct airplay_master_session *rms)
{
  struct airplay_master_session *s;
  struct airplay_session *rs;

  // First check if any other session is using the master session
  for (rs = airplay_sessions; rs; rs=rs->next)
    {
      if (rs->master_session == rms)
	return;
    }

  if (rms == airplay_master_sessions)
    airplay_master_sessions = airplay_master_sessions->next;
  else
    {
      for (s = airplay_master_sessions; s && (s->next != rms); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_master_session not found in list; BUG!\n");
      else
	s->next = rms->next;
    }

  master_session_free(rms);
}

static struct airplay_master_session *
master_session_make(struct media_quality *quality)
{
  struct airplay_master_session *rms;
  struct transcode_encode_setup_args encode_args = { .profile = XCODE_ALAC, .quality = quality };
  int ret;

  // First check if we already have a suitable session
  for (rms = airplay_master_sessions; rms; rms = rms->next)
    {
      if (quality_is_equal(quality, &rms->rtp_session->quality))
	return rms;
    }

  // Let's create a master session
  ret = outputs_quality_subscribe(quality);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not subscribe to required audio quality (%d/%d/%d)\n", quality->sample_rate, quality->bits_per_sample, quality->channels);
      return NULL;
    }

  CHECK_NULL(L_AIRPLAY, rms = calloc(1, sizeof(struct airplay_master_session)));

  rms->rtp_session = rtp_session_new(quality, AIRPLAY_PACKET_BUFFER_SIZE, 0);
  if (!rms->rtp_session)
    {
      goto error;
    }

  encode_args.src_ctx = transcode_decode_setup_raw(XCODE_PCM16, quality);
  if (!encode_args.src_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create decoding context\n");
      goto error;
    }

  rms->encode_ctx = transcode_encode_setup(encode_args);
  transcode_decode_cleanup(&encode_args.src_ctx);
  if (!rms->encode_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Will not be able to stream AirPlay 2, ffmpeg has no ALAC encoder\n");
      goto error;
    }

  rms->quality = *quality;
  rms->samples_per_packet = AIRPLAY_SAMPLES_PER_PACKET;
  rms->rawbuf_size = STOB(rms->samples_per_packet, quality->bits_per_sample, quality->channels);
  rms->output_buffer_samples = OUTPUTS_BUFFER_DURATION * quality->sample_rate;

  CHECK_NULL(L_AIRPLAY, rms->rawbuf = malloc(rms->rawbuf_size));
  CHECK_NULL(L_AIRPLAY, rms->input_buffer = evbuffer_new());
  CHECK_NULL(L_AIRPLAY, rms->encoded_buffer = evbuffer_new());

  rms->next = airplay_master_sessions;
  airplay_master_sessions = rms;

  return rms;

 error:
  master_session_free(rms);
  return NULL;
}

static void
session_free(struct airplay_session *rs)
{
  if (!rs)
    return;

  if (rs->master_session)
    master_session_cleanup(rs->master_session);

  if (rs->ctrl)
    {
      evrtsp_connection_set_closecb(rs->ctrl, NULL, NULL);
      evrtsp_connection_free(rs->ctrl);
    }

  if (rs->deferredev)
    event_free(rs->deferredev);

  if (rs->server_fd >= 0)
    close(rs->server_fd);

  chacha_close(rs->packet_cipher_hd);

  pair_setup_free(rs->pair_setup_ctx);
  pair_verify_free(rs->pair_verify_ctx);
  pair_cipher_free(rs->control_cipher_ctx);

  free(rs->local_address);
  free(rs->realm);
  free(rs->nonce);
  free(rs->address);
  free(rs->devname);

  free(rs);
}

static void
session_cleanup(struct airplay_session *rs)
{
  struct airplay_session *s;

  if (rs == airplay_sessions)
    airplay_sessions = airplay_sessions->next;
  else
    {
      for (s = airplay_sessions; s && (s->next != rs); s = s->next)
	; /* EMPTY */

      if (!s)
	DPRINTF(E_WARN, L_AIRPLAY, "WARNING: struct airplay_session not found in list; BUG!\n");
      else
	s->next = rs->next;
    }

  outputs_device_session_remove(rs->device_id);

  session_free(rs);
}

static void
session_failure(struct airplay_session *rs)
{
  /* Session failed, let our user know */
  if (rs->state != AIRPLAY_STATE_AUTH)
    rs->state = AIRPLAY_STATE_FAILED;

  session_status(rs);

  session_cleanup(rs);
}

static void
deferred_session_failure_cb(int fd, short what, void *arg)
{
  struct airplay_session *rs = arg;

  DPRINTF(E_DBG, L_AIRPLAY, "Cleaning up failed session (deferred) on device '%s'\n", rs->devname);
  session_failure(rs);
}

static void
deferred_session_failure(struct airplay_session *rs)
{
  struct timeval tv;

  if (rs->state != AIRPLAY_STATE_AUTH)
    rs->state = AIRPLAY_STATE_FAILED;

  evutil_timerclear(&tv);
  evtimer_add(rs->deferredev, &tv);
}

static void
rtsp_close_cb(struct evrtsp_connection *evcon, void *arg)
{
  struct airplay_session *rs = arg;

  DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' closed RTSP connection\n", rs->devname);

  deferred_session_failure(rs);
}

static void
session_success(struct airplay_session *rs)
{
  session_status(rs);

  session_cleanup(rs);
}

static void
session_connected(struct airplay_session *rs)
{
  rs->state = AIRPLAY_STATE_CONNECTED;

  session_status(rs);
}

static void
session_pair_success(struct airplay_session *rs)
{
  if (rs->next_seq != AIRPLAY_SEQ_CONTINUE)
    {
      sequence_start(rs->next_seq, rs, NULL, "pair_success");
      rs->next_seq = AIRPLAY_SEQ_CONTINUE;
      return;
    }

  session_success(rs);
}

static int
session_connection_setup(struct airplay_session *rs, struct output_device *rd, int family)
{
  char *address;
  char *intf;
  unsigned short port;
  int ret;

  rs->naddr.ss.ss_family = family;

  switch (family)
    {
      case AF_INET:
	if (!rd->v4_address)
	  return -1;

	address = rd->v4_address;
	port = rd->v4_port;


	ret = inet_pton(AF_INET, address, &rs->naddr.sin.sin_addr);
	break;

      case AF_INET6:
	if (!rd->v6_address)
	  return -1;

	address = rd->v6_address;
	port = rd->v6_port;

	intf = strchr(address, '%');
	if (intf)
	  *intf = '\0';

	ret = inet_pton(AF_INET6, address, &rs->naddr.sin6.sin6_addr);

	if (intf)
	  {
	    *intf = '%';

	    intf++;

	    rs->naddr.sin6.sin6_scope_id = if_nametoindex(intf);
	    if (rs->naddr.sin6.sin6_scope_id == 0)
	      {
		DPRINTF(E_LOG, L_AIRPLAY, "Could not find interface %s\n", intf);

		ret = -1;
		break;
	      }
	  }

	break;

      default:
	return -1;
    }

  if (ret <= 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Device '%s' has invalid address (%s) for %s\n", rd->name, address, (family == AF_INET) ? "ipv4" : "ipv6");
      return -1;
    }

  rs->ctrl = evrtsp_connection_new(address, port);
  if (!rs->ctrl)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control connection to '%s' (%s)\n", rd->name, address);
      return -1;
    }

  evrtsp_connection_set_base(rs->ctrl, evbase_player);

  rs->address = strdup(address);
  rs->family = family;

  return 0;
}

static int
session_cipher_setup(struct airplay_session *rs, const uint8_t *key, size_t key_len)
{
  struct pair_cipher_context *control_cipher_ctx = NULL;
  gcry_cipher_hd_t packet_cipher_hd = NULL;

  // For transient pairing the key_len will be 64 bytes, and rs->shared_secret is 32 bytes
  if (key_len < AIRPLAY_AUDIO_KEY_LEN || key_len > sizeof(rs->shared_secret))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Ciphering setup error: Unexpected key length (%zu)\n", key_len);
      goto error;
    }

  rs->shared_secret_len = key_len;
  memcpy(rs->shared_secret, key, key_len);

  control_cipher_ctx = pair_cipher_new(rs->pair_type, 0, key, key_len);
  if (!control_cipher_ctx)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create control ciphering context\n");
      goto error;
    }

  packet_cipher_hd = chacha_open(rs->shared_secret, AIRPLAY_AUDIO_KEY_LEN);
  if (!packet_cipher_hd)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not create packet ciphering handle\n");
      goto error;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Ciphering setup of '%s' completed succesfully, now using encrypted mode\n", rs->devname);

  rs->state = AIRPLAY_STATE_ENCRYPTED;
  rs->control_cipher_ctx = control_cipher_ctx;
  rs->packet_cipher_hd = packet_cipher_hd;

  evrtsp_connection_set_ciphercb(rs->ctrl, rtsp_cipher, rs);

  return 0;

 error:
  pair_cipher_free(control_cipher_ctx);
  chacha_close(packet_cipher_hd);
  return -1;
}

static int
session_ids_set(struct airplay_session *rs)
{
  char *address = NULL;
  char *intf;
  unsigned short port;
  int family;
  int ret;

  // Determine local address, needed for session URL
  evrtsp_connection_get_local_address(rs->ctrl, &address, &port, &family);
  if (!address || (port == 0))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not determine local address\n");
      goto error;
    }

  intf = strchr(address, '%');
  if (intf)
    {
      *intf = '\0';
      intf++;
    }

  DPRINTF(E_DBG, L_AIRPLAY, "Local address: %s (LL: %s) port %d\n", address, (intf) ? intf : "no", port);

  // Session UUID, ID and session URL
  uuid_make(rs->session_uuid);

  gcry_randomize(&rs->session_id, sizeof(rs->session_id), GCRY_STRONG_RANDOM);

  if (family == AF_INET)
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://%s/%u", address, rs->session_id);
  else
    ret = snprintf(rs->session_url, sizeof(rs->session_url), "rtsp://[%s]/%u", address, rs->session_id);
  if ((ret < 0) || (ret >= sizeof(rs->session_url)))
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Session URL length exceeds 127 characters\n");
      goto error;
    }

  rs->local_address = address;
  return 0;

 error:
  free(address);
  return -1;
}

static struct airplay_session *
session_find_by_address(union net_sockaddr *peer_addr)
{
  struct airplay_session *rs;
  uint32_t *addr_ptr;
  int family = peer_addr->sa.sa_family;

  for (rs = airplay_sessions; rs; rs = rs->next)
    {
      if (family == rs->family)
	{
	  if (family == AF_INET && peer_addr->sin.sin_addr.s_addr == rs->naddr.sin.sin_addr.s_addr)
	    break;

	  if (family == AF_INET6 && IN6_ARE_ADDR_EQUAL(&peer_addr->sin6.sin6_addr, &rs->naddr.sin6.sin6_addr))
	    break;
	}
      else if (family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&peer_addr->sin6.sin6_addr))
	{
	  // ipv4 mapped to ipv6 consists of 16 bytes/4 words: 0x00000000 0x00000000 0x0000ffff 0x[IPv4]
	  addr_ptr = (uint32_t *)(&peer_addr->sin6.sin6_addr);
	  if (addr_ptr[3] == rs->naddr.sin.sin_addr.s_addr)
	    break;
	}
    }

  return rs;
}

static struct airplay_session *
session_make(struct output_device *rd, int callback_id)
{
  struct airplay_session *rs;
  struct airplay_extra *re;
  int ret;

  re = rd->extra_device_info;


  CHECK_NULL(L_AIRPLAY, rs = calloc(1, sizeof(struct airplay_session)));
  CHECK_NULL(L_AIRPLAY, rs->deferredev = evtimer_new(evbase_player, deferred_session_failure_cb, rs));

  rs->devname = strdup(rd->name);
  rs->volume = rd->volume;

  rs->state = AIRPLAY_STATE_STOPPED;
  rs->reqs_in_flight = 0;
  rs->cseq = 1;

  rs->device_id = rd->id;
  rs->callback_id = callback_id;

  rs->server_fd = -1;

  rs->password = rd->password;

  rs->supports_auth_setup = re->supports_auth_setup;
  rs->wanted_metadata = re->wanted_metadata;

  rs->next_seq = AIRPLAY_SEQ_CONTINUE;

  rs->timing_svc = &airplay_timing_svc;
  rs->control_svc = &airplay_control_svc;

  ret = session_connection_setup(rs, rd, AF_INET6);
  if (ret < 0)
    {
      ret = session_connection_setup(rs, rd, AF_INET);
      if (ret < 0)
	goto error;
    }

  rs->master_session = master_session_make(&rd->quality);
  if (!rs->master_session)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not attach a master session for device '%s'\n", rd->name);
      goto error;
    }

  // Attach to list of sessions
  rs->next = airplay_sessions;
  airplay_sessions = rs;

  // rs is now the official device session
  outputs_device_session_add(rd->id, rs);

  return rs;

 error:
  session_free(rs);

  return NULL;
}


/* ---------------------- Request/response sequence control ----------------- */

/*
 * Request queueing HOWTO
 *
 * Sending:
 * - increment rs->reqs_in_flight
 * - set evrtsp connection closecb to NULL
 *
 * Request callback:
 * - decrement rs->reqs_in_flight first thing, even if the callback is
 *   called for error handling (req == NULL or HTTP error code)
 * - if rs->reqs_in_flight == 0, setup evrtsp connection closecb
 *
 * When a request fails, the whole AirPlay session is declared failed and
 * torn down by calling session_failure(), even if there are requests
 * queued on the evrtsp connection. There is no reason to think pending
 * requests would work out better than the one that just failed and recovery
 * would be tricky to get right.
 *
 * evrtsp behaviour with queued requests:
 * - request callback is called with req == NULL to indicate a connection
 *   error; if there are several requests queued on the connection, this can
 *   happen for each request if the connection isn't destroyed
 * - the connection is reset, and the closecb is called if the connection was
 *   previously connected. There is no closecb set when there are requests in
 *   flight
 */

static struct airplay_seq_definition airplay_seq_definition[] =
{
  { AIRPLAY_SEQ_START, NULL, start_retry },
  { AIRPLAY_SEQ_START_PLAYBACK, session_connected, start_failure },
  { AIRPLAY_SEQ_PROBE, session_success, session_failure },
  { AIRPLAY_SEQ_FLUSH, session_status, session_failure },
  { AIRPLAY_SEQ_STOP, session_success, session_failure },
  { AIRPLAY_SEQ_FAILURE, session_success, session_failure},
  { AIRPLAY_SEQ_PIN_START, session_success, session_failure },
  { AIRPLAY_SEQ_SEND_VOLUME, session_status, session_failure },
  { AIRPLAY_SEQ_SEND_TEXT, NULL, session_failure },
  { AIRPLAY_SEQ_SEND_PROGRESS, NULL, session_failure },
  { AIRPLAY_SEQ_SEND_ARTWORK, NULL, session_failure },
  { AIRPLAY_SEQ_PAIR_SETUP, session_pair_success, session_failure },
  { AIRPLAY_SEQ_PAIR_VERIFY, session_pair_success, session_failure },
  { AIRPLAY_SEQ_PAIR_TRANSIENT, session_pair_success, session_failure },
  { AIRPLAY_SEQ_FEEDBACK, NULL, session_failure },
};

// The size of the second array dimension MUST at least be the size of largest
// sequence + 1, because then we can count on a zero terminator when iterating
static struct airplay_seq_request airplay_seq_request[][7] =
{
  {
    { AIRPLAY_SEQ_START, "GET /info", EVRTSP_REQ_GET, NULL, response_handler_info_start, NULL, "/info", false },
  },
  {
#if AIRPLAY_USE_AUTH_SETUP
    { AIRPLAY_SEQ_START_PLAYBACK, "auth-setup", EVRTSP_REQ_POST, payload_make_auth_setup, NULL, "application/octet-stream", "/auth-setup", true },
#endif
    // proceed_on_rtsp_not_ok is true because a device may reply with 401 Unauthorized
    // and a WWW-Authenticate header, and then we may need re-run with password auth
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (session)", EVRTSP_REQ_SETUP, payload_make_setup_session, response_handler_setup_session, "application/x-apple-binary-plist", NULL, true },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETPEERS", EVRTSP_REQ_SETPEERS, payload_make_setpeers, NULL, "/peer-list-changed", NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "SETUP (stream)", EVRTSP_REQ_SETUP, payload_make_setup_stream, response_handler_setup_stream, "application/x-apple-binary-plist", NULL, false },
    { AIRPLAY_SEQ_START_PLAYBACK, "RECORD", EVRTSP_REQ_RECORD, payload_make_record, response_handler_record, NULL, NULL, false },
    // Some devices (e.g. Sonos Symfonisk) don't register the volume if it isn't last
    { AIRPLAY_SEQ_START_PLAYBACK, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, response_handler_volume_start, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_PROBE, "GET /info (probe)", EVRTSP_REQ_GET, NULL, response_handler_info_probe, NULL, "/info", false },
  },
  {
    { AIRPLAY_SEQ_FLUSH, "FLUSH", EVRTSP_REQ_FLUSH, payload_make_flush, response_handler_flush, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_STOP, "TEARDOWN", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown, NULL, NULL, true },
  },
  {
    { AIRPLAY_SEQ_FAILURE, "TEARDOWN (failure)", EVRTSP_REQ_TEARDOWN, payload_make_teardown, response_handler_teardown_failure, NULL, NULL, false },
  },
  {
    { AIRPLAY_SEQ_PIN_START, "PIN start", EVRTSP_REQ_POST, payload_make_pin_start, response_handler_pin_start, NULL, "/pair-pin-start", false },
  },
  {
    { AIRPLAY_SEQ_SEND_VOLUME, "SET_PARAMETER (volume)", EVRTSP_REQ_SET_PARAMETER, payload_make_set_volume, NULL, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_TEXT, "SET_PARAMETER (text)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_text, NULL, "application/x-dmap-tagged", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_PROGRESS, "SET_PARAMETER (progress)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_progress, NULL, "text/parameters", NULL, true },
  },
  {
    { AIRPLAY_SEQ_SEND_ARTWORK, "SET_PARAMETER (artwork)", EVRTSP_REQ_SET_PARAMETER, payload_make_send_artwork, NULL, NULL, NULL, true },
  },
  {
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
    { AIRPLAY_SEQ_PAIR_SETUP, "pair setup 3", EVRTSP_REQ_POST, payload_make_pair_setup3, response_handler_pair_setup3, "application/octet-stream", "/pair-setup", false },
  },
  {
    // Proceed on error is true because we want to delete the device key in the response handler if the verification fails
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 1", EVRTSP_REQ_POST, payload_make_pair_verify1, response_handler_pair_verify1, "application/octet-stream", "/pair-verify", true },
    { AIRPLAY_SEQ_PAIR_VERIFY, "pair verify 2", EVRTSP_REQ_POST, payload_make_pair_verify2, response_handler_pair_verify2, "application/octet-stream", "/pair-verify", false },
  },
  {
    // Some devices (i.e. my ATV4) gives a 470 when trying transient, so we proceed on that so the handler can trigger PIN setup sequence
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 1", EVRTSP_REQ_POST, payload_make_pair_setup1, response_handler_pair_setup1, "application/octet-stream", "/pair-setup", true },
    { AIRPLAY_SEQ_PAIR_TRANSIENT, "pair setup 2", EVRTSP_REQ_POST, payload_make_pair_setup2, response_handler_pair_setup2, "application/octet-stream", "/pair-setup", false },
  },
  {
    { AIRPLAY_SEQ_FEEDBACK, "POST /feedback", EVRTSP_REQ_POST, NULL, NULL, NULL, "/feedback", true },
  },
};

/*----------------------------------------------------------------------------*/
static int
airplay2cl_init(char *dacp_id)
{
  int ret;
  int i;
  int timing_port;
  int control_port;

  airplay_device_id = strtoull(dacp_id, NULL, 16);

  // Check alignment of enum seq_type with airplay_seq_definition and
  // airplay_seq_request
  for (i = 0; i < ARRAY_SIZE(airplay_seq_definition); i++)
    {
      if (airplay_seq_definition[i].seq_type != i || airplay_seq_request[i][0].seq_type != i)
        {
	  DPRINTF(E_LOG, L_AIRPLAY, "Bug! Misalignment between sequence enum and structs: %d, %d, %d\n", i, airplay_seq_definition[i].seq_type, airplay_seq_request[i][0].seq_type);
	  return -1;
        }
    }

  CHECK_NULL(L_AIRPLAY, keep_alive_timer = evtimer_new(evbase_player, airplay_keep_alive_timer_cb, NULL));

//   timing_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "timing_port");
  ret = service_start(&airplay_timing_svc, timing_svc_cb, timing_port, "AirPlay timing");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay time synchronization failed to start\n");
      goto out_free_timer;
    }

//   control_port = cfg_getint(cfg_getsec(cfg, "airplay_shared"), "control_port");
  ret = service_start(&airplay_control_svc, control_svc_cb, control_port, "AirPlay control");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay playback control failed to start\n");
      goto out_stop_timing;
    }

  ret = airplay_events_init();
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "AirPlay events failed to start\n");
      goto out_stop_control;
    }

  ret = mdns_browse("_airplay._tcp", airplay_device_cb, MDNS_CONNECTION_TEST);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Could not add mDNS browser for AirPlay devices\n");
      goto out_stop_events;
    }

  return 0;

 out_stop_events:
  airplay_events_deinit();
 out_stop_control:
  service_stop(&airplay_control_svc);
 out_stop_timing:
  service_stop(&airplay_timing_svc);
 out_free_timer:
  event_free(keep_alive_timer);

  return -1;
}

/*----------------------------------------------------------------------------*/
int *airplay2cl_create(struct in_addr host, uint16_t port_base, uint16_t port_range,
							   char *DACP_id, char *active_remote,
							   airplay2_codec_t codec, int frame_len, int latency_frames,
							   airplay2_crypto_t crypto, bool auth, char *secret, char *passwd,
							   char *et, char *md,
							   int sample_rate, int sample_size, int channels, float volume)
{
	return 0;
}
