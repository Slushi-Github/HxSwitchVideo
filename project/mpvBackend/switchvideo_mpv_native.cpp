/*
 * switchvideo_mpv_native.cpp - Video and audio backend for HxSwitchVideo
 *
 * MPV render glue (EGL/GLES FBO), and an FFmpeg to OpenAL audio
 * streaming thread.
 *
 * By DgM (@doggywatty on GitHub) and Slushi (@Slushi-Github on GitHub)
 */
#ifdef __SWITCH__
#include "switchvideo_mpv_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <mutex>
#include <deque>
#include <string>
#include <stdarg.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"

// Callback function pointer for external log redirection.
static SwitchVideoLogFn s_logFn = NULL;

static void vidlog(const char *fmt, ...);

/**
 * Sets an external callback function for capturing backend log output.
 *
 * @param fn Function pointer matching the SwitchVideoLogFn signature.
 */
extern "C" void switchvideo_mpv_set_log_callback(SwitchVideoLogFn fn)
{
	s_logFn = fn;
}

// Thread-safe message queue for pending log entries.
static std::mutex s_pendingLogMutex;
static std::deque<std::string> s_pendingLog;

/**
 * Polls and retrieves the oldest pending log message from the internal queue.
 *
 * @param out Pointer to the destination buffer where the log string will be copied.
 * @param out_size Maximum capacity of the destination buffer.
 * @return 1 if a log message was successfully retrieved, or 0 if the queue is empty.
 */
extern "C" int switchvideo_mpv_poll_pending_log(char *out, int out_size)
{
	std::lock_guard<std::mutex> lk(s_pendingLogMutex);
	if (s_pendingLog.empty())
		return 0;
	strncpy(out, s_pendingLog.front().c_str(), out_size - 1);
	out[out_size - 1] = 0;
	s_pendingLog.pop_front();
	return 1;
}

/**
 * Formats a log string and either pushes it to the pending queue (if callback is active)
 * or outputs it directly to the video logging system.
 */
static void vlog(const char *fmt, va_list ap)
{
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, ap);

	if (s_logFn)
	{
		std::lock_guard<std::mutex> lk(s_pendingLogMutex);
		if (s_pendingLog.size() < 1000)
			s_pendingLog.emplace_back(buf);
	}
	else
	{
		vidlog("%s", buf);
	}
}

/**
 * Internal logging wrapper. Operates only when SWITCHVIDEO_LOGGING is defined.
 */
static void vidlog(const char *fmt, ...)
{
#ifdef SWITCHVIDEO_LOGGING
	va_list ap;
	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
#endif
}

/**
 * Snapshot structure to store OpenGL ES state flags, bindings, and configurations.
 * Used to preserve state before MPV rendering and restore it afterward.
 */
struct GLStateSnapshot
{
	GLint program;
	GLboolean blendEnabled;
	GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
	GLint activeTexture;
	GLint boundTexture2D;
	GLint arrayBuffer, elementArrayBuffer;
	GLboolean depthTest;
	GLboolean cullFace;
	GLboolean scissorTest;
	GLint scissorBox[4];
	GLint viewport[4];
	GLint framebuffer;
	GLint vertexAttrib0Enabled;
};

/**
 * Saves current OpenGL state variables into the provided snapshot structure.
 */
static void switchvideo_mpv_gl_save_state(GLStateSnapshot *s)
{
	glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
	s->blendEnabled = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &s->blendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &s->blendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blendDstAlpha);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s->activeTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->boundTexture2D);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->arrayBuffer);
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->elementArrayBuffer);
	s->depthTest = glIsEnabled(GL_DEPTH_TEST);
	s->cullFace = glIsEnabled(GL_CULL_FACE);
	s->scissorTest = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, s->scissorBox);
	glGetIntegerv(GL_VIEWPORT, s->viewport);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->framebuffer);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->vertexAttrib0Enabled);
}

/**
 * Restores OpenGL state variables from a previously saved snapshot.
 */
