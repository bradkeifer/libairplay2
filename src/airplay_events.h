/*
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
 * This code has been copied from https://github.com/owntone/owntone-server
 */

#ifndef __AIRPLAY_EVENTS_H__
#define __AIRPLAY_EVENTS_H__

int
airplay_events_listen(const char *name, const char *address, unsigned short port, const uint8_t *key, size_t key_len);

int
airplay_events_init(void);

void
airplay_events_deinit(void);

#endif  /* !__AIRPLAY_EVENTS_H__ */
