/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * H.264 Bitmap Compression — HarmonyOS (OHOS) Hardware Decoder
 *
 * Copyright 2025 hFreeRDP contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/wlog.h>
#include <winpr/assert.h>

#include <freerdp/log.h>
#include <freerdp/codec/h264.h>

#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcapability.h>

#ifdef WITH_OHOS_HWCODEC_SURFACE
#include <native_window/external_window.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#include "h264.h"

#define OHOS_CODEC_MIME OH_AVCODEC_MIMETYPE_VIDEO_AVC

static const int OHOS_MINIMUM_WIDTH = 320;
static const int OHOS_MINIMUM_HEIGHT = 240;

/* Frame-stall watchdog bridge — defined in libentry (gdi_bridge.cpp).
 * Available in both surface and buffer modes. errorKind: 1=CODEC 2=TIMEOUT 3=PERMANENT 4=OK. */
extern void surface_decoder_report_decode_error(void* session, void* surfaceKey,
                                                int errorKind, int consecutiveErrors,
                                                int consecutiveTimeouts);
extern int surface_decoder_get_recovery_version(void* session);
extern int surface_decoder_take_recovery_flags(void* session);

#ifdef WITH_OHOS_HWCODEC_SURFACE
/* External bridge functions — defined in libentry (gdi_bridge.cpp).
 * Resolved at final link time when FreeRDP static lib is linked into libentry.so. */
extern OHNativeWindow* surface_decoder_request_native_window(void* session, void* surfaceKey,
                                                             int width, int height,
                                                             int originX, int originY,
                                                             int contentW, int contentH);
extern OHNativeWindow* surface_decoder_reuse_or_create_native_window(void* session, void* surfaceKey,
                                                                      int width, int height,
                                                                      int originX, int originY,
                                                                      int contentW, int contentH);
extern void surface_decoder_notify_frame(void* session, void* surfaceKey);
extern void surface_decoder_destroy_native_image(void* session, void* surfaceKey);
extern void surface_decoder_deactivate_oes(void* session, void* surfaceKey);
extern void surface_decoder_activate_oes(void* session, void* surfaceKey);
extern void surface_decoder_request_refresh(void* session);
extern void surface_decoder_update_output_size(void* session, void* surfaceKey, int width, int height);
extern void surface_decoder_update_crop_rect(void* session, void* surfaceKey, int top, int bottom, int left, int right);
extern bool surface_decoder_is_permanently_unavailable(void* session, void* surfaceKey);
#endif

typedef struct
{
	OH_AVCodec* decoder;
	OH_AVFormat* format;
	int32_t width;
	int32_t height;
	int32_t outputWidth;
	int32_t outputHeight;
	int32_t outputStride;

	/* Synchronization: OHOS uses async callbacks, but FreeRDP Decompress is synchronous */
	pthread_mutex_t inputMutex;
	pthread_cond_t inputCond;
	int32_t inputIndex;
	OH_AVBuffer* inputBuffer;
	bool inputReady;

	pthread_mutex_t outputMutex;
	pthread_cond_t outputCond;
	int32_t outputIndex;
	OH_AVBuffer* outputBuffer;
	bool outputReady;
	bool outputEos;
	bool formatChanged;

	/* Written by the codec callback thread (ohos_on_error) and read/written by
	 * the RDP event thread without a common lock — must be atomic. Plain
	 * load/store suffices (no ordering requirements beyond visibility). */
	atomic_bool codecError;

	/* Auto-recovery: consecutive decode errors counter. Atomic because in
	 * surface mode it is cleared from the codec output callback thread
	 * (surface_on_output) while the event thread increments/reads it. */
	atomic_int consecutiveErrors;

	/* Saved format parameters for decoder reset/recovery */
	int32_t savedWidth;
	int32_t savedHeight;
	bool savedSurfaceMode;

	/* NV12→I420 conversion buffer */
	uint8_t* chromaConvBuf;
	size_t chromaConvBufSize;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	bool decoderStarted;     /* Decoder configured and started (deferred from init) */
	bool surfaceMode;        /* Currently using Surface mode */
	OHNativeWindow* surfaceWindow;  /* NativeImage-provided output window */
	int surfaceRetryCount;   /* Surface upgrade retry count */
	int outputFrameCount;    /* Total output frames (diagnostic) */
	bool hasSpsReceived;     /* True once decoder has received SPS/PPS in surface mode */
#else
	bool decoderStarted;     /* Decoder configured and started (deferred from init) */
#endif

	int totalDecompressCalls; /* Total decompress calls (for first-frame tolerance) */
	int consecutiveTimeouts;  /* Consecutive output timeout count */
	bool hasProducedOutput;   /* True once decoder has produced at least one frame */

	int appliedRecoveryVersion; /* Last watchdog recovery-request version applied (per H264_CONTEXT) */
	bool recoveryVersionSeeded; /* appliedRecoveryVersion has been synced with the bridge at least
	                             * once for this context — a freshly created H264_CONTEXT must adopt
	                             * the current version WITHOUT applying stale flags left over from
	                             * recoveries that predate it (they target a decoder that no longer
	                             * exists). */
	uint64_t lastRefreshRequestMs; /* Monotonic ms of last recovery-triggered IDR refresh request
	                                * (throttle to avoid full-screen IDR refresh storms) */
} H264_CONTEXT_OHOS;

/* ======================================================================
 * Async callbacks — bridge OHOS async model to synchronous Decompress
 * ====================================================================== */

static void ohos_on_error(OH_AVCodec* codec, int32_t errorCode, void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys;
	(void)codec;

	if (!h264)
		return;

	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	WLog_Print(h264->log, WLOG_ERROR, "OHOS VideoDecoder error: %d", errorCode);

	if (sys)
	{
		sys->codecError = true;
		pthread_mutex_lock(&sys->inputMutex);
		sys->inputReady = true;
		pthread_cond_signal(&sys->inputCond);
		pthread_mutex_unlock(&sys->inputMutex);

		pthread_mutex_lock(&sys->outputMutex);
		sys->outputReady = true;
		pthread_cond_signal(&sys->outputCond);
		pthread_mutex_unlock(&sys->outputMutex);
	}
}

static void ohos_on_stream_changed(OH_AVCodec* codec, OH_AVFormat* format, void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys;
	int32_t w = 0, h = 0, stride = 0;
	(void)codec;

	if (!h264)
		return;
	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	if (!sys)
		return;

	if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w))
		sys->outputWidth = w;
	if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h))
		sys->outputHeight = h;
	if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride))
		sys->outputStride = stride;

	WLog_Print(h264->log, WLOG_DEBUG,
	           "OHOS stream changed: %dx%d stride=%d", sys->outputWidth, sys->outputHeight,
	           sys->outputStride);

	pthread_mutex_lock(&sys->outputMutex);
	sys->formatChanged = true;
	pthread_mutex_unlock(&sys->outputMutex);
}

static void ohos_on_need_input(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                               void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys;
	(void)codec;

	if (!h264)
		return;
	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	if (!sys)
		return;

	pthread_mutex_lock(&sys->inputMutex);
	sys->inputIndex = (int32_t)index;
	sys->inputBuffer = buffer;
	sys->inputReady = true;
	pthread_cond_signal(&sys->inputCond);
	pthread_mutex_unlock(&sys->inputMutex);
}