static void switchvideo_mpv_gl_restore_state(const GLStateSnapshot *s)
{
	glUseProgram(s->program);
	if (s->blendEnabled)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	glBlendFuncSeparate(s->blendSrcRGB, s->blendDstRGB, s->blendSrcAlpha, s->blendDstAlpha);
	glActiveTexture(s->activeTexture);
	glBindTexture(GL_TEXTURE_2D, s->boundTexture2D);
	glBindBuffer(GL_ARRAY_BUFFER, s->arrayBuffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->elementArrayBuffer);
	if (s->depthTest)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	if (s->cullFace)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
	if (s->scissorTest)
		glEnable(GL_SCISSOR_TEST);
	else
		glDisable(GL_SCISSOR_TEST);
	glScissor(s->scissorBox[0], s->scissorBox[1], s->scissorBox[2], s->scissorBox[3]);
	glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	glBindFramebuffer(GL_FRAMEBUFFER, s->framebuffer);
	if (s->vertexAttrib0Enabled)
		glEnableVertexAttribArray(0);
	else
		glDisableVertexAttribArray(0);
}

/**
 * Bridge function to resolve OpenGL function pointers via EGL.
 */
static void *switchvideo_mpv_gl_get_proc_address(void *ctx, const char *name)
{
	return (void *)eglGetProcAddress(name);
}

/**
 * Creates and initializes an MPV OpenGL render context.
 *
 * @param mpv Pointer to the active MPV instance handle.
 * @param error Pointer to store the result code returned by MPV.
 * @return Opaque pointer to the created mpv_render_context.
 */
