/*
 * A place to define dummy functions from owntones in order to compile
 * cliairplay2 without the need for the full owntones codebase.
 *
 * Try to eliminate the entries below over time.
 */
#include <event2/event.h>
#include <event2/buffer.h>

 #include "player.h"
 #include "db.h"
 #include "artwork.h"
 #include "dmap_common.h"

/* ----------- player.[c,h] -------------------------*/
struct event_base *evbase_player;

int
player_get_status(struct player_status *status) {
  // Dummy function to satisfy the call in cliairplay2
  status->status = PLAY_PLAYING;
  status->repeat = REPEAT_OFF;
  status->shuffle = false;
  status->consume = false;
  status->volume = 100; // Default volume
  return 0;
}

int
player_device_add(void *device) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
player_device_remove(void *device) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
player_playback_start(void) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
player_playback_pause(void) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
player_playback_next(void) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
player_playback_prev(void) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

const char *
player_pmap(void *p) {
  // Dummy function to satisfy the call in cliairplay2
  return "dummy_player";
}

/* -------------- db.h -------------------------*/
int
db_speaker_get(struct output_device *device, uint64_t id) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
db_speaker_save(struct output_device *device)
{
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

int
db_perthread_init(void) {
  // Dummy function to satisfy the call in cliairplay2
  return 0;
}

void
db_perthread_deinit(void) {
  // Dummy function to satisfy the call in cliairplay2
  // No resources to free in this dummy implementation
}

struct db_queue_item *
db_queue_fetch_byitemid(uint32_t item_id) {
  // Dummy function to satisfy the call in cliairplay2
  return (struct db_queue_item *) NULL;
}

void
free_queue_item(struct db_queue_item *qi, int content_only) {
    // Dummy function to satisfy the call in cliairplay2
    // No resources to free in this dummy implementation
}

/* ------------------- artwork.h ----------------------*/
int
artwork_get_item(struct evbuffer *evbuf, int id, int max_w, int max_h, int format) {
  // Dummy function to satisfy the call in cliairplay2
  // Just return an error indicating no artwork found
  return -1;
}

/* ------------------- dmap_common.h -------------------*/
int
dmap_encode_queue_metadata(struct evbuffer *songlist, struct evbuffer *song, struct db_queue_item *queue_item) {
  // Dummy function to satisfy the call in cliairplay2
  // Just return an error indicating no metadata encoded
  return -1;
}
