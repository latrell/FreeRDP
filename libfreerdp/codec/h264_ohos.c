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
#include <pthread.h>

#include "h264.h"

#define OHOS_CODEC_MIME OH_AVCODEC_MIMETYPE_VIDEO_AVC

static const int OHOS_MINIMUM_WIDTH = 320;
static const int OHOS_MINIMUM_HEIGHT = 240;

#ifdef WITH_OHOS_HWCODEC_SURFACE
/* External bridge functions — defined in libentry (gdi_bridge.cpp).
 * Resolved at final link time when FreeRDP static lib is linked into libentry.so. */
extern OHNativeWindow* surface_decoder_request_native_window(void* session, int width, int height);
extern OHNativeWindow* surface_decoder_reuse_or_create_native_window(void* session, int width, int height);
extern void surface_decoder_notify_frame(void* session);
extern void surface_decoder_destroy_native_image(void* session);
extern void surface_decoder_deactivate_oes(void* session);
extern void surface_decoder_activate_oes(void* session);
extern void surface_decoder_request_refresh(void* session);
extern void surface_decoder_update_output_size(void* session, int width, int height);
extern void surface_decoder_update_crop_rect(void* session, int top, int bottom, int left, int right);
extern bool surface_decoder_is_permanently_unavailable(void* session);
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

	bool codecError;

	/* NV12→I420 conversion buffer */
	uint8_t* chromaConvBuf;
	size_t chromaConvBufSize;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	bool decoderStarted;     /* Decoder configured and started (deferred from init) */
	bool surfaceMode;        /* Currently using Surface mode */
	OHNativeWindow* surfaceWindow;  /* NativeImage-provided output window */
	int surfaceRetryCount;   /* Surface upgrade retry count */
	int outputFrameCount;    /* Total output frames (diagnostic) */
#else
	bool decoderStarted;     /* Decoder configured and started (deferred from init) */
#endif
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
			surface_decoder_update_output_size(h264->yuvReadyContext, w, h);
	}
	if (stride > 0)
		sys->outputStride = stride;

	/* Pass precise crop rect to renderer for accurate content boundary clipping */
	if (h264->yuvReadyContext && (cropRight > 0 || cropBottom > 0))
		surface_decoder_update_crop_rect(h264->yuvReadyContext, cropTop, cropBottom, cropLeft, cropRight);
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
	    h264->yuvReadyContext, w, h);
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
	sys->width = w;
	sys->height = h;
	sys->outputWidth = w;
	sys->outputHeight = h;
	sys->outputStride = 0;
	surface_decoder_activate_oes(h264->yuvReadyContext);

	WLog_Print(h264->log, WLOG_INFO,
	           "Surface mode H.264 decoder activated %dx%d", w, h);
	return true;

cleanup:
	surface_decoder_destroy_native_image(h264->yuvReadyContext);
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

static int ohos_decompress(H264_CONTEXT* h264, const BYTE* pSrcData, UINT32 SrcSize)
{
	H264_CONTEXT_OHOS* sys;
	BYTE** pYUVData;
	UINT32* iStride;

	WINPR_ASSERT(h264);
	WINPR_ASSERT(pSrcData);

	sys = (H264_CONTEXT_OHOS*)h264->pSystemData;
	WINPR_ASSERT(sys);

	if (sys->codecError)
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder in error state");
		return -1;
	}

	/* Determine resolution for decoder startup/resize */
	int32_t w = ((int32_t)h264->width > 0) ? (int32_t)h264->width : sys->width;
	int32_t h_val = ((int32_t)h264->height > 0) ? (int32_t)h264->height : sys->height;
	if (w < OHOS_MINIMUM_WIDTH) w = OHOS_MINIMUM_WIDTH;
	if (h_val < OHOS_MINIMUM_HEIGHT) h_val = OHOS_MINIMUM_HEIGHT;

#ifdef WITH_OHOS_HWCODEC_SURFACE
	/* Deferred start: on first Decompress, try Surface mode first */
	if (!sys->decoderStarted)
	{
		if (h264->yuvReadyContext && start_surface_mode(h264, sys, w, h_val))
		{
			/* Request IDR in case GFX pipeline is mid-stream (not at first key frame) */
			surface_decoder_request_refresh(h264->yuvReadyContext);
		}
		else
		{
			/* Surface not available, fall back to buffer mode */
			if (!start_buffer_mode(h264, sys, w, h_val))
				return -1;
		}
	}

	/* Already in buffer mode but Surface should be available → retry upgrade
	 * (up to 30 attempts, covering the EglRenderer initialization window).
	 * Skip retries entirely if Surface mode is permanently unavailable (e.g. DGLES OES bug). */
	if (sys->decoderStarted && !sys->surfaceMode
	    && h264->yuvReadyContext && sys->surfaceRetryCount < 30
	    && !surface_decoder_is_permanently_unavailable(h264->yuvReadyContext))
	{
		sys->surfaceRetryCount++;

		/* Stop + Reset buffer decoder to re-enter Initialized state */
		OH_VideoDecoder_Stop(sys->decoder);
		OH_AVErrCode resetErr = OH_VideoDecoder_Reset(sys->decoder);
		if (resetErr == AV_ERR_OK)
		{
			clear_sync_state(sys);
			sys->decoderStarted = false;

			if (start_surface_mode(h264, sys, w, h_val))
			{
				/* DPB was cleared by Reset() above — force server to send IDR key frame,
				 * otherwise P-frames can't reconstruct non-dirty regions (green NV12). */
				surface_decoder_request_refresh(h264->yuvReadyContext);
				WLog_Print(h264->log, WLOG_INFO,
				    "OHOS: Surface mode activated after %d retries (refresh requested)",
				    sys->surfaceRetryCount);
				goto surface_path;
			}
			/* Surface upgrade failed, restart buffer mode */
			if (!start_buffer_mode(h264, sys, w, h_val))
			{
				sys->codecError = true;
				return -1;
			}
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
			    h264->yuvReadyContext, newW, newH);
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

			/* Re-activate OES mode for new NativeImage */
			surface_decoder_activate_oes(h264->yuvReadyContext);
			/* Force IDR after resize (same DPB-empty issue as initial upgrade) */
			surface_decoder_request_refresh(h264->yuvReadyContext);
			WLog_Print(h264->log, WLOG_INFO,
			           "OHOS Surface decoder: resize complete %dx%d (refresh requested)", newW, newH);
		}

		/* Wait for input buffer, push NAL, return 0 (no CPU output) */
		if (!wait_input_ready(sys, 1000))
		{
			WLog_Print(h264->log, WLOG_ERROR,
			           "OHOS Surface decoder: timeout waiting for input");
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
		WLog_Print(h264->log, WLOG_INFO, "OHOS decoder: resize complete %dx%d", newW, newH);
	}

	/* Wait for an input buffer from the async callback */
	if (!wait_input_ready(sys, 1000))
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: timeout waiting for input buffer");
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

	/* Wait for decoded output */
	if (!wait_output_ready(sys, 2000))
	{
		WLog_Print(h264->log, WLOG_ERROR, "OHOS decoder: timeout waiting for output");
		return -1;
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
		surface_decoder_deactivate_oes(h264->yuvReadyContext);
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