static void ohos_on_output(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys;
	(void)codec;

	if (!h264)
		return;
	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	if (!sys)
		return;

	pthread_mutex_lock(&sys->outputMutex);
	sys->outputIndex = (int32_t)index;
	sys->outputBuffer = buffer;
	sys->outputReady = true;
	pthread_cond_signal(&sys->outputCond);
	pthread_mutex_unlock(&sys->outputMutex);
}

/* ======================================================================
 * Wait helpers with timeout
 * ====================================================================== */

static bool wait_input_ready(H264_CONTEXT_OHOS* sys, int timeoutMs)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeoutMs / 1000;
	ts.tv_nsec += (timeoutMs % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&sys->inputMutex);
	while (!sys->inputReady && !sys->codecError)
	{
		if (pthread_cond_timedwait(&sys->inputCond, &sys->inputMutex, &ts) != 0)
		{
			pthread_mutex_unlock(&sys->inputMutex);
			return false;
		}
	}
	sys->inputReady = false;
	pthread_mutex_unlock(&sys->inputMutex);
	return !sys->codecError;
}

static bool wait_output_ready(H264_CONTEXT_OHOS* sys, int timeoutMs)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeoutMs / 1000;
	ts.tv_nsec += (timeoutMs % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&sys->outputMutex);
	while (!sys->outputReady && !sys->codecError)
	{
		if (pthread_cond_timedwait(&sys->outputCond, &sys->outputMutex, &ts) != 0)
		{
			pthread_mutex_unlock(&sys->outputMutex);
			return false;
		}
	}
	sys->outputReady = false;
	pthread_mutex_unlock(&sys->outputMutex);
	return !sys->codecError;
}

/* ======================================================================
 * NV12 → I420 plane separation (hardware decoders output NV12)
 * ====================================================================== */

static void nv12_to_i420_chroma(const uint8_t* nv12Uv, uint8_t* dstU, uint8_t* dstV,
                                int32_t chromaWidth, int32_t chromaHeight, int32_t uvStride)
{
	for (int32_t row = 0; row < chromaHeight; row++)
	{
		const uint8_t* src = nv12Uv + row * uvStride;
		for (int32_t col = 0; col < chromaWidth; col++)
		{
			dstU[col] = src[col * 2];
			dstV[col] = src[col * 2 + 1];
		}
		dstU += chromaWidth;
		dstV += chromaWidth;
	}
}

/* ======================================================================
 * Sync state helpers
 * ====================================================================== */

/**
 * Clear async callback state after decoder Reset.
 * Must be called after OH_VideoDecoder_Reset to discard stale input/output references.
 */
static void clear_sync_state(H264_CONTEXT_OHOS* sys)
{
	pthread_mutex_lock(&sys->inputMutex);
	sys->inputReady = false;
	sys->inputBuffer = NULL;
	sys->inputIndex = -1;
	pthread_mutex_unlock(&sys->inputMutex);

	pthread_mutex_lock(&sys->outputMutex);
	sys->outputReady = false;
	sys->outputBuffer = NULL;
	sys->outputIndex = -1;
	sys->formatChanged = false;
	pthread_mutex_unlock(&sys->outputMutex);
}

/* Monotonic milliseconds — for refresh-request throttling */
static uint64_t ohos_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
}

/**
 * Request a full-screen IDR refresh, throttled to at most once per 500ms.
 * Used only by the error-recovery path: if the hardware keeps failing every
 * frame while Reset keeps succeeding, an unthrottled refresh per attempt
 * would flood the server with full-frame re-encode requests (bandwidth storm).
 * One-shot refreshes (initial start / resize) call the bridge directly.
 */
static void ohos_request_refresh_throttled(H264_CONTEXT* h264, H264_CONTEXT_OHOS* sys)
{
	if (!h264->yuvReadyContext)
		return;

	const uint64_t now = ohos_now_ms();
	if (sys->lastRefreshRequestMs != 0 && (now - sys->lastRefreshRequestMs) < 500)
	{
		WLog_Print(h264->log, WLOG_DEBUG,
		           "OHOS decoder: IDR refresh throttled (last request %llums ago)",
		           (unsigned long long)(now - sys->lastRefreshRequestMs));
		return;
	}
	sys->lastRefreshRequestMs = now;
	surface_decoder_request_refresh(h264->yuvReadyContext);
}

/* ======================================================================
 * H.264 NAL unit inspection
 * ====================================================================== */

/**
 * Check if H.264 Annex-B bitstream contains an SPS NAL unit (type 7).
 * A freshly created hardware decoder MUST receive SPS/PPS before any slice data,
 * otherwise it reports "missing parameter sets" and cannot decode.
 */
static bool has_sps_nalu(const BYTE* data, UINT32 size)
{
	for (UINT32 i = 0; i + 4 < size; i++)
	{
		if (data[i] != 0 || data[i + 1] != 0)
			continue;

		UINT32 off = 0;
		if (data[i + 2] == 1)
			off = i + 3;
		else if (data[i + 2] == 0 && i + 4 < size && data[i + 3] == 1)
			off = i + 4;

		if (off > 0 && off < size && ((data[off] & 0x1F) == 7))
			return true;
	}
	return false;
}

/* ======================================================================
 * Deferred decoder start helpers
 * ====================================================================== */

/**
 * Start decoder in buffer mode (NV12 CPU output).
 * Registers buffer callbacks, configures, prepares and starts the decoder.
 */
static bool start_buffer_mode(H264_CONTEXT* h264, H264_CONTEXT_OHOS* sys, int32_t w, int32_t h)
{
	OH_AVErrCode err;
	OH_AVCodecCallback cb;

	cb.onError = ohos_on_error;
	cb.onStreamChanged = ohos_on_stream_changed;
	cb.onNeedInputBuffer = ohos_on_need_input;
	cb.onNewOutputBuffer = ohos_on_output;

	err = OH_VideoDecoder_RegisterCallback(sys->decoder, cb, (void*)h264);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: RegisterCallback failed in buffer mode: %d", err);
		return false;
	}

	OH_AVFormat* fmt = OH_AVFormat_Create();
	OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, w);
	OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, h);
	OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);

	err = OH_VideoDecoder_Configure(sys->decoder, fmt);
	OH_AVFormat_Destroy(fmt);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Configure failed in buffer mode: %d", err);
		return false;
	}

	err = OH_VideoDecoder_Prepare(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Prepare failed in buffer mode: %d", err);
		return false;
	}

	err = OH_VideoDecoder_Start(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Start failed in buffer mode: %d", err);
		return false;
	}

	sys->decoderStarted = true;
	sys->surfaceMode = false;
	sys->width = w;
	sys->height = h;
	sys->outputWidth = w;
	sys->outputHeight = h;
	sys->outputStride = 0;
	sys->codecError = false;
	/* consecutiveErrors intentionally NOT cleared — only real frame production
	 * proves recovery (see surface_on_output / buffer-mode OK report). */
	sys->savedWidth = w;
	sys->savedHeight = h;
	sys->savedSurfaceMode = false;

	WLog_Print(h264->log, WLOG_INFO,
	           "OHOS decoder: buffer mode started %dx%d", w, h);
	return true;
}

