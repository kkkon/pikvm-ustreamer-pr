/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    Copyright (C) 2018-2023  Maxim Devaev <mdevaev@gmail.com>               #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "stream.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <pthread.h>

#include "../libs/types.h"
#include "../libs/tools.h"
#include "../libs/threading.h"
#include "../libs/process.h"
#include "../libs/logging.h"
#include "../libs/ring.h"
#include "../libs/frame.h"
#include "../libs/memsink.h"
#include "../libs/device.h"

#include "blank.h"
#include "encoder.h"
#include "workers.h"
#include "h264.h"
#ifdef WITH_GPIO
#	include "gpio/gpio.h"
#endif


typedef struct {
	us_device_s		*dev;
	us_queue_s		*queue;
	pthread_mutex_t	*mutex;
	atomic_bool		*stop;
} _releaser_context_s;


static void *_releaser_thread(void *v_ctx);

static void _stream_release_buffer(us_stream_s *stream, us_hw_buffer_s *hw);
static bool _stream_is_stopped(us_stream_s *stream);
static bool _stream_has_any_clients(us_stream_s *stream);
static bool _stream_slowdown(us_stream_s *stream);
static int _stream_init_loop(us_stream_s *stream);
static void _stream_expose_frame(us_stream_s *stream, us_frame_s *frame);


#define _SINK_PUT(x_sink, x_frame) { \
		if (stream->x_sink && us_memsink_server_check(stream->x_sink, x_frame)) {\
			bool m_key_requested; /* Unused */ \
			us_memsink_server_put(stream->x_sink, x_frame, &m_key_requested); \
		} \
	}

#define _H264_PUT(x_frame, x_force_key) { \
		if (stream->run->h264) { \
			us_h264_stream_process(stream->run->h264, x_frame, x_force_key); \
		} \
	}


us_stream_s *us_stream_init(us_device_s *dev, us_encoder_s *enc) {
	us_stream_runtime_s *run;
	US_CALLOC(run, 1);
	US_MUTEX_INIT(run->release_mutex);
	atomic_init(&run->release_stop, false);
	US_RING_INIT_WITH_ITEMS(run->http_jpeg_ring, 4, us_frame_init);
	atomic_init(&run->http_has_clients, false);
	atomic_init(&run->http_last_request_ts, 0);
	atomic_init(&run->http_captured_fps, 0);
	atomic_init(&run->stop, false);
	run->blank = us_blank_init();

	us_stream_s *stream;
	US_CALLOC(stream, 1);
	stream->dev = dev;
	stream->enc = enc;
	stream->last_as_blank = -1;
	stream->error_delay = 1;
	stream->h264_bitrate = 5000; // Kbps
	stream->h264_gop = 30;
	stream->run = run;
	return stream;
}

void us_stream_destroy(us_stream_s *stream) {
	us_blank_destroy(stream->run->blank);
	US_RING_DELETE_WITH_ITEMS(stream->run->http_jpeg_ring, us_frame_destroy);
	US_MUTEX_DESTROY(stream->run->release_mutex);
	free(stream->run);
	free(stream);
}