extern void *switchvideo_mpv_render_context_create(void *mpv, int *error)
{
	mpv_opengl_init_params gl_init = {switchvideo_mpv_gl_get_proc_address, 0};
	mpv_render_param params[] = {
		{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
		{MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
		{0, 0}};
	mpv_render_context *ctx = 0;
	int rc = mpv_render_context_create(&ctx, (mpv_handle *)mpv, params);
	if (error)
		*error = rc;
	return (void *)ctx;
}

/**
 * Triggers an update check on the MPV render context.
 *
 * @param ctx Pointer to the MPV render context.
 * @return Bitmask of flags indicating if rendering updates are required.
 */
extern int switchvideo_mpv_render_context_update(void *ctx)
{
	if (ctx)
		return mpv_render_context_update((mpv_render_context *)ctx);
	return 0;
}

/**
 * Renders the current video frame into the specified Framebuffer Object (FBO).
 * Automatically preserves and restores OpenGL state during execution.
 *
 * @param ctx Pointer to the MPV render context.
 * @param fbo Framebuffer handle target.
 * @param w Width of the target framebuffer.
 * @param h Height of the target framebuffer.
 */
extern void switchvideo_mpv_render_frame(void *ctx, int fbo, int w, int h)
{
	GLStateSnapshot snap;
	switchvideo_mpv_gl_save_state(&snap);

	mpv_opengl_fbo fbo_info = {fbo, w, h, 0};
	int flip = 0;
	mpv_render_param params[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &fbo_info},
		{MPV_RENDER_PARAM_FLIP_Y, &flip},
		{0, 0}};
	mpv_render_context_render((mpv_render_context *)ctx, params);

	switchvideo_mpv_gl_restore_state(&snap);
}

/**
 * Destroys and frees an MPV render context instance.
 */
extern void switchvideo_mpv_render_context_free(void *ctx)
{
	if (ctx)
		mpv_render_context_free((mpv_render_context *)ctx);
}

/**
 * Issues a command to MPV to load a media file from the given path.
 */
extern int switchvideo_mpv_loadfile(void *ctx, const char *path)
{
	const char *args[] = {"loadfile", path, NULL};
	return mpv_command((mpv_handle *)ctx, args);
}

/**
 * Sets a numerical double property in MPV.
 */
extern void switchvideo_mpv_set_double(void *ctx, const char *name, double v)
{
	mpv_set_property((mpv_handle *)ctx, name, MPV_FORMAT_DOUBLE, &v);
}

/**
 * Retrieves a integer property from MPV.
 */
extern int switchvideo_mpv_get_int(void *ctx, const char *name)
{
	int64_t v = 0;
	if (mpv_get_property((mpv_handle *)ctx, name, MPV_FORMAT_INT64, &v) >= 0)
		return (int)v;
	return 0;
}

/**
 * Retrieves a double-precision floating-point property from MPV.
 */
extern double switchvideo_mpv_get_double(void *ctx, const char *name)
{
	double v = 0;
	if (mpv_get_property((mpv_handle *)ctx, name, MPV_FORMAT_DOUBLE, &v) >= 0)
		return v;
	return 0.0;
}

/**
 * Retrieves a boolean flag property from MPV.
 */
extern int switchvideo_mpv_get_boolean(void *ctx, const char *name)
{
	int v = 0;
	if (mpv_get_property((mpv_handle *)ctx, name, MPV_FORMAT_FLAG, &v) >= 0)
		return v;
	return 0;
}

/**
 * Extracts the end-file reason code from an MPV event.
 */
extern int switchvideo_mpv_end_file_reason(const void *event_data)
{
	if (!event_data)
		return -1;
	return ((const mpv_event_end_file *)event_data)->reason;
}

/**
 * Returns the event ID from an MPV event pointer.
 */
extern int switchvideo_mpv_event_id(const void *evt)
{
	if (!evt)
		return 0;
	return ((const mpv_event *)evt)->event_id;
}

/**
 * Extracts raw event data payload from an MPV event structure.
 */
extern const void *switchvideo_mpv_event_data(const void *evt)
{
	if (!evt)
		return 0;
	return ((const mpv_event *)evt)->data;
}

/**
 * Extracts the prefix string from an MPV log message event data block.
 */
extern const char *switchvideo_mpv_log_message_prefix(const void *data)
{
	if (!data)
		return 0;
	return ((const mpv_event_log_message *)data)->prefix;
}

/**
 * Extracts the log text from an MPV log message event data block.
 */
extern const char *switchvideo_mpv_log_message_text(const void *data)
{
	if (!data)
		return 0;
	return ((const mpv_event_log_message *)data)->text;
}

/**
 * Creates an OpenGL Framebuffer Object (FBO) along with an associated 2D texture attachment.
 *
 * @param fbo Pointer to store the generated Framebuffer handle.
 * @param tex Pointer to store the generated Texture handle.
 * @param w Width of the target texture/framebuffer.
 * @param h Height of the target texture/framebuffer.
 */
extern void switchvideo_mpv_gl_create_fbo(int *fbo, int *tex, int w, int h)
{
	GLuint t = 0, f = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	*fbo = (int)f;
	*tex = (int)t;
}

/**
 * Reads raw RGBA pixel data from the specified FBO back into memory.
 */
extern void switchvideo_mpv_gl_read_pixels(int fbo, int w, int h, unsigned char *out)
{
	GLint prevFbo = 0;
	GLint prevViewport[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
	glGetIntegerv(GL_VIEWPORT, prevViewport);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
	glViewport(0, 0, w, h);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

/**
 * Deletes an FBO and its attached texture resources.
 */
extern void switchvideo_mpv_gl_delete_fbo(int fbo, int tex)
{
	GLuint f = (GLuint)fbo, t = (GLuint)tex;
	glDeleteFramebuffers(1, &f);
	glDeleteTextures(1, &t);
}

/*
	Insurance: clear the default framebuffer so no stale pixels (e.g. a ghost video frame) survive into the next frame/state.
	Flixel redraws over it anyway, so this never causes a visible flash.
*/
extern void switchvideo_mpv_gl_clear_default(void)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

/**
 * Swaps Red and Blue channels in-place to convert raw pixel data between RGBA and ARGB component order.
 *
 * @param src Pointer to source RGBA byte stream.
 * @param dst Pointer to destination buffer where converted channel data is written.
 * @param pixels Total number of pixels to convert.
 */
extern void switchvideo_mpv_rgba_to_argb(const unsigned char *src, unsigned char *dst, int pixels)
{
	for (int i = 0; i < pixels; i++)
	{
		dst[i * 4 + 0] = src[i * 4 + 2]; // R <- B (or vice versa)
		dst[i * 4 + 1] = src[i * 4 + 1]; // G stays G
		dst[i * 4 + 2] = src[i * 4 + 0]; // B <- R
		dst[i * 4 + 3] = src[i * 4 + 3]; // A stays A
	}
}

/*
 * Audio: main thread OpenAL, background FFmpeg decode only
 *
 * The background thread ONLY decodes FFmpeg audio into a lock-free ring
 * buffer, it NEVER touches OpenAL.
 *
 * The main thread (switchvideo_mpv_ao_update function) drains the ring buffer and
 * feeds OpenAL, all OpenAL calls happen on the main thread, it's the
 * only thread that has the game's context current.
 *
 * This avoids 3 Switch openal-soft bugs:
 *   1. Creating a second context on the same device corrupts the device.
 *   2. Calling alcMakeContextCurrent(NULL) from a non-main thread corrupts it.
 *   3. Sharing a context between threads causes stuttering.
 */

#include <AL/al.h>
#include <AL/alc.h>

extern "C"
{
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
	#include <libswresample/swresample.h>
	#include <libavutil/channel_layout.h>
}

// ring buffer: 48kHz stereo S16 = 192000 bytes/sec. 2MB ~10s streaming.
#define RING_SIZE 2097152
#define RING_MASK (RING_SIZE - 1)
#define AO_SAMP 4096
#define AO_IOBUF 32768
#define AL_BUF_COUNT 8
#define AL_BUF_SAMPLES 4096

static pthread_t aoThread;
static int aoThreadCreated = 0;
static volatile int aoWantStop = 0;
static volatile int aoReady = 0;
static volatile int aoDone = 0;
static volatile int aoGate = 0;

// ring buffer (decoder writes, main thread reads)
static uint8_t aoRing[RING_SIZE];
static volatile int aoRingHead = 0;
static volatile int aoRingTail = 0;
static volatile int aoRingEof = 0;

// position tracking
static double aoPosCache = 0;
static double aoPlayStartSec = 0;

// AL state (main thread only, on game's context)
static ALuint aoSrc = 0;
static ALuint aoBufs[AL_BUF_COUNT];
static int aoBufHasData[AL_BUF_COUNT];

extern void switchvideo_mpv_audio_stop_func(void);
extern void switchvideo_mpv_audio_start(const char *cpath);

/*
	FFmpeg I/O callbacks
*/
static int switchvideo_mpv_ao_interrupt(void *unused)
{
	return aoWantStop;
}

/*
	Custom AVIO: romfs:/ paths are not real URLs, bypass the URL layer
	and read through fopen() like MPV does.
*/
static int switchvideo_mpv_ao_read(void *opaque, uint8_t *buf, int buf_size)
{
	FILE *f = (FILE *)opaque;
	return (int)fread(buf, 1, buf_size, f);
}

static int64_t switchvideo_mpv_ao_seek(void *opaque, int64_t offset, int whence)
{
	FILE *f = (FILE *)opaque;
	if (whence == AVSEEK_SIZE)
	{
		long cur = ftell(f);
		if (fseek(f, 0, SEEK_END) != 0)
			return -1;
		long size = ftell(f);
		fseek(f, cur, SEEK_SET);
		return size;
	}
	if (fseek(f, (long)offset, whence) != 0)
		return -1;
	return ftell(f);
}

// lock-free SPSC ring buffer
static int ring_write(const uint8_t *data, int len)
{
	int head = aoRingHead;
	int tail = aoRingTail;
	int free = (tail - head - 1) & RING_MASK;
	if (len > free)
		len = free;
	if (len <= 0)
		return 0;
	int avail = RING_SIZE - head;
	if (len > avail)
	{
		memcpy(aoRing + head, data, avail);
		memcpy(aoRing, data + avail, len - avail);
	}
	else
	{
		memcpy(aoRing + head, data, len);
	}
	aoRingHead = (head + len) & RING_MASK;
	return len;
}

static int ring_read(uint8_t *data, int len)
{
	int head = aoRingHead;
	int tail = aoRingTail;
	int avail = (head - tail) & RING_MASK;
	if (len > avail)
		len = avail;
	if (len <= 0)
		return 0;
	int to_end = RING_SIZE - tail;
	if (len > to_end)
	{
		memcpy(data, aoRing + tail, to_end);
		memcpy(data + to_end, aoRing, len - to_end);
	}
	else
	{
		memcpy(data, aoRing + tail, len);
	}
	aoRingTail = (tail + len) & RING_MASK;
	return len;
}

// bg thread: FFmpeg decode ONLY, no OpenAL
static void *switchvideo_mpv_ao_func(void *arg)
{
	char *path = (char *)arg;
	vidlog("[audio] decode thread started, path=%s\n", path);

	AVFormatContext *fmt = NULL;
	AVCodecContext *dec = NULL;
	SwrContext *swr = NULL;
	AVFrame *frm = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	int16_t *cvt = NULL;
	int idx = -1;
	int64_t totalWritten = 0;
	const AVCodec *codec = NULL;
	AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
	FILE *file = NULL;
	AVIOContext *avio = NULL;

	file = fopen(path, "rb");
	if (!file)
	{
		vidlog("[audio] fopen FAILED\n");
		goto done;
	}

	avio = avio_alloc_context((unsigned char *)av_malloc(AO_IOBUF), AO_IOBUF, 0, file, switchvideo_mpv_ao_read, NULL, switchvideo_mpv_ao_seek);
	if (!avio)
	{
		fclose(file);
		file = NULL;
		goto done;
	}

	fmt = avformat_alloc_context();
	if (!fmt)
	{
		avio_context_free(&avio);
		fclose(file);
		file = NULL;
		goto done;
	}
	fmt->pb = avio;
	fmt->interrupt_callback.callback = switchvideo_mpv_ao_interrupt;
	{
		int ret = avformat_open_input(&fmt, NULL, NULL, NULL);
		if (ret < 0)
		{
			if (fmt)
			{
				avformat_free_context(fmt);
				fmt = NULL;
			}
			avio_context_free(&avio);
			fclose(file);
			file = NULL;
			goto done;
		}
	}
	avio = NULL;

	if (avformat_find_stream_info(fmt, NULL) < 0)
		goto done;
	for (unsigned i = 0; i < fmt->nb_streams; i++)
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			idx = (int)i;
			break;
		}
	if (idx < 0)
		goto done;
	for (unsigned i = 0; i < fmt->nb_streams; i++)
		if ((int)i != idx)
			fmt->streams[i]->discard = AVDISCARD_ALL;

	codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
	if (!codec)
		goto done;

	dec = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
	if (avcodec_open2(dec, codec, NULL) < 0)
		goto done;
	vidlog("[audio] decoder=%s %dHz %dch\n", codec->name, dec->sample_rate, dec->ch_layout.nb_channels);

	if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, 48000, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 || !swr || swr_init(swr) < 0)
		goto done;

	cvt = (int16_t *)av_malloc(AO_SAMP * 2 * (int)sizeof(int16_t));

	vidlog("[audio] decoder ready, signaling main thread\n");
	aoReady = 1;

	while (!aoWantStop)
	{
		int got_frame = 0;
		if (avcodec_receive_frame(dec, frm) == 0)
		{
			int n = swr_convert(swr, (uint8_t **)&cvt, AO_SAMP, (const uint8_t **)frm->extended_data, frm->nb_samples);
			if (n > 0)
			{
				int bytes = n * 2 * (int)sizeof(int16_t);
				int written = 0;
				while (written < bytes && !aoWantStop)
				{
					int w = ring_write((uint8_t *)cvt + written, bytes - written);
					if (w > 0)
					{
						written += w;
						totalWritten += w;
					}
					else
					{
						struct timespec ts = {0, 5000000};
						nanosleep(&ts, NULL);
					}
				}
				got_frame = 1;
			}
		}
		if (!got_frame)
		{
			if (av_read_frame(fmt, pkt) < 0)
			{
				avcodec_send_packet(dec, NULL);
				if (avcodec_receive_frame(dec, frm) == 0)
				{
					int n = swr_convert(swr, (uint8_t **)&cvt, AO_SAMP, const uint8_t **)frm->extended_data, frm->nb_samples);
					if (n > 0)
					{
						int bytes = n * 2 * (int)sizeof(int16_t);
						int written = 0;
						while (written < bytes && !aoWantStop)
						{
							int w = ring_write((uint8_t *)cvt + written, bytes - written);
							if (w > 0)
							{
								written += w;
								totalWritten += w;
							}
							else
							{
								struct timespec ts = {0, 5000000};
								nanosleep(&ts, NULL);
							}
						}
					}
				}
				aoRingEof = 1;
				vidlog("[audio] decode EOF total=%lld bytes %.2fs\n", (long long)totalWritten, (double)totalWritten / 192000.0);
				break;
			}
			if (pkt->stream_index == idx)
				avcodec_send_packet(dec, pkt);
			av_packet_unref(pkt);
		}
	}

done:
	if (cvt)
		av_free(cvt);
	if (frm)
		av_frame_free(&frm);
	if (pkt)
		av_packet_free(&pkt);
	if (swr)
		swr_free(&swr);
	if (dec)
		avcodec_free_context(&dec);
	if (fmt)
		avformat_close_input(&fmt);
	if (file)
		fclose(file);
	free(path);
	vidlog("[audio] decode thread exiting\n");
	return NULL;
}