/* ======================================================================
 * Surface mode support (WITH_OHOS_HWCODEC_SURFACE)
 * ====================================================================== */

#ifdef WITH_OHOS_HWCODEC_SURFACE

/* Surface mode output callback — render decoded frame to NativeImage surface */
static void surface_on_output(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer,
                               void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys = NULL;
	if (h264)
		sys = (H264_CONTEXT_OHOS*)h264->pSystemData;

	/* Diagnostic: log first 5 frames + every 300th frame */
	if (sys)
	{
		sys->outputFrameCount++;
		if (sys->outputFrameCount <= 5 || sys->outputFrameCount % 300 == 0)
		{
			OH_AVCodecBufferAttr attr;
			memset(&attr, 0, sizeof(attr));
			if (buffer)
				OH_AVBuffer_GetBufferAttr(buffer, &attr);
			WLog_Print(h264->log, WLOG_DEBUG,
			           "OHOS Surface output: frame=%d index=%u flags=0x%x pts=%lld",
			           sys->outputFrameCount, (unsigned)index,
			           attr.flags, (long long)attr.pts);
		}
	}

	OH_AVErrCode err = OH_VideoDecoder_RenderOutputBuffer(codec, index);
	if (err != AV_ERR_OK)
	{
		if (h264)
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface: RenderOutputBuffer failed: %d (index=%u)", err, index);
	}
	else if (sys)
	{
		/* A frame actually reached the surface — the decoder has proven it is
		 * healthy. consecutiveErrors is ONLY cleared on real frame production
		 * (here for surface mode; at the OK report for buffer mode), never on a
		 * merely successful Reset — otherwise the permanent-failure threshold
		 * (-2) would be unreachable when the hardware errors on every frame. */
		sys->consecutiveErrors = 0;
	}
}

/* Surface mode stream changed callback — log actual decoder output format for diagnostics */
static void surface_on_stream_changed(OH_AVCodec* codec, OH_AVFormat* format, void* userData)
{
	H264_CONTEXT* h264 = (H264_CONTEXT*)userData;
	H264_CONTEXT_OHOS* sys;
	int32_t w = 0, h = 0, stride = 0;
	int32_t cropTop = 0, cropBottom = 0, cropLeft = 0, cropRight = 0;
	(void)codec;

	if (!h264)
		return;
	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	if (!sys)
		return;

	OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
	OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
	OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride);
	/* Try to read crop rectangle (key names may vary across HarmonyOS versions) */
	OH_AVFormat_GetIntValue(format, "video_crop_top", &cropTop);
	OH_AVFormat_GetIntValue(format, "video_crop_bottom", &cropBottom);
	OH_AVFormat_GetIntValue(format, "video_crop_left", &cropLeft);
	OH_AVFormat_GetIntValue(format, "video_crop_right", &cropRight);

	WLog_Print(h264->log, WLOG_INFO,
	           "OHOS Surface decoder: format changed — output %dx%d stride=%d crop(T=%d B=%d L=%d R=%d)",
	           w, h, stride, cropTop, cropBottom, cropLeft, cropRight);

	if (w > 0 && h > 0)
	{
		sys->outputWidth = w;
		sys->outputHeight = h;
		/* Notify renderer of actual decoder output buffer size (with macroblock alignment) */
		if (h264->yuvReadyContext)
			surface_decoder_update_output_size(h264->yuvReadyContext, (void*)h264, w, h);
	}
	if (stride > 0)
		sys->outputStride = stride;

	/* Pass precise crop rect to renderer for accurate content boundary clipping */
	if (h264->yuvReadyContext && (cropRight > 0 || cropBottom > 0))
		surface_decoder_update_crop_rect(h264->yuvReadyContext, (void*)h264, cropTop, cropBottom, cropLeft, cropRight);
}

/**
 * Start decoder in Surface mode (zero-copy via NativeImage).
 * Requests NativeWindow from EglRenderer, configures surface output, starts decoder.
 * Returns true on success, false if Surface mode is not available (caller should fall back).
 */
static bool start_surface_mode(H264_CONTEXT* h264, H264_CONTEXT_OHOS* sys, int32_t w, int32_t h)
{
	OH_AVErrCode err;

	WLog_Print(h264->log, WLOG_INFO,
	           "OHOS decoder: attempting Surface mode %dx%d", w, h);

	/* Step 1: Reuse existing NativeWindow if size matches, or create new one */
	sys->surfaceWindow = surface_decoder_reuse_or_create_native_window(
	    h264->yuvReadyContext, (void*)h264, w, h,
	    h264->surfaceOriginX, h264->surfaceOriginY,
	    h264->surfaceWidth, h264->surfaceHeight);
	if (!sys->surfaceWindow)
	{
		WLog_Print(h264->log, WLOG_DEBUG,
		           "OHOS decoder: NativeWindow not available (renderer may not be ready)");
		return false;
	}

	/* Step 2: Register Surface mode callbacks */
	{
		OH_AVCodecCallback cb;
		cb.onError = ohos_on_error;
		cb.onStreamChanged = surface_on_stream_changed;
		cb.onNeedInputBuffer = ohos_on_need_input;
		cb.onNewOutputBuffer = surface_on_output;

		err = OH_VideoDecoder_RegisterCallback(sys->decoder, cb, (void*)h264);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_WARN,
			           "OHOS decoder: RegisterCallback failed for surface: %d", err);
			goto cleanup;
		}
	}

	/* Step 3: Configure + SetSurface + Prepare + Start */
	{
		OH_AVFormat* fmt = OH_AVFormat_Create();
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, w);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, h);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);

		err = OH_VideoDecoder_Configure(sys->decoder, fmt);
		OH_AVFormat_Destroy(fmt);
	}
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder: Configure failed for surface: %d", err);
		goto cleanup;
	}

	err = OH_VideoDecoder_SetSurface(sys->decoder, sys->surfaceWindow);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder: SetSurface failed: %d", err);
		goto cleanup;
	}

	err = OH_VideoDecoder_Prepare(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder: Prepare failed for surface: %d", err);
		goto cleanup;
	}

	err = OH_VideoDecoder_Start(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder: Start failed for surface: %d", err);
		goto cleanup;
	}

	/* Success */
	sys->surfaceMode = true;
	sys->decoderStarted = true;
	sys->codecError = false;
	/* consecutiveErrors intentionally NOT cleared — see surface_on_output */
	sys->width = w;
	sys->height = h;
	sys->outputWidth = w;
	sys->outputHeight = h;
	sys->outputStride = 0;
	sys->savedWidth = w;
	sys->savedHeight = h;
	sys->savedSurfaceMode = true;
	surface_decoder_activate_oes(h264->yuvReadyContext, (void*)h264);

	WLog_Print(h264->log, WLOG_INFO,
	           "Surface mode H.264 decoder activated %dx%d", w, h);
	return true;

