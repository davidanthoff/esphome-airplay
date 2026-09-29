/*
 * audio_output_esphome.c — an upstream audio output backend (implements
 * upstream main/audio/audio_output.h) that writes into ESPHome's media
 * pipeline instead of driving I2S itself.
 *
 * Upstream already selects exactly one backend at build time
 * (audio_output.c = I2S, audio_output_spdif.c, audio_output_usb.c) and keeps
 * weak defaults for the optional calls in audio_output_common.c. This file is
 * one more backend. Read upstream audio_output.c next to it: the structure
 * (playback task pulling from audio_receiver_read(), queue-depth cursor for
 * the timing engine) is deliberately the same.
 *
 * The one idea that matters:
 *   upstream I2S backend:  queued = frames handed to DMA - frames DMA sent
 *   this backend:          queued = frames ESPHome accepted
 *                                   - frames ESPHome reported as played
 *                                     (MediaSource::notify_audio_played)
 * audio_output_get_pipeline_us() turns that into "how long until the next
 * sample we write is heard", which is what the timing engine's position
 * servo needs. Everything between write_output() and the I2S peripheral
 * (resampler, mixer, ring buffers, DMA) is inside that measurement. Only
 * delay after the ESP32 (TOSLINK receiver, amp DSP) is not, which is what
 * output_delay_us is for.
 *
 * STATUS: plays on hardware (M1). Sync accuracy is milestone M2 (HANDOFF.md).
 */

#include "audio_output.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "airplay_core_internal.h"
#include "audio_receiver.h"

static const char *TAG = "airplay_out";

/* Frames per AirPlay packet. Same value as upstream audio_output.c. */
#define FRAME_SAMPLES 352

/* How long one host write may block. Short, so stop/deactivate is noticed
 * quickly; the retry loop covers the backpressure case. */
#define HOST_WRITE_TIMEOUT_MS 20

/* Fallback latency model, used only when no played-frames feedback has
 * arrived yet (audio_output_get_hardware_latency_us()).
 * TODO(M2): measure on real hardware (log pipeline_us once playing) and
 * replace this guess. */
#define HOST_NOMINAL_LATENCY_US 100000

#define PLAYBACK_TASK_STACK 4096

/* On resume after a pause, keep the cursor unless nothing has been played for
 * this long. By then the speaker chain (resampler + mixer + I2S, ~0.7 s of
 * buffering plus their idle timeouts) has certainly drained, so any frames it
 * never reported as played were dropped and must not count as queued. */
#define CURSOR_IDLE_RESET_US 1500000

/* Period of the pipeline diagnostic log line while output is active. */
#define PIPELINE_LOG_INTERVAL_US 10000000

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_active = false;
static volatile bool s_flush_requested = false;
static volatile int s_source_rate = 44100;
static uint32_t s_output_delay_us = 0;

/* Written by the playback task / speaker callback task; 64-bit so they never
 * wrap. Accessed with __atomic builtins like upstream does. */
static uint64_t s_submitted_frames = 0;
static uint64_t s_played_frames = 0;
static int64_t s_last_played_us = 0;

/* All-zero frame used when the receiver has nothing for us. */
static int16_t s_silence[FRAME_SAMPLES * 2];

static void reset_cursor(void) {
  __atomic_store_n(&s_submitted_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s_played_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s_last_played_us, 0, __ATOMIC_RELAXED);
}

/* Push all frames to the host, retrying while it is not ready. Returns false
 * if we were stopped/deactivated before everything was accepted. */
static bool write_all(const int16_t *pcm, size_t frames, uint32_t rate) {
  size_t done = 0;
  while (done < frames) {
    if (!s_running || !s_active) {
      return false;
    }
    size_t n = airplay_core_host_write(pcm + done * 2, frames - done, rate,
                                       HOST_WRITE_TIMEOUT_MS);
    if (n > 0) {
      done += n;
      __atomic_add_fetch(&s_submitted_frames, (uint64_t)n, __ATOMIC_RELAXED);
    }
    /* n == 0: the host blocked for HOST_WRITE_TIMEOUT_MS (not yet the active
     * source, or reconfiguring the speaker for a new stream format). Retry
     * the same data. The timing engine's servo absorbs the delay. */
  }
  return true;
}