// primer (removed on Switch, game keeps device alive)
static void ao_start_primer(void)
{
}

static void ao_stop_primer(void)
{
}

/*
	Main-thread API
*/
extern void switchvideo_mpv_audio_start(const char *cpath)
{
	vidlog("[audio] switchvideo_mpv_audio_start: %s\n", cpath);
	switchvideo_mpv_audio_stop_func();

	aoWantStop = 0;
	aoReady = 0;
	aoDone = 0;
	aoGate = 0;
	aoRingHead = 0;
	aoRingTail = 0;
	aoRingEof = 0;
	aoPlayStartSec = 0;
	aoPosCache = 0;
	for (int i = 0; i < AL_BUF_COUNT; i++)
		aoBufHasData[i] = 0;

	ao_start_primer();

	char *path = (char *)malloc(strlen(cpath) + 1);
	strcpy(path, cpath);

	pthread_create(&aoThread, NULL, switchvideo_mpv_ao_func, path);
	aoThreadCreated = 1;
	vidlog("[audio] decode thread launched\n");
}

/**
 * Opens the playback synchronization gate to unblock audio playback.
 */
extern void switchvideo_mpv_audio_gate_open(void)
{
	aoGate = 1;
}

/**
 * Polls the initialization state of the audio thread.
 *
 * @return 1 if initial audio buffers are primed and ready for playback, or 0 otherwise.
 */