cleanup:
	surface_decoder_destroy_native_image(h264->yuvReadyContext, (void*)h264);
	sys->surfaceWindow = NULL;
	return false;
}

#endif /* WITH_OHOS_HWCODEC_SURFACE */

/* ======================================================================
 * H264_CONTEXT_SUBSYSTEM implementation
 * ====================================================================== */

static int ohos_compress(H264_CONTEXT* h264, const BYTE** pSrcYuv, const UINT32* pStride,
                         BYTE** ppDstData, UINT32* pDstSize)
{
	WINPR_ASSERT(h264);
	WLog_Print(h264->log, WLOG_ERROR, "OHOS VideoDecoder does not support encoding");
	return -1;
}

/**
 * Attempt to reset and recover the OHOS decoder after an error.
 * Sequence: Stop → Reset → Re-register callbacks → Configure → (SetSurface) → Prepare → Start.
 * Returns TRUE if the decoder was successfully recovered and is ready for new frames.
 */
static BOOL ohos_reset_decoder(H264_CONTEXT* h264, H264_CONTEXT_OHOS* sys)
{
	OH_AVErrCode err;

	/* decoderStarted is NOT required: after a failed rebuild (e.g. buffer-mode
	 * restart failure during a surface-upgrade attempt) the decoder object still
	 * exists in a stopped state and OH_VideoDecoder_Reset() is valid — refusing
	 * to recover here would turn a transient failure into a guaranteed -2
	 * permanent-failure disconnect. Only the format parameters are mandatory. */
	if (!sys->decoder || sys->savedWidth <= 0 || sys->savedHeight <= 0)
		return FALSE;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	/* Half-surface guard: a "surface" rebuild without a live surfaceWindow would
	 * register surface callbacks but skip SetSurface — the decoder then runs in
	 * buffer mode while surface_on_output tries RenderOutputBuffer every frame
	 * (fails), the output queue drains and a fresh stall follows. Rebuild in
	 * plain buffer mode instead and tell the renderer the zero-copy path is gone. */
	if (sys->savedSurfaceMode && !sys->surfaceWindow)
	{
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder: surfaceWindow lost — recovery falls back to buffer mode");
		sys->savedSurfaceMode = false;
		sys->surfaceMode = false;
		if (h264->yuvReadyContext)
			surface_decoder_deactivate_oes(h264->yuvReadyContext, (void*)h264);
	}
#endif

	WLog_Print(h264->log, WLOG_WARN,
	           "OHOS decoder: attempting reset recovery (%dx%d, surface=%d)",
	           sys->savedWidth, sys->savedHeight, (int)sys->savedSurfaceMode);

	/* Step 1: Stop and Reset to Initialized state */
	OH_VideoDecoder_Stop(sys->decoder);
	err = OH_VideoDecoder_Reset(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Reset failed during recovery: %d", err);
		return FALSE;
	}

	clear_sync_state(sys);

	/* Step 2: Re-register callbacks */
	{
		OH_AVCodecCallback cb;
		cb.onError = ohos_on_error;
	#ifdef WITH_OHOS_HWCODEC_SURFACE
		if (sys->savedSurfaceMode)
		{
			cb.onStreamChanged = surface_on_stream_changed;
			cb.onNeedInputBuffer = ohos_on_need_input;
			cb.onNewOutputBuffer = surface_on_output;
		}
		else
	#endif
		{
			cb.onStreamChanged = ohos_on_stream_changed;
			cb.onNeedInputBuffer = ohos_on_need_input;
			cb.onNewOutputBuffer = ohos_on_output;
		}

		err = OH_VideoDecoder_RegisterCallback(sys->decoder, cb, (void*)h264);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS decoder: RegisterCallback failed during recovery: %d", err);
			return FALSE;
		}
	}

	/* Step 3: Configure with saved format parameters */
	{
		OH_AVFormat* fmt = OH_AVFormat_Create();
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, sys->savedWidth);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, sys->savedHeight);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);

		err = OH_VideoDecoder_Configure(sys->decoder, fmt);
		OH_AVFormat_Destroy(fmt);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS decoder: Configure failed during recovery: %d", err);
			return FALSE;
		}
	}

	/* Step 4: SetSurface (surface mode only) */
#ifdef WITH_OHOS_HWCODEC_SURFACE
	if (sys->savedSurfaceMode && sys->surfaceWindow)
	{
		err = OH_VideoDecoder_SetSurface(sys->decoder, sys->surfaceWindow);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS decoder: SetSurface failed during recovery: %d", err);
			return FALSE;
		}
	}
#endif

	/* Step 5: Prepare and Start */
	err = OH_VideoDecoder_Prepare(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Prepare failed during recovery: %d", err);
		return FALSE;
	}

	err = OH_VideoDecoder_Start(sys->decoder);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: Start failed during recovery: %d", err);
		return FALSE;
	}

	/* Step 6: Restore decoder state.
	 * consecutiveErrors is deliberately NOT cleared here: a successful Reset does
	 * not prove the decoder can decode — only actually producing a frame does.
	 * Clearing it here would make the permanent-failure threshold unreachable
	 * when the hardware errors on every frame but Reset always succeeds. */
	sys->codecError = false;
	sys->decoderStarted = true;
	sys->width = sys->savedWidth;
	sys->height = sys->savedHeight;
	sys->outputWidth = sys->savedWidth;
	sys->outputHeight = sys->savedHeight;
	sys->outputStride = 0;
	sys->totalDecompressCalls = 0;
	sys->consecutiveTimeouts = 0;
	sys->hasProducedOutput = false;
#ifdef WITH_OHOS_HWCODEC_SURFACE
	sys->hasSpsReceived = false;
	/* Keep runtime mode consistent with the mode we actually rebuilt in */
	sys->surfaceMode = sys->savedSurfaceMode;
#endif

	WLog_Print(h264->log, WLOG_INFO,
	           "OHOS decoder: reset recovery successful %dx%d (surface=%d)",
	           sys->savedWidth, sys->savedHeight, (int)sys->savedSurfaceMode);
	return TRUE;
}

