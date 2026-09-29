#pragma once

/* Glue shared between the airplay_core source files. Not part of the public
 * API in include/airplay_core.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- airplay_core.c ------------------------------------------------------ */

/** Name from airplay_core_config_t, owned by airplay_core.c. */
const char *airplay_core_get_name(void);

/** Forward PCM to the host (wraps airplay_core_host_t.write). */
size_t airplay_core_host_write(const int16_t *pcm, size_t frames,
                               uint32_t sample_rate, uint32_t timeout_ms);

/* ---- audio_output_esphome.c ---------------------------------------------- */
/* The rest of the output backend is the upstream audio_output.h API. */

void airplay_output_configure(uint32_t output_delay_us);
void airplay_output_set_active(bool active);
void airplay_output_reset_cursor(void);
void airplay_output_notify_played(uint32_t frames, int64_t timestamp_us);

/* ---- mdns_airplay_esphome.c ---------------------------------------------- */
/* Implements upstream's mdns_airplay_init() (network/mdns_airplay.h) without
 * calling mdns_init()/mdns_hostname_set(): ESPHome's mdns component owns
 * those. */

#ifdef __cplusplus
}
#endif