extern int switchvideo_mpv_ao_is_ready(void)
{
	return aoReady;
}

/**
 * Retrieves the current playback time position in seconds.
 *
 * @return Playback time in seconds.
 */
extern double switchvideo_mpv_ao_get_pos(void)
{
	return aoPosCache;
}

/*
	Called every frame from the main (game) thread.
	All OpenAL calls happen here. The decode thread never touches OpenAL.
*/
extern void switchvideo_mpv_ao_update(void)
{
	if (!aoReady || aoDone)
		return;
	ALCcontext *ctx = alcGetCurrentContext();
	if (!ctx)
		return;

	// create source and buffers on first call
	if (aoSrc == 0)
	{
		ALCdevice *dev = alcGetContextsDevice(ctx);
		const char *devName = dev ? alcGetString(dev, ALC_DEVICE_SPECIFIER) : "null";
		vidlog("[audio] context=%p device=%p name='%s'\n", ctx, dev, devName ? devName : "null");

		ALfloat listenerGain = 0;
		alGetListenerf(AL_GAIN, &listenerGain);
		vidlog("[audio] listener gain=%.2f\n", (double)listenerGain);

		alGetError();
		alGenSources(1, &aoSrc);
		ALenum e = alGetError();
		if (aoSrc == 0 || e != AL_NO_ERROR)
		{
			vidlog("[audio] alGenSources failed: src=%u err=0x%x\n", aoSrc, e);
			aoSrc = 0;
			return;
		}
		alGenBuffers(AL_BUF_COUNT, aoBufs);
		e = alGetError();
		if (e != AL_NO_ERROR)
			vidlog("[audio] alGenBuffers failed: err=0x%x\n", e);
		alSourcef(aoSrc, AL_GAIN, 1.0f);

		ALfloat srcGain = 0;
		alGetSourcef(aoSrc, AL_GAIN, &srcGain);
		vidlog("[audio] source gain=%.2f after set\n", (double)srcGain);

		ALfloat pos[3] = {0,0,0};
		alGetListenerfv(AL_POSITION, pos);
		vidlog("[audio] listener pos=(%.1f,%.1f,%.1f)\n", (double)pos[0], (double)pos[1], (double)pos[2]);

		alGetSourcefv(aoSrc, AL_POSITION, pos);
		vidlog("[audio] source pos=(%.1f,%.1f,%.1f)\n", (double)pos[0], (double)pos[1], (double)pos[2]);

		for (int i = 0; i < AL_BUF_COUNT; i++)
			aoBufHasData[i] = 0;
		vidlog("[audio] AL source=%u created\n", aoSrc);
	}

	/*
		Recycle processed buffers, it can't rely on AL_BUFFERS_PROCESSED on Switch (broken asf),
		so try to unqueue directly and check error.
	*/
	int reclaimed = 0;
	for (int i = 0; i < AL_BUF_COUNT; i++)
	{
		alGetError();
		ALuint buf = 0;
		alSourceUnqueueBuffers(aoSrc, 1, &buf);
		ALenum e = alGetError();
		if (e != AL_NO_ERROR)
			break;
		for (int j = 0; j < AL_BUF_COUNT; j++)
		{
			if (aoBufs[j] == buf)
			{
				aoBufHasData[j] = 0;
				reclaimed++;
				break;
			}
		}
	}
	if (reclaimed > 2)
		vidlog("[audio] reclaimed %d buffers\n", reclaimed);

	// fill empty buffers from ring buffer. Count how many are still queued after reclaim.
	int totalQueued = 0;
	for (int i = 0; i < AL_BUF_COUNT; i++)
		if (aoBufHasData[i])
			totalQueued++;

	for (int i = 0; i < AL_BUF_COUNT; i++)
	{
		if (aoBufHasData[i])
			continue;

		int16_t tmp[AL_BUF_SAMPLES * 2];
		int bytes = AL_BUF_SAMPLES * 2 * (int)sizeof(int16_t);
		int got = ring_read((uint8_t *)tmp, bytes);
		if (got <= 0)
			break;
		{
			int nonZero = 0;
			for (int k = 0; k < got / 2; k++)
				if (tmp[k] != 0)
				{
					nonZero = 1;
					break;
				}
			if (!nonZero)
			{
				static int silentLog = 0;
				if (silentLog++ < 3)
					vidlog("[audio] warning: silence buffer got=%d\n", got);
			}
		}

		alGetError();
		alBufferData(aoBufs[i], AL_FORMAT_STEREO16, tmp, got, 48000);
		ALenum e1 = alGetError();
		alSourceQueueBuffers(aoSrc, 1, &aoBufs[i]);
		ALenum e2 = alGetError();
		if (e1 != AL_NO_ERROR || e2 != AL_NO_ERROR)
			vidlog("[audio] queue failed buf=%u got=%d e1=0x%x e2=0x%x\n", aoBufs[i], got, e1, e2);
		else
		{
			aoBufHasData[i] = 1;
			totalQueued++;
		}
	}

	// start playback when gate is open and we have at least 1 buffer
	ALint state = 0;
	alGetSourcei(aoSrc, AL_SOURCE_STATE, &state);
	if (aoGate && state != AL_PLAYING && state != AL_PAUSED)
	{
		if (totalQueued >= 1)
		{
			ao_stop_primer();
			alGetError();
			alSourcePlay(aoSrc);
			ALenum e = alGetError();
			{
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				aoPlayStartSec = ts.tv_sec + ts.tv_nsec / 1e9;
			}
			ALint queued = 0;
			alGetSourcei(aoSrc, AL_BUFFERS_QUEUED, &queued);
			vidlog("[audio] playback started queued=%d state=%d err=0x%x alQueued=%d\n", totalQueued, state, e, queued);
			alGetSourcei(aoSrc, AL_SOURCE_STATE, &state);
			vidlog("[audio] after play state=%d\n", state);
		}
	}

	// if stopped mid-playback and more data available, restart
	if (aoGate && state == AL_STOPPED && !aoRingEof && totalQueued > 0)
	{
		alSourcePlay(aoSrc);
		vidlog("[audio] restart after stall queued=%d\n", totalQueued);
	}

	// update position, AL_SEC_OFFSET is stuck at ~81ms on Switch, use wall time
	if (aoPlayStartSec > 0)
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		aoPosCache = ts.tv_sec + ts.tv_nsec / 1e9 - aoPlayStartSec;
		ALfloat sec = 0;
		alGetSourcef(aoSrc, AL_SEC_OFFSET, &sec);
		static int posLog = 0;
		if (++posLog % 60 == 0)
			vidlog("[audio] pos wall=%.2f secOffset=%.3f state=%d queued=%d\n", aoPosCache, (double)sec, state, totalQueued);
	}

	// check if playback is complete: EOF + ring empty + source stopped
	if (aoRingEof)
	{
		int ringAvail = (aoRingHead - aoRingTail) & RING_MASK;
		if (ringAvail == 0 && totalQueued == 0 && state == AL_STOPPED && aoPlayStartSec > 0)
		{
			vidlog("[audio] playback complete\n");
			aoDone = 1;
		}
	}
}