static int ohos_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
	H264_CONTEXT_OHOS* sys;
	BYTE** pYUVData;
	UINT32* iStride;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData);

	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	WINPR_ASSERT(sys);

	/* Frame-stall watchdog: apply any pending recovery request (set from the
	 * performance-monitor thread). Version bump signals a new request; we apply
	 * it once per H264_CONTEXT. The bridge is read-only + version-deduplicated,
	 * so the version/flags persist for the whole session.
	 *
	 * Consumption semantics (recFlags: 0x1 = reset decoder, 0x2 = downgrade —
	 * disable surface zero-copy), four cases (started × flag):
	 *   started  + 0x2, surfaceMode  → flip to buffer mode, deactivate OES,
	 *                                  force rebuild via codecError (self-contained:
	 *                                  does NOT rely on 0x1 accompanying 0x2 —
	 *                                  without a rebuild the decoder would keep
	 *                                  surface callbacks while decompress takes
	 *                                  the buffer path → 1s timeout per frame)
	 *   started  + 0x2, !surfaceMode → already buffer mode; just forbid re-upgrade
	 *   started  + 0x1               → codecError = true → reset recovery path below
	 *   !started + 0x2               → only forbid the surface path for the upcoming
	 *                                  start (decoder not built yet — nothing to
	 *                                  rebuild, must NOT set codecError)
	 *   !started + 0x1               → ignore: a decoder that never started needs no
	 *                                  reset; it will start fresh anyway. Applying it
	 *                                  would send a brand-new context into the error
	 *                                  path and eventually a spurious -2 disconnect.
	 *
	 * Seeding: a freshly created H264_CONTEXT (resolution change / ResetGraphics /
	 * monitor hotplug / GFX reconnect) starts with appliedRecoveryVersion == 0.
	 * If a recovery happened earlier in this session, the bridge version is already
	 * non-zero and its stale flags target a decoder that no longer exists — on the
	 * first check we therefore only adopt the current version WITHOUT applying
	 * flags. */
	if (h264->yuvReadyContext)
	{
		int recVer = surface_decoder_get_recovery_version(h264->yuvReadyContext);
		if (!sys->recoveryVersionSeeded)
		{
			sys->recoveryVersionSeeded = true;
			sys->appliedRecoveryVersion = recVer;
		}
		else if (recVer != sys->appliedRecoveryVersion)
		{
			sys->appliedRecoveryVersion = recVer;
			int recFlags = surface_decoder_take_recovery_flags(h264->yuvReadyContext);
			bool started = sys->decoderStarted;
#ifdef WITH_OHOS_HWCODEC_SURFACE
			if (recFlags & 0x2)
			{
				/* Downgrade: forbid surface zero-copy from now on (both the initial
				 * deferred start and the buffer→surface upgrade retry check
				 * surfaceRetryCount). */
				sys->surfaceRetryCount = 30;
				sys->savedSurfaceMode = false;
				if (sys->surfaceMode)
				{
					/* Deactivate OES (clears surfaceMode in gdi_bridge + restores
					 * EndPaint BGRA upload) and force a rebuild with buffer
					 * callbacks via the codecError→reset path. */
					sys->surfaceMode = false;
					sys->surfaceWindow = NULL;
					surface_decoder_deactivate_oes(h264->yuvReadyContext, (void*)h264);
					if (started)
						sys->codecError = true;
					WLog_Print(h264->log, WLOG_WARN,
					           "OHOS decoder: watchdog downgrade to buffer mode (version %d)",
					           recVer);
				}
			}
#endif
			if ((recFlags & 0x1) && started)
				sys->codecError = true; /* trigger existing reset recovery path below */
		}
	}

	if (sys->codecError)
	{
		sys->consecutiveErrors++;

		/* Report error state to the watchdog (event thread) */
		if (h264->yuvReadyContext)
			surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
			                                    1 /* CODEC */, sys->consecutiveErrors,
			                                    sys->consecutiveTimeouts);

		/* Threshold: give up after too many consecutive failures */
		if (sys->consecutiveErrors > 5)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS decoder: %d consecutive errors, permanent failure",
			           sys->consecutiveErrors);
			if (h264->yuvReadyContext)
				surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
				                                    3 /* PERMANENT */, sys->consecutiveErrors,
				                                    sys->consecutiveTimeouts);
			return -2;
		}

		/* Attempt auto-recovery: reset decoder, skip current frame,
		 * next frame will use the fresh decoder. */
		WLog_Print(h264->log, WLOG_WARN,
		           "OHOS decoder error (attempt %d), attempting reset recovery...",
		           sys->consecutiveErrors);

		if (ohos_reset_decoder(h264, sys))
		{
			/* Reset succeeded — skip this frame, next frame will decode normally.
			 * Request IDR refresh (throttled: at most once per 500ms) so the fresh
			 * decoder gets a key frame without flooding the server when the
			 * error→reset cycle repeats. */
			ohos_request_refresh_throttled(h264, sys);
			return -1;
		}

		/* Reset failed — will retry on next call (up to threshold) */
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: reset recovery failed (attempt %d/%d)",
		           sys->consecutiveErrors, 5);
		return -1;
	}

	/* NOTE: consecutiveErrors is NOT cleared just because codecError is currently
	 * false — after a reset the flag is clear but the decoder is unproven. Only
	 * actually producing a frame (surface_on_output / buffer OK report) clears it,
	 * so a persistent per-frame error correctly reaches the -2 threshold. */

	/* Determine resolution for decoder startup/resize */
	int32_t w = ((int32_t)h264->width > 0) ? (int32_t)h264->width : sys->width;
	int32_t h_val = ((int32_t)h264->height > 0) ? (int32_t)h264->height : sys->height;
	if (w < OHOS_MINIMUM_WIDTH) w = OHOS_MINIMUM_WIDTH;
	if (h_val < OHOS_MINIMUM_HEIGHT) h_val = OHOS_MINIMUM_HEIGHT;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	/* Deferred start: on first Decompress, try Surface mode first.
	 * surfaceRetryCount >= 30 means the surface path has been forbidden (watchdog
	 * downgrade flag 0x2 received before start) — start directly in buffer mode. */
	if (!sys->decoderStarted)
	{
		if (h264->yuvReadyContext && sys->surfaceRetryCount < 30 &&
		    start_surface_mode(h264, sys, w, h_val))
		{
			/* Refresh is now coordinated by surface_decoder_activate_oes() in
			 * gdi_bridge.cpp — it waits for ALL renderers to have active OES
			 * surfaces, then issues a single session-level suppress+resume.
			 * This avoids the timing issue where per-surface refresh causes
			 * later surfaces to receive P-frames without SPS/PPS. */
		}
		else
		{
			/* Surface not available, fall back to buffer mode.
			 * activate_oes() 只在 Surface 成功路径中调用，Buffer 路径必须自己
			 * 请求 IDR，否则解码器只会收到 P 帧永远无法产出首帧（黑屏）。
			 * 与下方 #else 分支保持一致。 */
			if (!start_buffer_mode(h264, sys, w, h_val))
				return -1;
			if (h264->yuvReadyContext)
				surface_decoder_request_refresh(h264->yuvReadyContext);
		}
	}

	/* Already in buffer mode but Surface should be available → retry upgrade
	 * (up to 30 attempts, covering the EglRenderer initialization window).
	 * Skip retries entirely if Surface mode is permanently unavailable (e.g. DGLES OES bug). */
	if (sys->decoderStarted && !sys->surfaceMode
	    && h264->yuvReadyContext && sys->surfaceRetryCount < 30
	    && !surface_decoder_is_permanently_unavailable(h264->yuvReadyContext, (void*)h264))
	{
		sys->surfaceRetryCount++;

		/* Stop + Reset buffer decoder to re-enter Initialized state */
		OH_VideoDecoder_Stop(sys->decoder);
		OH_AVErrCode resetErr = OH_VideoDecoder_Reset(sys->decoder);
		if (resetErr == AV_ERR_OK)
		{
			clear_sync_state(sys);
			sys->decoderStarted = false;
			sys->hasSpsReceived = false;

			if (start_surface_mode(h264, sys, w, h_val))
			{
				/* DPB was cleared by Reset() above — refresh is coordinated by
				 * surface_decoder_activate_oes() in gdi_bridge.cpp. */
				WLog_Print(h264->log, WLOG_INFO,
				    "OHOS: Surface mode activated after %d retries",
				    sys->surfaceRetryCount);
				goto surface_path;
			}
			/* Surface upgrade failed, restart buffer mode */
			if (!start_buffer_mode(h264, sys, w, h_val))
			{
				sys->codecError = true;
				return -1;
			}
			/* Reset() 已清空 DPB，必须请求 IDR，否则 P 帧无法重建非脏区域。 */
			if (h264->yuvReadyContext)
				surface_decoder_request_refresh(h264->yuvReadyContext);
		}
	}

	if (sys->surfaceMode)
	{
surface_path:
		/* Surface mode: resolution change handling */
		if (sys->width != (int32_t)h264->width || sys->height != (int32_t)h264->height)
		{
			int32_t newW = (int32_t)h264->width;
			int32_t newH = (int32_t)h264->height;

			WLog_Print(h264->log, WLOG_INFO,
			           "OHOS Surface decoder: resolution change %dx%d -> %dx%d",
			           sys->width, sys->height, newW, newH);

			OH_VideoDecoder_Stop(sys->decoder);
			OH_AVErrCode serr = OH_VideoDecoder_Reset(sys->decoder);
			if (serr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: Reset failed on resize: %d", serr);
				sys->codecError = true;
				return -1;
			}

			clear_sync_state(sys);

			/* Re-request NativeImage for new resolution */
			sys->surfaceWindow = surface_decoder_request_native_window(
			    h264->yuvReadyContext, (void*)h264, newW, newH,
			    h264->surfaceOriginX, h264->surfaceOriginY,
			    h264->surfaceWidth, h264->surfaceHeight);
			if (!sys->surfaceWindow)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: NativeWindow failed on resize");
				sys->codecError = true;
				return -1;
			}

			/* Re-register callbacks after Reset */
			{
				OH_AVCodecCallback cb;
				cb.onError = ohos_on_error;
				cb.onStreamChanged = surface_on_stream_changed;
				cb.onNeedInputBuffer = ohos_on_need_input;
				cb.onNewOutputBuffer = surface_on_output;

				serr = OH_VideoDecoder_RegisterCallback(sys->decoder, cb, (void*)h264);
				if (serr != AV_ERR_OK)
				{
					WLog_Print(h264->log, WLOG_ERROR,
					           "OHOS Surface decoder: RegisterCallback failed on resize: %d", serr);
					sys->codecError = true;
					return -1;
				}
			}

			OH_AVFormat* fmt = OH_AVFormat_Create();
			OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, newW);
			OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, newH);
			OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);

			serr = OH_VideoDecoder_Configure(sys->decoder, fmt);
			OH_AVFormat_Destroy(fmt);
			if (serr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: Configure failed on resize: %d", serr);
				sys->codecError = true;
				return -1;
			}

			/* SetSurface — must be called in Configured state (after Configure, before Prepare) */
			serr = OH_VideoDecoder_SetSurface(sys->decoder, sys->surfaceWindow);
			if (serr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: SetSurface failed on resize: %d", serr);
				sys->codecError = true;
				return -1;
			}

			serr = OH_VideoDecoder_Prepare(sys->decoder);
			if (serr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: Prepare failed on resize: %d", serr);
				sys->codecError = true;
				return -1;
			}

			serr = OH_VideoDecoder_Start(sys->decoder);
			if (serr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_ERROR,
				           "OHOS Surface decoder: Start failed on resize: %d", serr);
				sys->codecError = true;
				return -1;
			}

			sys->width = newW;
			sys->height = newH;
			sys->outputWidth = newW;
			sys->outputHeight = newH;
			sys->outputStride = 0;
			sys->codecError = false;
			/* Keep saved recovery parameters in sync so a later reset rebuilds
			 * at the CURRENT resolution, not the pre-resize one. */
			sys->savedWidth = newW;
			sys->savedHeight = newH;
			sys->savedSurfaceMode = true;

			/* Re-activate OES mode for new NativeImage */
			surface_decoder_activate_oes(h264->yuvReadyContext, (void*)h264);
			/* Force IDR after resize (same DPB-empty issue as initial upgrade) */
			surface_decoder_request_refresh(h264->yuvReadyContext);
			sys->hasSpsReceived = false;
			WLog_Print(h264->log, WLOG_INFO,
			           "OHOS Surface decoder: resize complete %dx%d (refresh requested)", newW, newH);
		}

		/* Safety net: freshly started decoder MUST receive SPS/PPS before any slice.
		 * If the current bitstream lacks SPS, skip it — the coordinated refresh
		 * (from surface_decoder_activate_oes) will deliver an IDR with SPS/PPS.
		 * This fixes multi-monitor black screen when a surface's first frame
		 * arrives without parameter sets due to a prior session-level refresh. */
		if (!sys->hasSpsReceived)
		{
			if (has_sps_nalu(pSrcData, SrcSize))
			{
				sys->hasSpsReceived = true;
			}
			else
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "OHOS Surface decoder: frame without SPS skipped, waiting for IDR");
				return 0;
			}
		}

		/* Wait for input buffer, push NAL, return 0 (no CPU output) */
		if (!wait_input_ready(sys, 1000))
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface decoder: timeout waiting for input");
			if (h264->yuvReadyContext)
				surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
				                                    2 /* TIMEOUT */, sys->consecutiveErrors,
				                                    sys->consecutiveTimeouts);
			return -1;
		}
		if (!sys->inputBuffer)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface decoder: null input buffer");
			return -1;
		}

		uint8_t* sInputAddr = OH_AVBuffer_GetAddr(sys->inputBuffer);
		int32_t sInputCap = OH_AVBuffer_GetCapacity(sys->inputBuffer);
		if (!sInputAddr || (int32_t)SrcSize > sInputCap)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface decoder: input buffer too small (need %u, have %d)",
			           SrcSize, sInputCap);
			return -1;
		}

		memcpy(sInputAddr, pSrcData, SrcSize);

		OH_AVCodecBufferAttr sAttr;
		memset(&sAttr, 0, sizeof(sAttr));
		sAttr.size = (int32_t)SrcSize;
		OH_AVBuffer_SetBufferAttr(sys->inputBuffer, &sAttr);

		OH_AVErrCode sErr = OH_VideoDecoder_PushInputBuffer(sys->decoder, sys->inputIndex);
		if (sErr != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface decoder: PushInputBuffer failed: %d", sErr);
			return -1;
		}

		/* Return 0: no CPU-side YUV output (zero-copy path) */
		return 0;
	}
