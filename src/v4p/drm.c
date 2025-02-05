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


#include "drm.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <linux/videodev2.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libdrm/drm.h>

#include "../libs/types.h"
#include "../libs/tools.h"
#include "../libs/logging.h"
#include "../libs/frame.h"
#include "../libs/frametext.h"


static void _drm_vsync_callback(int fd, uint n_frame, uint sec, uint usec, void *v_buf);
static int _drm_check_status(us_drm_s *drm);
static void _drm_ensure_dpms_power(us_drm_s *drm, bool on);
static int _drm_init_buffers(us_drm_s *drm, const us_device_s *dev);
static int _drm_find_sink(us_drm_s *drm, uint width, uint height, float hz);

static drmModeModeInfo *_find_best_mode(drmModeConnector *conn, uint width, uint height, float hz);
static u32 _find_dpms(int fd, drmModeConnector *conn);
static u32 _find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, u32 *taken_crtcs);
static const char *_connector_type_to_string(u32 type);
static float _get_refresh_rate(const drmModeModeInfo *mode);


#define _D_LOG_ERROR(x_msg, ...)	US_LOG_ERROR("DRM: " x_msg, ##__VA_ARGS__)
#define _D_LOG_PERROR(x_msg, ...)	US_LOG_PERROR("DRM: " x_msg, ##__VA_ARGS__)
#define _D_LOG_INFO(x_msg, ...)		US_LOG_INFO("DRM: " x_msg, ##__VA_ARGS__)
#define _D_LOG_VERBOSE(x_msg, ...)	US_LOG_VERBOSE("DRM: " x_msg, ##__VA_ARGS__)
#define _D_LOG_DEBUG(x_msg, ...)	US_LOG_DEBUG("DRM: " x_msg, ##__VA_ARGS__)


us_drm_s *us_drm_init(void) {
	us_drm_runtime_s *run;
	US_CALLOC(run, 1);
	run->fd = -1;
	run->status_fd = -1;
	run->dpms_state = -1;
	run->has_vsync = true;
	run->exposing_dma_fd = -1;
	run->ft = us_frametext_init();

	us_drm_s *drm;
	US_CALLOC(drm, 1);
	// drm->path = "/dev/dri/card0";
	drm->path = "/dev/dri/by-path/platform-gpu-card";
	drm->port = "HDMI-A-1";
	drm->timeout = 5;
	drm->run = run;
	return drm;
}

void us_drm_destroy(us_drm_s *drm) {
	us_frametext_destroy(drm->run->ft);
	US_DELETE(drm->run, free);
	US_DELETE(drm, free); // cppcheck-suppress uselessAssignmentPtrArg
}