extern void switchvideo_mpv_audio_stop_func(void)
{
	aoWantStop = 1;
	aoGate = 0;

	vidlog("[audio] stop: joining decode thread\n");
	if (aoThreadCreated)
	{
		pthread_join(aoThread, NULL);
		aoThreadCreated = 0;
	}
	vidlog("[audio] stop: thread joined, cleaning up AL objects\n");

	if (aoSrc != 0)
	{
		alGetError();
		alSourceStop(aoSrc);
		alSourcei(aoSrc, AL_BUFFER, 0);
		alGetError();
	}

	// Let the mixer process the state changes before we free the objects.
	struct timespec ts = {0, 100000000};
	nanosleep(&ts, NULL);

	if (aoSrc != 0)
	{
		alDeleteSources(1, &aoSrc);
		aoSrc = 0;
	}
	// ONLY delete buffers if they were actually created (non-zero names)
	{
		int haveBufs = 0;
		for (int i = 0; i < AL_BUF_COUNT; i++)
			if (aoBufs[i] != 0) { haveBufs = 1; break; }
		if (haveBufs)
			alDeleteBuffers(AL_BUF_COUNT, aoBufs);
		for (int i = 0; i < AL_BUF_COUNT; i++)
			aoBufs[i] = 0;
	}

	vidlog("[audio] stop: AL objects deleted\n");

	aoWantStop = 0;
	aoReady = 0;
	aoDone = 0;
	aoRingHead = 0;
	aoRingTail = 0;
	aoRingEof = 0;
	aoPlayStartSec = 0;
	aoPosCache = 0;
}
#endif