#else
	/* No Surface support: deferred buffer mode start */
	if (!sys->decoderStarted)
	{
		if (!start_buffer_mode(h264, sys, w, h_val))
			return -1;
		/* Request IDR key frame — without this the decoder only receives
		 * P-frames and cannot produce output, causing prolonged black screen. */
		if (h264->yuvReadyContext)
			surface_decoder_request_refresh(h264->yuvReadyContext);
	}
#endif /* WITH_OHOS_HWCODEC_SURFACE */

	/* Buffer mode path */
	pYUVData = h264->pYUVData;
	WINPR_ASSERT(pYUVData);

	iStride = h264->iStride;
	WINPR_ASSERT(iStride);

	/* Handle resolution changes */
	if (sys->width != (int32_t)h264->width || sys->height != (int32_t)h264->height)
	{
		int32_t newW = (int32_t)h264->width;
		int32_t newH = (int32_t)h264->height;

		if (newW < OHOS_MINIMUM_WIDTH || newH < OHOS_MINIMUM_HEIGHT)
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS decoder: resolution %dx%d below minimum %dx%d",
			           newW, newH, OHOS_MINIMUM_WIDTH, OHOS_MINIMUM_HEIGHT);
			return -1;
		}

		WLog_Print(h264->log, WLOG_INFO, "OHOS decoder: resolution change %dx%d -> %dx%d",
		           sys->width, sys->height, newW, newH);

		/* Reconfigure: Reset (→Initialized) → Configure → Prepare → Start.
		 * Stop() only goes to Configured state where Configure() is invalid.
		 * Reset() returns to Initialized state where the full init sequence is valid. */
		OH_VideoDecoder_Stop(sys->decoder);
		OH_AVErrCode err = OH_VideoDecoder_Reset(sys->decoder);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: Reset failed on resize: %d", err);
			return -1;
		}

		clear_sync_state(sys);

		OH_AVFormat* fmt = OH_AVFormat_Create();
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, newW);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, newH);
		OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);

		err = OH_VideoDecoder_Configure(sys->decoder, fmt);
		OH_AVFormat_Destroy(fmt);

		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: Configure failed on resize: %d", err);
			return -1;
		}

		err = OH_VideoDecoder_Prepare(sys->decoder);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: Prepare failed on resize: %d", err);
			return -1;
		}

		err = OH_VideoDecoder_Start(sys->decoder);
		if (err != AV_ERR_OK)
		{
			WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: Start failed on resize: %d", err);
			return -1;
		}

		sys->width = newW;
		sys->height = newH;
		sys->outputWidth = newW;
		sys->outputHeight = newH;
		sys->outputStride = 0;
		sys->codecError = false;
		/* Keep saved recovery parameters in sync (see surface resize path) */
		sys->savedWidth = newW;
		sys->savedHeight = newH;
		sys->savedSurfaceMode = false;
		sys->totalDecompressCalls = 0;
		sys->consecutiveTimeouts = 0;
		sys->hasProducedOutput = false;
		WLog_Print(h264->log, WLOG_INFO, "OHOS decoder: resize complete %dx%d", newW, newH);
		/* DPB cleared by Reset — request IDR to avoid P-frame-only timeout */
		if (h264->yuvReadyContext)
			surface_decoder_request_refresh(h264->yuvReadyContext);
	}

	/* Wait for an input buffer from the async callback */
	if (!wait_input_ready(sys, 1000))
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: timeout waiting for input buffer");
		if (h264->yuvReadyContext)
			surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
			                                    2 /* TIMEOUT */, sys->consecutiveErrors,
			                                    sys->consecutiveTimeouts);
		return -1;
	}

	if (!sys->inputBuffer)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: null input buffer");
		return -1;
	}

	/* Copy H.264 NAL data into the input buffer */
	uint8_t* inputAddr = OH_AVBuffer_GetAddr(sys->inputBuffer);
	int32_t inputCapacity = OH_AVBuffer_GetCapacity(sys->inputBuffer);

	if (!inputAddr || (int32_t)SrcSize > inputCapacity)
	{
		WLog_Print(h264->log, WLOG_ERROR,
		           "OHOS decoder: input buffer too small (need %u, have %d)", SrcSize,
		           inputCapacity);
		return -1;
	}

	memcpy(inputAddr, pSrcData, SrcSize);

	OH_AVCodecBufferAttr attr;
	memset(&attr, 0, sizeof(attr));
	attr.size = (int32_t)SrcSize;
	attr.offset = 0;
	attr.pts = 0;
	attr.flags = 0;
	OH_AVBuffer_SetBufferAttr(sys->inputBuffer, &attr);

	OH_AVErrCode err = OH_VideoDecoder_PushInputBuffer(sys->decoder, sys->inputIndex);
	if (err != AV_ERR_OK)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: PushInputBuffer failed: %d", err);
		return -1;
	}

	/* Wait for decoded output.
	 *
	 * CRITICAL: ohos_decompress runs on the RDP event loop thread. While we block
	 * here, no new GFX frames (including IDR key frames) can be processed. The
	 * decoder often needs an IDR to produce first output, but the IDR arrives as
	 * a subsequent GFX surface command that can only be processed when this call
	 * returns. A long timeout here creates a deadlock: we wait for output that
	 * requires data that can't arrive until we stop waiting.
	 *
	 * Strategy: Before the decoder has produced any output, use a very short
	 * timeout (just a quick poll) so the event loop keeps running. Each call feeds
	 * one frame to the decoder and quickly returns, allowing the next frame
	 * (possibly the IDR) to be processed. Once the decoder has proven it can
	 * produce output, use normal timeouts. */
	{
		int timeoutMs;
		sys->totalDecompressCalls++;

		if (!sys->hasProducedOutput)
			timeoutMs = 100;   /* Pre-first-output: quick poll, don't block event loop.
			                    * The IDR key frame needed by the decoder arrives as a
			                    * subsequent GFX command on this same thread. */
		else if (sys->consecutiveTimeouts >= 5)
			timeoutMs = 200;   /* After many timeouts: short poll to avoid event loop stall */
		else
			timeoutMs = 2000;  /* Normal: 2s */

		if (!wait_output_ready(sys, timeoutMs))
		{
			sys->consecutiveTimeouts++;
			WLog_Print(h264->log, WLOG_WARN,
			           "OHOS decoder: timeout waiting for output (attempt %d, timeout %dms)",
			           sys->consecutiveTimeouts, timeoutMs);
			if (h264->yuvReadyContext)
				surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
				                                    2 /* TIMEOUT */, sys->consecutiveErrors,
				                                    sys->consecutiveTimeouts);
			return -1;
		}
		sys->consecutiveTimeouts = 0;
		if (!sys->hasProducedOutput)
		{
			sys->hasProducedOutput = true;
			WLog_Print(h264->log, WLOG_INFO,
			           "OHOS decoder: first output after %d calls", sys->totalDecompressCalls);
		}
	}

	if (!sys->outputBuffer)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: null output buffer");
		return -1;
	}

	uint8_t* outputAddr = OH_AVBuffer_GetAddr(sys->outputBuffer);
	if (!outputAddr)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: null output address");
		OH_VideoDecoder_FreeOutputBuffer(sys->decoder, sys->outputIndex);
		return -1;
	}

	int32_t outW = sys->outputWidth;
	int32_t outH = sys->outputHeight;
	int32_t yStride = (sys->outputStride > 0) ? sys->outputStride : outW;
	int32_t chromaW = (outW + 1) / 2;
	int32_t chromaH = (outH + 1) / 2;
	int32_t uvStride = yStride;

	/*
	 * Output is NV12: Y plane followed by interleaved UV plane.
	 * FreeRDP expects I420: separate Y, U, V planes.
	 * Y plane can be used directly (pointer into codec buffer).
	 * U and V need to be deinterleaved from NV12 UV plane.
	 */
	size_t chromaPlaneSize = (size_t)chromaW * chromaH;
	size_t neededSize = chromaPlaneSize * 2;
	if (sys->chromaConvBufSize < neededSize)
	{
		free(sys->chromaConvBuf);
		sys->chromaConvBuf = (uint8_t*)malloc(neededSize);
		sys->chromaConvBufSize = neededSize;
		if (!sys->chromaConvBuf)
		{
			WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: chroma buffer alloc failed");
			OH_VideoDecoder_FreeOutputBuffer(sys->decoder, sys->outputIndex);
			return -1;
		}
	}

	uint8_t* nv12Uv = outputAddr + yStride * outH;
	uint8_t* uPlane = sys->chromaConvBuf;
	uint8_t* vPlane = sys->chromaConvBuf + chromaPlaneSize;

	nv12_to_i420_chroma(nv12Uv, uPlane, vPlane, chromaW, chromaH, uvStride);

	iStride[0] = (UINT32)yStride;
	iStride[1] = (UINT32)chromaW;
	iStride[2] = (UINT32)chromaW;
	pYUVData[0] = outputAddr;
	pYUVData[1] = uPlane;
	pYUVData[2] = vPlane;

	/* Release the output buffer back to the codec after the caller has consumed YUV data.
	 * Note: FreeRDP copies/converts the YUV data before the next Decompress call,
	 * so we release here. The Y pointer becomes invalid but has already been consumed. */
	OH_VideoDecoder_FreeOutputBuffer(sys->decoder, sys->outputIndex);
	sys->outputIndex = -1;
	sys->outputBuffer = NULL;

	/* Buffer-mode frame produced successfully — the decoder has proven it is
	 * healthy: clear the consecutive-error counter (BUG-3 semantics: only real
	 * frame production clears it) and signal the watchdog to reset its stall
	 * state machine. (Surface mode clears the counter in surface_on_output.) */
	sys->consecutiveErrors = 0;
	if (h264->yuvReadyContext)
		surface_decoder_report_decode_error(h264->yuvReadyContext, (void*)h264,
		                                    4 /* OK */, 0, 0);

	return 1;
}