void us_stream_loop(us_stream_s *stream) {
	us_stream_runtime_s *const run = stream->run;

	US_LOG_INFO("Using V4L2 device: %s", stream->dev->path);
	US_LOG_INFO("Using desired FPS: %u", stream->dev->desired_fps);

	atomic_store(&run->http_last_request_ts, us_get_now_monotonic());

	if (stream->h264_sink != NULL) {
		run->h264 = us_h264_stream_init(stream->h264_sink, stream->h264_m2m_path, stream->h264_bitrate, stream->h264_gop);
	}

	while (!_stream_init_loop(stream)) {
		const uint n_releasers = stream->dev->run->n_bufs;
		US_CALLOC(run->releasers, n_releasers);
		for (uint index = 0; index < n_releasers; ++index) {
			run->releasers[index].queue = us_queue_init(1);
			_releaser_context_s *ctx;
			US_CALLOC(ctx, 1);
			ctx->dev = stream->dev;
			ctx->queue = run->releasers[index].queue;
			ctx->mutex = &run->release_mutex;
			ctx->stop = &run->release_stop;
			US_THREAD_CREATE(run->releasers[index].tid, _releaser_thread, ctx);
		}

		ldf grab_after = 0;
		uint fluency_passed = 0;
		uint captured_fps_accum = 0;
		sll captured_fps_ts = 0;

		US_LOG_INFO("Capturing ...");

		while (!_stream_is_stopped(stream) && !atomic_load(&run->release_stop)) {
			US_SEP_DEBUG('-');
			US_LOG_DEBUG("Waiting for worker ...");

			us_worker_s *const ready_wr = us_workers_pool_wait(stream->enc->run->pool);
			us_encoder_job_s *const ready_job = ready_wr->job;

			if (ready_job->hw != NULL) {
				_stream_release_buffer(stream, ready_job->hw);
				ready_job->hw = NULL;
				if (ready_wr->job_failed) {
					// pass
				} else if (ready_wr->job_timely) {
					_stream_expose_frame(stream, ready_job->dest);
					US_LOG_PERF("##### Encoded JPEG exposed; worker=%s, latency=%.3Lf",
						ready_wr->name, us_get_now_monotonic() - ready_job->dest->grab_ts);
				} else {
					US_LOG_PERF("----- Encoded JPEG dropped; worker=%s", ready_wr->name);
				}
			}

			const bool h264_force_key = _stream_slowdown(stream);
			if (_stream_is_stopped(stream)) {
				goto close;
			}

			us_hw_buffer_s *hw;
			const int buf_index = us_device_grab_buffer(stream->dev, &hw);
			switch (buf_index) {
				case -3: continue; // Broken frame
				case -2: // Persistent timeout
				case -1: goto close; // Any error
			}
			assert(buf_index >= 0);

#			ifdef WITH_GPIO
			us_gpio_set_stream_online(true);
#			endif

			const ldf now_ts = us_get_now_monotonic();

			if (now_ts < grab_after) {
				fluency_passed += 1;
				US_LOG_VERBOSE("Passed %u frames for fluency: now=%.03Lf, grab_after=%.03Lf",
					fluency_passed, now_ts, grab_after);
				_stream_release_buffer(stream, hw);
			} else {
				fluency_passed = 0;

				const sll now_sec_ts = us_floor_ms(now_ts);
				if (now_sec_ts != captured_fps_ts) {
					US_LOG_PERF_FPS("A new second has come; captured_fps=%u", captured_fps_accum);
					atomic_store(&run->http_captured_fps, captured_fps_accum);
					captured_fps_accum = 0;
					captured_fps_ts = now_sec_ts;
				}
				captured_fps_accum += 1;

				const ldf fluency_delay = us_workers_pool_get_fluency_delay(stream->enc->run->pool, ready_wr);
				grab_after = now_ts + fluency_delay;
				US_LOG_VERBOSE("Fluency: delay=%.03Lf, grab_after=%.03Lf", fluency_delay, grab_after);

				ready_job->hw = hw;
				us_workers_pool_assign(stream->enc->run->pool, ready_wr);
				US_LOG_DEBUG("Assigned new frame in buffer=%d to worker=%s", buf_index, ready_wr->name);

				_SINK_PUT(raw_sink, &hw->raw);
				_H264_PUT(&hw->raw, h264_force_key);
			}
		}

	close:
		atomic_store(&run->release_stop, true);
		for (uint index = 0; index < n_releasers; ++index) {
			US_THREAD_JOIN(run->releasers[index].tid);
			us_queue_destroy(run->releasers[index].queue);
		}
		free(run->releasers);
		atomic_store(&run->release_stop, false);

		us_encoder_close(stream->enc);
		us_device_close(stream->dev);

#		ifdef WITH_GPIO
		us_gpio_set_stream_online(false);
#		endif
	}

	US_DELETE(run->h264, us_h264_stream_destroy);
}

void us_stream_loop_break(us_stream_s *stream) {
	atomic_store(&stream->run->stop, true);
}

static void *_releaser_thread(void *v_ctx) {
	_releaser_context_s *ctx = v_ctx;
	while (!atomic_load(ctx->stop)) {
		us_hw_buffer_s *hw;
		if (!us_queue_get(ctx->queue, (void**)&hw, 0.1)) {
			US_MUTEX_LOCK(*ctx->mutex);
			const int released = us_device_release_buffer(ctx->dev, hw);
			US_MUTEX_UNLOCK(*ctx->mutex);
			if (released < 0) {
				break;
			}
		}
	}
	atomic_store(ctx->stop, true); // Stop all other guys
	free(ctx);
	return NULL;
}

static void _stream_release_buffer(us_stream_s *stream, us_hw_buffer_s *hw) {
	assert(!us_queue_put(stream->run->releasers[hw->buf.index].queue, hw, 0));
}

static bool _stream_is_stopped(us_stream_s *stream) {
	us_stream_runtime_s *const run = stream->run;
	const bool stop = atomic_load(&run->stop);
	if (stop) {
		return true;
	}
	if (stream->exit_on_no_clients > 0) {
		const ldf now_ts = us_get_now_monotonic();
		const ull http_last_request_ts = atomic_load(&run->http_last_request_ts); // Seconds
		if (_stream_has_any_clients(stream)) {
			atomic_store(&run->http_last_request_ts, now_ts);
		} else if (http_last_request_ts + stream->exit_on_no_clients < now_ts) {
			US_LOG_INFO("No requests or HTTP/sink clients found in last %u seconds, exiting ...",
				stream->exit_on_no_clients);
			us_process_suicide();
			atomic_store(&run->http_last_request_ts, now_ts);
		}
	}
	return false;
}