static void playback_task(void *arg) {
  (void)arg;
  int16_t *pcm = heap_caps_malloc((size_t)(FRAME_SAMPLES + 1) * 2 *
                                      sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (pcm == NULL) {
    ESP_LOGE(TAG, "Failed to allocate playback buffer");
    s_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  int64_t last_log_us = 0;
  while (s_running) {
    if (!s_active) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (s_flush_requested) {
      s_flush_requested = false;
      /* TODO(M3): upstream flushes the DMA ring here. We cannot remove audio
       * that is already queued inside ESPHome's speaker chain through the
       * MediaSource API. Right now stale audio (up to the chain's buffer
       * depth) plays out after a seek/pause and the timing engine drops the
       * late frames that follow. Options are in HANDOFF.md ("Flush"). */
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= PIPELINE_LOG_INTERVAL_US) {
      last_log_us = now_us;
      uint32_t pipeline_us = 0;
      bool measured = audio_output_get_pipeline_us(NULL, &pipeline_us);
      ESP_LOGI(TAG, "pipeline=%" PRIu32 " ms (%s) submitted=%" PRIu64
               " played=%" PRIu64,
               pipeline_us / 1000, measured ? "measured" : "no feedback yet",
               __atomic_load_n(&s_submitted_frames, __ATOMIC_RELAXED),
               __atomic_load_n(&s_played_frames, __ATOMIC_RELAXED));
    }

    uint32_t rate = (uint32_t)s_source_rate;
    size_t frames = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    if (frames > 0) {
      write_all(pcm, frames, rate);
    } else {
      /* Receiver underflow or still waiting for the first frame's play time:
       * upstream keeps the DMA clocked with silence so the playout position
       * stays continuous. Do the same, so the host's played-frames counter
       * keeps advancing and pipeline_us stays meaningful. */
      write_all(s_silence, FRAME_SAMPLES, rate);
    }
  }

  heap_caps_free(pcm);
  s_task = NULL;
  vTaskDelete(NULL);
}

/* ---- glue called from airplay_core.c --------------------------------- */

void airplay_output_configure(uint32_t output_delay_us) {
  s_output_delay_us = output_delay_us;
}

void airplay_output_set_active(bool active) {
  if (active && !s_active) {
    /* Resume after a pause. Do NOT simply reset: after a quick pause/resume
     * (Apple Music does one on many track changes) the speaker chain is still
     * playing out up to ~0.7 s of earlier audio. Its played-frame reports
     * would then count against a zeroed "submitted", the queue would read as
     * empty while it is not, and every later frame would play that much late
     * for the rest of the session. Only reset once the chain has gone idle. */
    int64_t last = __atomic_load_n(&s_last_played_us, __ATOMIC_RELAXED);
    if (last != 0 && esp_timer_get_time() - last > CURSOR_IDLE_RESET_US) {
      reset_cursor();
    }
  }
  s_active = active;
}

void airplay_output_reset_cursor(void) {
  /* New playback: the host (speaker_source) has just reset its own
   * pending-frame counter, so from here on it reports exactly the frames we
   * write. Start from zero at the same point. */
  reset_cursor();
}

void airplay_output_notify_played(uint32_t frames, int64_t timestamp_us) {
  __atomic_add_fetch(&s_played_frames, (uint64_t)frames, __ATOMIC_RELAXED);
  __atomic_store_n(&s_last_played_us, timestamp_us, __ATOMIC_RELAXED);
}

/* ---- upstream audio_output.h API -------------------------------------- */

esp_err_t audio_output_init(void) {
  memset(s_silence, 0, sizeof(s_silence));
  reset_cursor();
  return ESP_OK;
}

void audio_output_start(void) {
  if (s_task != NULL) {
    return;
  }
  s_running = true;
  /* Upstream pins playback to a core and gives it AUDIO_PLAYBACK_TASK_PRIORITY
   * so it outranks the receiver tasks (see the comment in audio_output.h).
   * Keep that. The stack stays in internal RAM (see spiram_task.h). */
  BaseType_t ok = xTaskCreatePinnedToCore(
      playback_task, "airplay_play", PLAYBACK_TASK_STACK, NULL,
      AUDIO_PLAYBACK_TASK_PRIORITY, &s_task, tskNO_AFFINITY);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback task");
    s_running = false;
    s_task = NULL;
  }
}

void audio_output_stop(void) {
  s_running = false;
  /* The task notices within one HOST_WRITE_TIMEOUT_MS / 10 ms tick and
   * deletes itself. */
}

void audio_output_flush(void) { s_flush_requested = true; }

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  /* Only used by upstream's Bluetooth A2DP path, which is not compiled. */
  (void)data;
  (void)bytes;
  (void)wait;
  return ESP_ERR_NOT_SUPPORTED;
}

void audio_output_set_sample_rate(uint32_t rate) {
  /* Only used by the Bluetooth path upstream. ESPHome's speaker chain adapts
   * to whatever rate we pass with each write. */
  (void)rate;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != s_source_rate) {
    ESP_LOGI(TAG, "Source sample rate %d Hz", rate);
    s_source_rate = rate;
    /* No resampling here: the host's resampler speaker converts to the
     * speaker rate. The cursor math below uses the source rate, which is
     * right as long as notify_audio_played() reports frames at the rate we
     * wrote them (check this in M2 if you ever see a non-44.1 kHz stream). */
  }
}

uint32_t audio_output_get_hardware_latency_us(void) {
  return HOST_NOMINAL_LATENCY_US + s_output_delay_us;
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  if (__atomic_load_n(&s_last_played_us, __ATOMIC_RELAXED) == 0) {
    /* No feedback yet (host not playing): let the timing engine use the
     * modelled latency instead. */
    return false;
  }
  uint64_t submitted = __atomic_load_n(&s_submitted_frames, __ATOMIC_RELAXED);
  uint64_t played = __atomic_load_n(&s_played_frames, __ATOMIC_RELAXED);
  uint64_t queued = submitted > played ? submitted - played : 0;
  if (now_us != NULL) {
    *now_us = esp_timer_get_time();
  }
  if (pipeline_us != NULL) {
    /* TODO(M2): "played" advances in DMA-block steps, so this over-reports by
     * up to one block (a few ms). Upstream accepts the same error for I2S and
     * lets the servo absorb it. A refinement is to extrapolate from
     * s_last_played_us: played += (now - last_played_us) * rate / 1e6,
     * clamped to one block. */
    *pipeline_us = (uint32_t)((queued * 1000000ULL) / (uint32_t)s_source_rate) +
                   s_output_delay_us;
  }
  return true;
}
