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
 
#include <stdint.h>
#include "misc.h"
#include "rtp_common.h"

#define DEFAULT_FRAMES_PER_CHUNK 352

#define AIRPLAY_QUALITY_SAMPLE_RATE_DEFAULT     44100
#define AIRPLAY_QUALITY_BITS_PER_SAMPLE_DEFAULT 16
#define AIRPLAY_QUALITY_CHANNELS_DEFAULT        2


/* Keep in sync with const char *airplay_devtype */
enum airplay_devtype {
  AIRPLAY_DEV_APEX2_80211N,
  AIRPLAY_DEV_APEX3_80211N,
  AIRPLAY_DEV_APPLETV,
  AIRPLAY_DEV_APPLETV4,
  AIRPLAY_DEV_HOMEPOD,
  AIRPLAY_DEV_OTHER,
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

// Info about the device, which is not required by the player, only internally
struct airplay_extra
{
  enum airplay_devtype devtype;

  char *mdns_name;

  uint16_t wanted_metadata;
  bool supports_auth_setup;
  bool supports_pairing_transient;
};

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

#define MS2NTP(ms) (((((uint64_t) (ms)) << 22) / 1000) << 10)
#define MS2TS(ms, rate) ((((uint64_t) (ms)) * (rate)) / 1000)

uint64_t airplay_get_ntp(struct ntp_timestamp* ntp);
int airplay_create(struct output_device *dev, char *DACP_id);
int airplay_destroy(void);