static void ohos_uninit(H264_CONTEXT* h264)
{
	H264_CONTEXT_OHOS* sys;

	WINPR_ASSERT(h264);
	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;

	WLog_Print(h264->log, WLOG_DEBUG, "OHOS VideoDecoder: uninitializing");

	if (!sys)
		return;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	if (sys->surfaceMode && h264->yuvReadyContext)
	{
		/* Don't destroy NativeImage — it is managed by EglRenderer and can be reused
		 * by the next decoder instance (e.g., after GFX DVC reconnect).
		 * Only deactivate OES mode so BGRA fallback can work if needed. */
		surface_decoder_deactivate_oes(h264->yuvReadyContext, (void*)h264);
		sys->surfaceWindow = NULL;
		sys->surfaceMode = false;
	}
#endif

	if (sys->decoder)
	{
		if (sys->decoderStarted)
		{
			/* Flush 排空所有挂起的输入/输出回调，确保 NativeWindow 的
			 * BufferQueue 不残留 decoder 持有的 buffer。
			 * 不 Flush 直接 Stop 会导致 buffer 泄漏——下次创建新 decoder
			 * 绑定同一 NativeWindow 时 BufferQueue slot 不足，input 回调
			 * 永远不触发，表现为黑屏。 */
			OH_AVErrCode flushErr = OH_VideoDecoder_Flush(sys->decoder);
			if (flushErr != AV_ERR_OK)
			{
				WLog_Print(h264->log, WLOG_WARN,
				           "OHOS decoder: Flush before Stop failed: %d (continuing)", flushErr);
			}
			OH_VideoDecoder_Stop(sys->decoder);
		}
		OH_VideoDecoder_Destroy(sys->decoder);
		sys->decoder = NULL;
	}

	free(sys->chromaConvBuf);
	sys->chromaConvBuf = NULL;

	pthread_mutex_destroy(&sys->inputMutex);
	pthread_cond_destroy(&sys->inputCond);
	pthread_mutex_destroy(&sys->outputMutex);
	pthread_cond_destroy(&sys->outputCond);

	free(sys);
	h264->pSystemData = NULL;
}