int us_drm_open(us_drm_s *drm, const us_device_s *dev) {
	us_drm_runtime_s *const run = drm->run;

	assert(run->fd < 0);

	switch (_drm_check_status(drm)) {
		case 0: break;
		case -2: goto unplugged;
		default: goto error;
	}

	_D_LOG_INFO("Configuring DRM device for %s ...", (dev == NULL ? "STUB" : "DMA"));

	if ((run->fd = open(drm->path, O_RDWR | O_CLOEXEC | O_NONBLOCK)) < 0) {
		_D_LOG_PERROR("Can't open DRM device");
		goto error;
	}
	_D_LOG_DEBUG("DRM device fd=%d opened", run->fd);

	int stub = 0; // Open the real device with DMA
	if (dev == NULL) {
		stub = US_DRM_STUB_USER;
	} else if (dev->run->format != V4L2_PIX_FMT_RGB24) {
		stub = US_DRM_STUB_BAD_FORMAT;
		char fourcc_str[8];
		us_fourcc_to_string(dev->run->format, fourcc_str, 8);
		_D_LOG_ERROR("Input format %s is not supported, forcing to STUB ...", fourcc_str);
	}

#	define CHECK_CAP(x_cap) { \
			_D_LOG_DEBUG("Checking %s ...", #x_cap); \
			u64 m_check; \
			if (drmGetCap(run->fd, x_cap, &m_check) < 0) { \
				_D_LOG_PERROR("Can't check " #x_cap); \
				goto error; \
			} \
			if (!m_check) { \
				_D_LOG_ERROR(#x_cap " is not supported"); \
				goto error; \
			} \
		}
	CHECK_CAP(DRM_CAP_DUMB_BUFFER);
	if (stub == 0) {
		CHECK_CAP(DRM_CAP_PRIME);
	}
#	undef CHECK_CAP

	const uint width = (stub > 0 ? 0 : dev->run->width);
	const uint height = (stub > 0 ? 0 : dev->run->height);
	const uint hz = (stub > 0 ? 0 : dev->run->hz);
	switch (_drm_find_sink(drm, width, height, hz)) {
		case 0: break;
		case -2: goto unplugged;
		default: goto error;
	}
	if ((stub == 0) && (width != run->mode.hdisplay || height < run->mode.vdisplay)) {
		// We'll try to show something instead of nothing if height != vdisplay
		stub = US_DRM_STUB_BAD_RESOLUTION;
		_D_LOG_ERROR("There is no appropriate modes for the capture, forcing to STUB ...");
	}

	if (_drm_init_buffers(drm, (stub > 0 ? NULL : dev)) < 0) {
		goto error;
	}

	run->saved_crtc = drmModeGetCrtc(run->fd, run->crtc_id);
	_D_LOG_DEBUG("Setting up CRTC ...");
	if (drmModeSetCrtc(run->fd, run->crtc_id, run->bufs[0].id, 0, 0, &run->conn_id, 1, &run->mode) < 0) {
		_D_LOG_PERROR("Can't set CRTC");
		goto error;
	}

	run->opened_for_stub = (stub > 0);
	run->exposing_dma_fd = -1;
	run->unplugged_reported = false;
	_D_LOG_INFO("Opened for %s ...", (run->opened_for_stub ? "STUB" : "DMA"));
	return stub;

error:
	us_drm_close(drm);
	return -1;

unplugged:
	if (!run->unplugged_reported) {
		_D_LOG_ERROR("Display is not plugged");
		run->unplugged_reported = true;
	}
	us_drm_close(drm);
	return -2;
}

void us_drm_close(us_drm_s *drm) {
	us_drm_runtime_s *const run = drm->run;

	if (run->exposing_dma_fd >= 0) {
		// Нужно подождать, пока dma_fd не освободится, прежде чем прерывать процесс.
		// Просто на всякий случай.
		assert(run->fd >= 0);
		us_drm_wait_for_vsync(drm);
		run->exposing_dma_fd = -1;
	}

	if (run->saved_crtc != NULL) {
		_D_LOG_DEBUG("Restoring CRTC ...");
		if (drmModeSetCrtc(run->fd,
			run->saved_crtc->crtc_id, run->saved_crtc->buffer_id,
			run->saved_crtc->x, run->saved_crtc->y,
			&run->conn_id, 1, &run->saved_crtc->mode
		) < 0 && errno != ENOENT) {
			_D_LOG_PERROR("Can't restore CRTC");
		}
		drmModeFreeCrtc(run->saved_crtc);
		run->saved_crtc = NULL;
	}

	if (run->bufs != NULL) {
		_D_LOG_DEBUG("Releasing buffers ...");
		for (uint n_buf = 0; n_buf < run->n_bufs; ++n_buf) {
			us_drm_buffer_s *const buf = &run->bufs[n_buf];
			if (buf->fb_added && drmModeRmFB(run->fd, buf->id) < 0) {
				_D_LOG_PERROR("Can't remove buffer=%u", n_buf);
			}
			if (buf->dumb_created) {
				struct drm_mode_destroy_dumb destroy = {.handle = buf->handle};
				if (drmIoctl(run->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0) {
					_D_LOG_PERROR("Can't destroy dumb buffer=%u", n_buf);
				}
			}
			if (buf->data != NULL && munmap(buf->data, buf->allocated)) {
				_D_LOG_PERROR("Can't unmap buffer=%u", n_buf);
			}
		}
		US_DELETE(run->bufs, free);
		run->n_bufs = 0;
	}

	const bool say = (run->fd >= 0);
	US_CLOSE_FD(run->status_fd);
	US_CLOSE_FD(run->fd);

	run->crtc_id = 0;
	run->dpms_state = -1;
	run->has_vsync = true;
	run->stub_n_buf = 0;

	if (say) {
		_D_LOG_INFO("Closed");
	}
}

int us_drm_dpms_power_off(us_drm_s *drm) {
	assert(drm->run->fd >= 0);
	switch (_drm_check_status(drm)) {
		case 0: break;
		case -2: return 0; // Unplugged, nice
		// Во время переключения DPMS монитор моргает один раз состоянием disconnected,
		// а потом почему-то снова оказывается connected. Так что просто считаем,
		// что отсоединенный монитор на этом этапе - это нормально.
		default: return -1;
	}
	_drm_ensure_dpms_power(drm, false);
	return 0;
}

int us_drm_wait_for_vsync(us_drm_s *drm) {
	us_drm_runtime_s *const run = drm->run;

	assert(run->fd >= 0);

	switch (_drm_check_status(drm)) {
		case 0: break;
		case -2: return -2;
		default: return -1;
	}
	_drm_ensure_dpms_power(drm, true);

	if (run->has_vsync) {
		return 0;
	}

	struct timeval timeout = {.tv_sec = drm->timeout};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(run->fd, &fds);

	_D_LOG_DEBUG("Calling select() for VSync ...");
	const int result = select(run->fd + 1, &fds, NULL, NULL, &timeout);
	if (result < 0) {
		_D_LOG_PERROR("Can't select(%d) device for VSync", run->fd);
		return -1;
	} else if (result == 0) {
		_D_LOG_ERROR("Device timeout while waiting VSync");
		return -1;
	}

	drmEventContext ctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = _drm_vsync_callback,
	};
	_D_LOG_DEBUG("Handling DRM event (maybe VSync) ...");
	if (drmHandleEvent(run->fd, &ctx) < 0) {
		_D_LOG_PERROR("Can't handle DRM event");
		return -1;
	}
	return 0;
}

static void _drm_vsync_callback(int fd, uint n_frame, uint sec, uint usec, void *v_buf) {
	(void)fd;
	(void)n_frame;
	(void)sec;
	(void)usec;
	us_drm_buffer_s *const buf = v_buf;
	*buf->ctx.has_vsync = true;
	*buf->ctx.exposing_dma_fd = -1;
	_D_LOG_DEBUG("Got VSync signal");
}

int us_drm_expose_stub(us_drm_s *drm, us_drm_stub_e stub, const us_device_s *dev) {
	us_drm_runtime_s *const run = drm->run;

	assert(run->fd >= 0);
	assert(run->opened_for_stub);

	switch (_drm_check_status(drm)) {
		case 0: break;
		case -2: return -2;
		default: return -1;
	}
	_drm_ensure_dpms_power(drm, true);

#	define DRAW_MSG(x_msg) us_frametext_draw(run->ft, (x_msg), run->mode.hdisplay, run->mode.vdisplay)
	switch (stub) {
		case US_DRM_STUB_BAD_RESOLUTION: {
			assert(dev != NULL);
			char msg[1024];
			US_SNPRINTF(msg, 1023,
				"=== PiKVM ==="
				"\n \n< UNSUPPORTED RESOLUTION >"
				"\n \n< %ux%up%.02f >"
				"\n \nby this display",
				dev->run->width, dev->run->height, dev->run->hz);
			DRAW_MSG(msg);
			break;
		};
		case US_DRM_STUB_BAD_FORMAT:
			DRAW_MSG(
				"=== PiKVM ==="
				"\n \n< UNSUPPORTED CAPTURE FORMAT >"
				"\n \nIt shouldn't happen ever."
				"\n \nPlease check the logs and report a bug:"
				"\n \n- https://github.com/pikvm/pikvm -");
			break;
		case US_DRM_STUB_NO_SIGNAL:
			DRAW_MSG("=== PiKVM ===\n \n< NO SIGNAL >");
			break;
		case US_DRM_STUB_BUSY:
			DRAW_MSG("=== PiKVM ===\n \n< ONLINE IS ACTIVE >");
			break;
		default:
			DRAW_MSG("=== PiKVM ===\n \n< ??? >");
			break;
	}
#	undef DRAW_MSG

	us_drm_buffer_s *const buf = &run->bufs[run->stub_n_buf];

	run->has_vsync = false;

	_D_LOG_DEBUG("Copying STUB frame ...")
	memcpy(buf->data, run->ft->frame->data, US_MIN(run->ft->frame->used, buf->allocated));

	_D_LOG_DEBUG("Exposing STUB framebuffer n_buf=%u ...", run->stub_n_buf);
	const int retval = drmModePageFlip(
		run->fd, run->crtc_id, buf->id,
		DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC,
		buf);
	if (retval < 0) {
		_D_LOG_PERROR("Can't expose STUB framebuffer n_buf=%u ...", run->stub_n_buf);
	}
	_D_LOG_DEBUG("Exposed STUB framebuffer n_buf=%u", run->stub_n_buf);

	run->stub_n_buf = (run->stub_n_buf + 1) % run->n_bufs;
	return retval;
}

int us_drm_expose_dma(us_drm_s *drm, const us_hw_buffer_s *hw) {
	us_drm_runtime_s *const run = drm->run;
	us_drm_buffer_s *const buf = &run->bufs[hw->buf.index];

	assert(run->fd >= 0);
	assert(!run->opened_for_stub);

	switch (_drm_check_status(drm)) {
		case 0: break;
		case -2: return -2;
		default: return -1;
	}
	_drm_ensure_dpms_power(drm, true);

	run->has_vsync = false;

	_D_LOG_DEBUG("Exposing DMA framebuffer n_buf=%u ...", hw->buf.index);
	const int retval = drmModePageFlip(
		run->fd, run->crtc_id, buf->id,
		DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC,
		buf);
	if (retval < 0) {
		_D_LOG_PERROR("Can't expose DMA framebuffer n_buf=%u ...", run->stub_n_buf);
	}
	_D_LOG_DEBUG("Exposed DMA framebuffer n_buf=%u", run->stub_n_buf);
	run->exposing_dma_fd = hw->dma_fd;
	return retval;
}

static int _drm_check_status(us_drm_s *drm) {
	us_drm_runtime_s *run = drm->run;

	if (run->status_fd < 0) {
		_D_LOG_DEBUG("Trying to find status file ...");
		struct stat st;
		if (stat(drm->path, &st) < 0) {
			_D_LOG_PERROR("Can't stat() DRM device");
			goto error;
		}
		const uint mi = minor(st.st_rdev);
		_D_LOG_DEBUG("DRM device minor(st_rdev)=%u", mi);

		char path[128];
		US_SNPRINTF(path, 127, "/sys/class/drm/card%u-%s/status", mi, drm->port);
		_D_LOG_DEBUG("Opening status file %s ...", path);
		if ((run->status_fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
			_D_LOG_PERROR("Can't open status file: %s", path);
			goto error;
		}
		_D_LOG_DEBUG("Status file fd=%d opened", run->status_fd);
	}

	char status_ch;
	if (read(run->status_fd, &status_ch, 1) != 1) {
		_D_LOG_PERROR("Can't read status file");
		goto error;
	}
	if (lseek(run->status_fd, 0, SEEK_SET) != 0) {
		_D_LOG_PERROR("Can't rewind status file");
		goto error;
	}
	_D_LOG_DEBUG("Current display status: %c", status_ch);
	return (status_ch == 'd' ? -2 : 0);

error:
	US_CLOSE_FD(run->status_fd);
	return -1;
}

static void _drm_ensure_dpms_power(us_drm_s *drm, bool on) {
	us_drm_runtime_s *const run = drm->run;
	if (run->dpms_id > 0 && run->dpms_state != (int)on) {
		_D_LOG_INFO("Changing DPMS power mode: %d -> %u ...", run->dpms_state, on);
		if (drmModeConnectorSetProperty(
			run->fd, run->conn_id, run->dpms_id,
			(on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF)
		) < 0) {
			_D_LOG_PERROR("Can't set DPMS power=%u (ignored)", on);
		}
	}
	run->dpms_state = (int)on;
}

static int _drm_init_buffers(us_drm_s *drm, const us_device_s *dev) {
	us_drm_runtime_s *const run = drm->run;

	const uint n_bufs = (dev == NULL ? 4 : dev->run->n_bufs);
	const char *name = (dev == NULL ? "STUB" : "DMA");

	_D_LOG_DEBUG("Initializing %u %s buffers ...", n_bufs, name);

	US_CALLOC(run->bufs, n_bufs);
	for (run->n_bufs = 0; run->n_bufs < n_bufs; ++run->n_bufs) {
		const uint n_buf = run->n_bufs;
		us_drm_buffer_s *const buf = &run->bufs[n_buf];

		buf->ctx.has_vsync = &run->has_vsync;
		buf->ctx.exposing_dma_fd = &run->exposing_dma_fd;

		u32 handles[4] = {0};
		u32 strides[4] = {0};
		u32 offsets[4] = {0};

		if (dev == NULL) {
			struct drm_mode_create_dumb create = {
				.width = run->mode.hdisplay,
				.height = run->mode.vdisplay,
				.bpp = 24,
			};
			if (drmIoctl(run->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
				_D_LOG_PERROR("Can't create %s buffer=%u", name, n_buf);
				return -1;
			}
			buf->handle = create.handle;
			buf->dumb_created = true;

			struct drm_mode_map_dumb map = {.handle = create.handle};
			if (drmIoctl(run->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
				_D_LOG_PERROR("Can't prepare dumb buffer=%u to mapping", n_buf);
				return -1;
			}
			if ((buf->data = mmap(
				NULL, create.size,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				run->fd, map.offset
			)) == MAP_FAILED) {
				_D_LOG_PERROR("Can't map buffer=%u", n_buf);
				return -1;
			}
			memset(buf->data, 0, create.size);
			buf->allocated = create.size;

			handles[0] = create.handle;
			strides[0] = create.pitch;

		} else {
			if (drmPrimeFDToHandle(run->fd, dev->run->hw_bufs[n_buf].dma_fd, &buf->handle) < 0) {
				_D_LOG_PERROR("Can't import DMA buffer=%u from capture device", n_buf);
				return -1;
			}
			handles[0] = buf->handle;
			strides[0] = dev->run->stride;
		}

		if (drmModeAddFB2(
			run->fd,
			run->mode.hdisplay, run->mode.vdisplay, DRM_FORMAT_RGB888,
			handles, strides, offsets, &buf->id, 0
		)) {
			_D_LOG_PERROR("Can't setup buffer=%u", n_buf);
			return -1;
		}
		buf->fb_added = true;
	}
	return 0;
}

static int _drm_find_sink(us_drm_s *drm, uint width, uint height, float hz) {
	us_drm_runtime_s *const run = drm->run;

	run->crtc_id = 0;

	_D_LOG_DEBUG("Trying to find the appropriate sink ...");

	drmModeRes *res = drmModeGetResources(run->fd);
	if (res == NULL) {
		_D_LOG_PERROR("Can't get resources info");
		goto done;
	}
	if (res->count_connectors <= 0) {
		_D_LOG_ERROR("Can't find any connectors");
		goto done;
	}

	for (int ci = 0; ci < res->count_connectors; ++ci) {
		drmModeConnector *conn = drmModeGetConnector(run->fd, res->connectors[ci]);
		if (conn == NULL) {
			_D_LOG_PERROR("Can't get connector index=%d", ci);
			goto done;
		}

		char port[32];
		US_SNPRINTF(port, 31, "%s-%u",
			_connector_type_to_string(conn->connector_type),
			conn->connector_type_id);
		if (strcmp(port, drm->port) != 0) {
			drmModeFreeConnector(conn);
			continue;
		}
		_D_LOG_INFO("Using connector %s: conn_type=%d, conn_type_id=%d",
			drm->port, conn->connector_type, conn->connector_type_id);

		if (conn->connection != DRM_MODE_CONNECTED) {
			_D_LOG_ERROR("Connector for port %s has !DRM_MODE_CONNECTED", drm->port);
			drmModeFreeConnector(conn);
			goto done;
		}

		drmModeModeInfo *best;
		if ((best = _find_best_mode(conn, width, height, hz)) == NULL) {
			_D_LOG_ERROR("Can't find any appropriate display modes");
			drmModeFreeConnector(conn);
			goto unplugged;
		}
		_D_LOG_INFO("Using best mode: %ux%up%.02f",
			best->hdisplay, best->vdisplay, _get_refresh_rate(best));

		if ((run->dpms_id = _find_dpms(run->fd, conn)) > 0) {
			_D_LOG_INFO("Using DPMS: id=%u", run->dpms_id);
		} else {
			_D_LOG_INFO("Using DPMS: None");
		}

		u32 taken_crtcs = 0; // Unused here
		if ((run->crtc_id = _find_crtc(run->fd, res, conn, &taken_crtcs)) == 0) {
			_D_LOG_ERROR("Can't find CRTC");
			drmModeFreeConnector(conn);
			goto done;
		}
		_D_LOG_INFO("Using CRTC: id=%u", run->crtc_id);

		run->conn_id = conn->connector_id;
		memcpy(&run->mode, best, sizeof(drmModeModeInfo));

		drmModeFreeConnector(conn);
		break;
	}

done:
	drmModeFreeResources(res);
	return (run->crtc_id > 0 ? 0 : -1);

unplugged:
	drmModeFreeResources(res);
	return -2;
}

static drmModeModeInfo *_find_best_mode(drmModeConnector *conn, uint width, uint height, float hz) {
	drmModeModeInfo *best = NULL;
	drmModeModeInfo *closest = NULL;
	drmModeModeInfo *pref = NULL;

	for (int mi = 0; mi < conn->count_modes; ++mi) {
		drmModeModeInfo *const mode = &conn->modes[mi];
		if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
			continue; // Discard interlaced
		}
		const float mode_hz = _get_refresh_rate(mode);
		if (mode->hdisplay == width && mode->vdisplay == height) {
			best = mode; // Any mode with exact resolution
			if (hz > 0 && mode_hz == hz) {
				break; // Exact mode with same freq
			}
		}
		if (mode->hdisplay == width && mode->vdisplay < height) {
			if (closest == NULL || _get_refresh_rate(closest) != hz) {
				closest = mode; // Something like 1920x1080p60 for 1920x1200p60 source
			}
		}
		if (pref == NULL && (mode->type & DRM_MODE_TYPE_PREFERRED)) {
			pref = mode; // Preferred mode if nothing is found
		}
	}

	if (best == NULL) {
		best = closest;
	}
	if (best == NULL) {
		best = pref;
	}
	if (best == NULL) {
		best = (conn->count_modes > 0 ? &conn->modes[0] : NULL);
	}
	assert(best == NULL || best->hdisplay > 0);
	assert(best == NULL || best->vdisplay > 0);
	return best;
}

static u32 _find_dpms(int fd, drmModeConnector *conn) {
	for (int pi = 0; pi < conn->count_props; pi++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, conn->props[pi]);
		if (prop != NULL) {
			if (!strcmp(prop->name, "DPMS")) {
				const u32 id = prop->prop_id;
				drmModeFreeProperty(prop);
				return id;
			}
			drmModeFreeProperty(prop);
		}
	}
	return 0;
}

static u32 _find_crtc(int fd, drmModeRes *res, drmModeConnector *conn, u32 *taken_crtcs) {
	for (int ei = 0; ei < conn->count_encoders; ++ei) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[ei]);
		if (enc == NULL) {
			continue;
		}
		for (int ci = 0; ci < res->count_crtcs; ++ci) {
			u32 bit = (1 << ci);
			if (!(enc->possible_crtcs & bit)) {
				continue; // Not compatible
			}
			if (*taken_crtcs & bit) {
				continue; // Already taken
			}
			drmModeFreeEncoder(enc);
			*taken_crtcs |= bit;
			return res->crtcs[ci];
		}
		drmModeFreeEncoder(enc);
	}
	return 0;
}

static const char *_connector_type_to_string(u32 type) {
	switch (type) {
#		define CASE_NAME(x_suffix, x_name) \
			case DRM_MODE_CONNECTOR_##x_suffix: return x_name;
		CASE_NAME(VGA,			"VGA");
		CASE_NAME(DVII,			"DVI-I");
		CASE_NAME(DVID,			"DVI-D");
		CASE_NAME(DVIA,			"DVI-A");
		CASE_NAME(Composite,	"Composite");
		CASE_NAME(SVIDEO,		"SVIDEO");
		CASE_NAME(LVDS,			"LVDS");
		CASE_NAME(Component,	"Component");
		CASE_NAME(9PinDIN,		"DIN");
		CASE_NAME(DisplayPort,	"DP");
		CASE_NAME(HDMIA,		"HDMI-A");
		CASE_NAME(HDMIB,		"HDMI-B");
		CASE_NAME(TV,			"TV");
		CASE_NAME(eDP,			"eDP");
		CASE_NAME(VIRTUAL,		"Virtual");
		CASE_NAME(DSI,			"DSI");
		CASE_NAME(DPI,			"DPI");
		CASE_NAME(WRITEBACK,	"Writeback");
		CASE_NAME(SPI,			"SPI");
		CASE_NAME(USB,			"USB");
		case DRM_MODE_CONNECTOR_Unknown: break;
#		undef CASE_NAME
	}
	return "Unknown";
}

static float _get_refresh_rate(const drmModeModeInfo *mode) {
	int mhz = (mode->clock * 1000000LL / mode->htotal + mode->vtotal / 2) / mode->vtotal;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		mhz *= 2;
	}
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN) {
		mhz /= 2;
	}
	if (mode->vscan > 1) {
		mhz /= mode->vscan;
	}
	return (float)mhz / 1000;
}