static bool _stream_has_any_clients(us_stream_s *stream) {
	const us_stream_runtime_s *const run = stream->run;
	return (
		atomic_load(&run->http_has_clients)
		// has_clients синков НЕ обновляются в реальном времени
		|| (stream->jpeg_sink != NULL && atomic_load(&stream->jpeg_sink->has_clients))
		|| (run->h264 != NULL && /*run->h264->sink == NULL ||*/ atomic_load(&run->h264->sink->has_clients))
	);
}

static bool _stream_slowdown(us_stream_s *stream) {
	if (stream->slowdown) {
		unsigned count = 0;
		while (count < 10 && !_stream_is_stopped(stream) && !_stream_has_any_clients(stream)) {
			usleep(100000);
			++count;
		}
		return (count >= 10);
	}
	return false;
}

static int _stream_init_loop(us_stream_s *stream) {
	us_stream_runtime_s *const run = stream->run;

	int access_errno = 0;
	while (!_stream_is_stopped(stream)) {
		unsigned width = stream->dev->run->width;
		unsigned height = stream->dev->run->height;
		if (width == 0 || height == 0) {
			width = stream->dev->width;
			height = stream->dev->height;
		}
		us_blank_draw(run->blank, "< NO SIGNAL >", width, height);

		atomic_store(&run->http_captured_fps, 0);
		_stream_expose_frame(stream, NULL);

		_SINK_PUT(raw_sink, run->blank->raw);
		_H264_PUT(run->blank->raw, false);

		if (access(stream->dev->path, R_OK|W_OK) < 0) {
			if (access_errno != errno) {
				US_SEP_INFO('=');
				US_LOG_PERROR("Can't access device");
				US_LOG_INFO("Waiting for the device access ...");
				access_errno = errno;
			}
			goto sleep_and_retry;
		}

		US_SEP_INFO('=');
		access_errno = 0;

		stream->dev->dma_export = (
			stream->enc->type == US_ENCODER_TYPE_M2M_VIDEO
			|| stream->enc->type == US_ENCODER_TYPE_M2M_IMAGE
			|| run->h264 != NULL
		);
		if (us_device_open(stream->dev) == 0) {
			us_encoder_open(stream->enc, stream->dev);
			return 0;
		}
		US_LOG_INFO("Sleeping %u seconds before new stream init ...", stream->error_delay);

	sleep_and_retry:
		sleep(stream->error_delay);
	}
	return -1;
}

static void _stream_expose_frame(us_stream_s *stream, us_frame_s *frame) {
	us_stream_runtime_s *const run = stream->run;

	us_frame_s *new = NULL;

	if (frame != NULL) {
		new = frame;
		run->last_as_blank_ts = 0; // Останавливаем таймер
		US_LOG_DEBUG("Exposed ALIVE video frame");

	} else {
		if (run->last_online) { // Если переходим из online в offline
			if (stream->last_as_blank < 0) { // Если last_as_blank выключен, просто покажем старую картинку
				new = run->blank->jpeg;
				US_LOG_INFO("Changed video frame to BLANK");
			} else if (stream->last_as_blank > 0) { // // Если нужен таймер - запустим
				run->last_as_blank_ts = us_get_now_monotonic() + stream->last_as_blank;
				US_LOG_INFO("Freezed last ALIVE video frame for %d seconds", stream->last_as_blank);
			} else {  // last_as_blank == 0 - показываем последний фрейм вечно
				US_LOG_INFO("Freezed last ALIVE video frame forever");
			}
		} else if (stream->last_as_blank < 0) {
			new = run->blank->jpeg;
			// US_LOG_INFO("Changed video frame to BLANK");
		}

		if ( // Если уже оффлайн, включена фича last_as_blank с таймером и он запущен
			stream->last_as_blank > 0
			&& run->last_as_blank_ts != 0
			&& run->last_as_blank_ts < us_get_now_monotonic()
		) {
			new = run->blank->jpeg;
			run->last_as_blank_ts = 0; // Останавливаем таймер
			US_LOG_INFO("Changed last ALIVE video frame to BLANK");
		}
	}

	int ri = -1;
	while (
		!_stream_is_stopped(stream)
		&& ((ri = us_ring_producer_acquire(run->http_jpeg_ring, 0)) < 0)
	) {
		US_LOG_ERROR("Can't push JPEG to HTTP ring (no free slots)");
	}
	if (ri < 0) {
		return;
	}

	us_frame_s *const dest = run->http_jpeg_ring->items[ri];
	if (new == NULL) {
		dest->used = 0;
		dest->online = false;
	} else {
		us_frame_copy(new, dest);
		dest->online = true;
	}
	run->last_online = (frame != NULL);
	us_ring_producer_release(run->http_jpeg_ring, ri);

	_SINK_PUT(jpeg_sink, (frame != NULL ? frame : run->blank->jpeg));
}