static BOOL ohos_init(H264_CONTEXT* h264)
{
	H264_CONTEXT_OHOS* sys;

	WINPR_ASSERT(h264);

	if (h264->Compressor)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS VideoDecoder does not support encoding");
		goto EXCEPTION;
	}

	WLog_Print(h264->log, WLOG_DEBUG, "Initializing OHOS hardware H.264 decoder (deferred start)");

	sys = (H264_CONTEXT_OHOS*)calloc(1, sizeof(H264_CONTEXT_OHOS));
	if (!sys)
		goto EXCEPTION;

	h264->pSystemData = (void*)sys;

	pthread_mutex_init(&sys->inputMutex, NULL);
	pthread_cond_init(&sys->inputCond, NULL);
	pthread_mutex_init(&sys->outputMutex, NULL);
	pthread_cond_init(&sys->outputCond, NULL);

	sys->inputIndex = -1;
	sys->outputIndex = -1;
	sys->width = OHOS_MINIMUM_WIDTH;
	sys->height = OHOS_MINIMUM_HEIGHT;
	sys->outputWidth = sys->width;
	sys->outputHeight = sys->height;
	sys->savedWidth = 0;
	sys->savedHeight = 0;
	sys->savedSurfaceMode = false;
	sys->consecutiveErrors = 0;

	/* Watchdog recovery-version seeding is done lazily on the first decompress
	 * (h264->yuvReadyContext is not yet attached at init time): the first check
	 * only adopts the bridge's current version without applying stale flags. */
	sys->appliedRecoveryVersion = 0;
	sys->recoveryVersionSeeded = false;
	sys->lastRefreshRequestMs = 0;

	sys->decoderStarted = false;
#ifdef WITH_OHOS_HWCODEC_SURFACE
	sys->surfaceMode = false;
	sys->surfaceWindow = NULL;
	sys->surfaceRetryCount = 0;
#endif

	/* Only create the decoder object — actual Configure/Prepare/Start is deferred
	 * to the first ohos_decompress() call, where we know the real resolution and
	 * can attempt Surface mode (zero-copy) directly. */
	sys->decoder = OH_VideoDecoder_CreateByMime(OHOS_CODEC_MIME);
	if (!sys->decoder)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OH_VideoDecoder_CreateByMime failed");
		goto EXCEPTION;
	}

	WLog_Print(h264->log, WLOG_INFO,
	           "OHOS hardware H.264 decoder created (start deferred to first Decompress)");
	return TRUE;

EXCEPTION:
	ohos_uninit(h264);
	return FALSE;
}

const H264_CONTEXT_SUBSYSTEM g_Subsystem_ohos = { "OHOS_HWCodec", ohos_init, ohos_uninit,
	                                              ohos_decompress, ohos_compress };
