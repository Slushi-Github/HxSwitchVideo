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

#include <AL/al.h>
#include <AL/alc.h>

/*
	FFmpeg audio decode to OpenAL buffer‑queue streaming
*/
#include <pthread.h>
#include <stdio.h>
#include <time.h>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#define AO_BUFS 4
#define AO_SAMP 4096
#define AO_IOBUF 32768

static pthread_t aoThread;
static volatile int aoStop = 0;
static int aoAlive = 0;
static ALuint aoSrc = 0;
static ALuint aoBufsInt[AO_BUFS];
static ALuint aoPrimerSrc = 0;
static ALuint aoPrimerBuf = 0;
static ALCcontext *aoCtx = NULL;
static volatile int aoGate = 0;
static volatile int aoReady = 0;

extern void switchvideo_mpv_audio_stop_func(void);
extern void switchvideo_mpv_audio_start(const char *cpath);

/**
 * Interrupt callback invoked by FFmpeg to gracefully handle thread termination.
 */
static int switchvideo_mpv_ao_interrupt(void *unused)
{
	return aoStop;
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

/*
	Decode packets until we get one audio frame, convert to S16 stereo, fill buffer.
	Returns 1 on success, 0 on EOF/error.
*/
static int switchvideo_mpv_ao_fill(AVFormatContext *fmt, AVCodecContext *dec, SwrContext *swr, AVFrame *frm, AVPacket *pkt, int idx, ALuint buf, int16_t *cvt)
{
	for (;;)
	{
		if (avcodec_receive_frame(dec, frm) == 0)
		{
			int n = swr_convert(swr, (uint8_t **)&cvt, AO_SAMP, (const uint8_t **)frm->extended_data, frm->nb_samples);
			if (n > 0)
			{
				ALsizei sz = n * 2 * (int)sizeof(int16_t);
				alBufferData(buf, AL_FORMAT_STEREO16, cvt, sz, 48000);
				{
					ALenum e = alGetError();
					if (e != AL_NO_ERROR)
						vidlog("[audio] alBufferData buf=%u n=%d bytes=%d err=0x%x\n", (unsigned)buf, n, (int)sz, e);
				}
				return 1;
			}
			continue;
		}
		if (av_read_frame(fmt, pkt) < 0)
		{
			avcodec_send_packet(dec, NULL);
			continue;
		}
		if (pkt->stream_index != idx)
		{
			av_packet_unref(pkt);
			continue;
		}
		avcodec_send_packet(dec, pkt);
		av_packet_unref(pkt);
	}
}

static double aoPosCache = 0;
static void switchvideo_mpv_ao_start_primer(void);
static void aoStop_primer(void);

/**
 * Worker thread execution routine responsible for decoding audio via FFmpeg,
 * resampling to 48kHz S16 stereo, and streaming buffers to OpenAL.
 */
static void *switchvideo_mpv_ao_func(void *arg)
{
	char *path = (char *)arg;

	AVFormatContext *fmt = NULL;
	AVCodecContext *dec = NULL;
	SwrContext *swr = NULL;
	AVFrame *frm = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	int16_t *cvt = NULL;
	int idx = -1;
	const AVCodec *codec = NULL;
	AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
	ALenum err;
	ALint state = 0;
	FILE *file = NULL;
	AVIOContext *avio = NULL;

	vidlog("[audio] thread started, path=%s ctx=%p\n", path, aoCtx);

	if (aoCtx)
	{
		if (!alcMakeContextCurrent(aoCtx))
			vidlog("[audio] alcMakeContextCurrent FAILED: 0x%x\n", alcGetError(NULL));
		else
		{
			ALCdevice *dev = alcGetContextsDevice(aoCtx);
			const ALCchar *name = dev ? alcGetString(dev, ALC_DEVICE_SPECIFIER) : NULL;
			vidlog("[audio] context current, device=%s\n", name ? name : "?");
			switchvideo_mpv_ao_start_primer();
		}
	}
	else
		vidlog("[audio] WARNING: no OpenAL context saved\n");

	file = fopen(path, "rb");
	if (!file)
	{
		vidlog("[audio] fopen FAILED: %s\n", path);
		goto end;
	}
	vidlog("[audio] fopen OK\n");

	avio = avio_alloc_context((unsigned char *)av_malloc(AO_IOBUF),
							  AO_IOBUF, 0, file, switchvideo_mpv_ao_read, NULL, switchvideo_mpv_ao_seek);
	if (!avio)
	{
		vidlog("[audio] avio_alloc_context FAILED\n");
		goto end;
	}

	fmt = avformat_alloc_context();
	if (!fmt)
	{
		vidlog("[audio] avformat_alloc_context FAILED\n");
		goto end;
	}
	fmt->pb = avio;
	fmt->interrupt_callback.callback = switchvideo_mpv_ao_interrupt;
	{
		int ret = avformat_open_input(&fmt, NULL, NULL, NULL);
		if (ret < 0)
		{
			vidlog("[audio] avformat_open_input FAILED ret=%d (0x%x)\n", ret, (unsigned)ret);
			if (fmt)
			{
				avformat_free_context(fmt);
				fmt = NULL;
			}
			avio_context_free(&avio);
			fclose(file);
			file = NULL;
			goto end;
		}
	}
	avio = NULL; // fmt owns the AVIO now
	vidlog("[audio] avformat_open_input OK\n");
	if (avformat_find_stream_info(fmt, NULL) < 0)
	{
		vidlog("[audio] avformat_find_stream_info FAILED\n");
		goto end;
	}
	vidlog("[audio] nb_streams=%u\n", fmt->nb_streams);

	for (unsigned i = 0; i < fmt->nb_streams; i++)
	{
		vidlog("[audio] stream[%u] type=%d\n", i, fmt->streams[i]->codecpar->codec_type);
		if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			idx = i;
			break;
		}
	}
	if (idx < 0)
	{
		vidlog("[audio] no audio stream found\n");
		goto end;
	}

	/*
		Discard everything except the audio stream so av_read_frame only
		ever returns audio packets; reading big VP9 video packets was
		slowing the decode loop below real-time.
	*/
	for (unsigned i = 0; i < fmt->nb_streams; i++)
		if ((int)i != idx)
			fmt->streams[i]->discard = AVDISCARD_ALL;

	codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
	if (!codec)
	{
		vidlog("[audio] avcodec_find_decoder FAILED\n");
		goto end;
	}
	vidlog("[audio] decoder=%s\n", codec->name);

	dec = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
	if (avcodec_open2(dec, codec, NULL) < 0)
	{
		vidlog("[audio] avcodec_open2 FAILED\n");
		goto end;
	}
	vidlog("[audio] avcodec_open2 OK: %dHz %dch\n", dec->sample_rate, dec->ch_layout.nb_channels);

	swr = NULL;
	if (swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, 48000, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 || !swr || swr_init(swr) < 0)
	{
		vidlog("[audio] swr_init FAILED\n");
		goto end;
	}
	vidlog("[audio] swr OK\n");

	cvt = (int16_t *)av_malloc(AO_SAMP * 2 * (int)sizeof(int16_t));

	err = alGetError();
	alGenSources(1, &aoSrc);
	err = alGetError();
	vidlog("[audio] alGenSources src=%u err=0x%x\n", aoSrc, err);

	alGenBuffers(AO_BUFS, aoBufsInt);
	err = alGetError();
	vidlog("[audio] alGenBuffers err=0x%x\n", err);

	if (aoSrc == 0)
	{
		vidlog("[audio] FAILED to create AL source\n");
		goto end;
	}

	// Decode + queue in a sliding window, play immediately
	{
		ALuint *bufs = NULL;
		int nbufs = 0, cap = 0;
		int chunk = AO_SAMP;
		int queued = 0;
		int written = 0;
		int eof = 0;

		// Queue first 25 buffers and start immediately
		{
			int want = 25;
			for (int tried = 0; tried < 2000 && queued < want && !aoStop; tried++)
			{
				if (avcodec_receive_frame(dec, frm) == 0)
				{
					int n = swr_convert(swr, (uint8_t **)&cvt, chunk, (const uint8_t **)frm->extended_data, frm->nb_samples);
					if (n > 0)
					{
						if (nbufs >= cap)
						{
							int ncap = cap ? cap * 2 : 64;
							ALuint *nb = (ALuint *)realloc(bufs, ncap * sizeof(ALuint));
							if (!nb)
								break;
							bufs = nb;
							cap = ncap;
						}
						alGenBuffers(1, &bufs[nbufs]);
						alBufferData(bufs[nbufs], AL_FORMAT_STEREO16, cvt, (ALsizei)(n * 2 * sizeof(int16_t)), 48000);
						alSourceQueueBuffers(aoSrc, 1, &bufs[nbufs]);
						nbufs++;
						queued++;
					}
					continue;
				}
				if (av_read_frame(fmt, pkt) < 0)
				{
					avcodec_send_packet(dec, NULL);
					eof = 1;
					break;
				}
				if (pkt->stream_index == idx)
					avcodec_send_packet(dec, pkt);
				av_packet_unref(pkt);
			}
		}

		vidlog("[audio] initial queue: %d buffers (%.2fs)\n", queued, (double)queued * chunk / 48000.0);

		// Signal the game: decode is ready, please unpause
		aoReady = 1;

		/*
			Wait for the video to render its first frame before starting
			playback, so audio and video begin from time 0 together.
		*/
		vidlog("[audio] waiting for video first frame\n");
		while (!aoGate && !aoStop)
		{
			{
				struct timespec ts = {0, 10000000};
				nanosleep(&ts, NULL);
			}
		}
		if (aoStop)
		{
			vidlog("[audio] stopped while waiting for gate\n");
			goto end;
		}
		vidlog("[audio] gate open, starting playback\n");
		aoStop_primer();

		alSourcePlay(aoSrc);
		vidlog("[audio] alSourcePlay err=0x%x\n", alGetError());
		{
			ALint st = 0, q = 0, pr = 0;
			alGetSourcei(aoSrc, AL_SOURCE_STATE, &st);
			alGetSourcei(aoSrc, AL_BUFFERS_QUEUED, &q);
			alGetSourcei(aoSrc, AL_BUFFERS_PROCESSED, &pr);
			vidlog("[audio] after play: state=%d queued=%d processed=%d err=0x%x\n", st, q, pr, alGetError());
		}

		/*
			Keep decoding and appending; the mixer consumes from the front,
			we add to the back. AL_BUFFERS_PROCESSED and AL_SAMPLE_OFFSET
			are broken on Switch openal-soft, so we just decode until EOF
			and wait for the source to finish.
		*/
		int spike = 0;
		while (!aoStop)
		{
			state = 0;
			alGetSourcei(aoSrc, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING && state != AL_PAUSED)
			{
				ALint q = 0, pr = 0;
				alGetSourcei(aoSrc, AL_SOURCE_STATE, &q);
				alGetSourcei(aoSrc, AL_BUFFERS_PROCESSED, &pr);
				vidlog("[audio] playback ended: state=%d queued=%d processed=%d err=0x%x\n", state, q, pr, alGetError());
				break;
			}

			// Try to decode more
			if (!eof && avcodec_receive_frame(dec, frm) == 0)
			{
				int n = swr_convert(swr, (uint8_t **)&cvt, chunk,
									(const uint8_t **)frm->extended_data, frm->nb_samples);
				if (n > 0)
				{
					if (nbufs >= cap)
					{
						int ncap = cap ? cap * 2 : 64;
						ALuint *nb = (ALuint *)realloc(bufs, ncap * sizeof(ALuint));
						if (!nb)
							break;
						bufs = nb;
						cap = ncap;
					}
					alGenBuffers(1, &bufs[nbufs]);
					alBufferData(bufs[nbufs], AL_FORMAT_STEREO16, cvt, (ALsizei)(n * 2 * sizeof(int16_t)), 48000);
					alSourceQueueBuffers(aoSrc, 1, &bufs[nbufs]);
					{
						ALenum e = alGetError();
						if (e != AL_NO_ERROR)
							vidlog("[audio] queue path err=0x%x at nbufs=%d\n", e, nbufs);
					}
					nbufs++;
					queued++;
					if (nbufs % 100 == 0)
						vidlog("[audio] total=%d queued=%d (%.2fs)\n", nbufs, queued, (double)queued * chunk / 48000.0);
				}
			}
			else if (!eof)
			{
				if (av_read_frame(fmt, pkt) < 0)
				{
					avcodec_send_packet(dec, NULL);
					eof = 1;
					vidlog("[audio] decode EOF, total=%d\n", nbufs);
				}
				else
				{
					if (pkt->stream_index == idx)
						avcodec_send_packet(dec, pkt);
					av_packet_unref(pkt);
				}
			}

			if (++spike % 500 == 0)
			{
				ALint st = 0, q = 0, pr = 0, off = 0;
				ALfloat sec = 0;
				alGetSourcei(aoSrc, AL_SOURCE_STATE, &st);
				alGetSourcei(aoSrc, AL_BUFFERS_QUEUED, &q);
				alGetSourcei(aoSrc, AL_BUFFERS_PROCESSED, &pr);
				alGetSourcei(aoSrc, AL_SAMPLE_OFFSET, &off);
				alGetSourcef(aoSrc, AL_SEC_OFFSET, &sec);
				aoPosCache = (double)sec;
				vidlog("[audio] tick state=%d queued=%d processed=%d offset=%d pos=%.3f\n", st, q, pr, off, sec);
			}
			{
				struct timespec ts = {0, 2000000};
				nanosleep(&ts, NULL);
			}
		}

		vidlog("[audio] done: total=%d\n", nbufs);
		if (bufs)
		{
			alSourceStop(aoSrc);
			alDeleteSources(1, &aoSrc);
			aoSrc = 0;
			alDeleteBuffers(nbufs, bufs);
			free(bufs);
		}
	}

end:
	vidlog("[audio] cleanup\n");
	aoStop_primer();
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
	aoAlive = 0;
	alcMakeContextCurrent(NULL);
	return NULL;
}

/**
 * Spawns the audio streaming thread targeting a media file path.
 * Stops any currently running audio instance prior to launch.
 *
 * @param cpath File path string to load audio from.
 */
extern void switchvideo_mpv_audio_start(const char *cpath)
{
	vidlog("[audio] switchvideo_mpv_audio_start: %s\n", cpath);
	switchvideo_mpv_audio_stop_func();
	aoStop = 0;
	aoAlive = 1;
	aoGate = 0;
	aoReady = 0;
	aoCtx = alcGetCurrentContext();
	vidlog("[audio] saved ctx=%p\n", aoCtx);
	char *path = (char *)malloc(strlen(cpath) + 1);
	strcpy(path, cpath);
	pthread_create(&aoThread, NULL, switchvideo_mpv_ao_func, path);
	vidlog("[audio] thread created\n");
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
	The Switch openal-soft mixer stops consuming buffers if the AL device
	sits idle (no playing source) for a while; the source then stays
	AL_PLAYING forever with AL_BUFFERS_PROCESSED stuck at 0. Keep a
	looping zero-gain silence source alive while we wait for the video
	gate so the device never goes idle.
*/
static void switchvideo_mpv_ao_start_primer(void)
{
	if (aoPrimerBuf != 0 || aoPrimerSrc != 0)
		return;
	short *sil = (short *)malloc(48000 * 2 * (int)sizeof(short));
	memset(sil, 0, 48000 * 2 * (int)sizeof(short));
	alGenBuffers(1, &aoPrimerBuf);
	alBufferData(aoPrimerBuf, AL_FORMAT_STEREO16, sil,
				 48000 * 2 * (int)sizeof(short), 48000);
	free(sil);
	alGenSources(1, &aoPrimerSrc);
	alSourcei(aoPrimerSrc, AL_LOOPING, AL_TRUE);
	alSourcei(aoPrimerSrc, AL_BUFFER, aoPrimerBuf);
	alSourcef(aoPrimerSrc, AL_GAIN, 0.0f);
	alSourcePlay(aoPrimerSrc);
	vidlog("[audio] primer source playing\n");
}

/**
 * Stops and destroys the zero-gain silent OpenAL primer source.
 */
static void aoStop_primer(void)
{
	if (aoPrimerSrc != 0)
	{
		alSourceStop(aoPrimerSrc);
		alDeleteSources(1, &aoPrimerSrc);
		aoPrimerSrc = 0;
	}
	if (aoPrimerBuf != 0)
	{
		alDeleteBuffers(1, &aoPrimerBuf);
		aoPrimerBuf = 0;
	}
}

/**
 * Signals the audio thread to terminate and waits for join completion.
 */
extern void switchvideo_mpv_audio_stop_func(void)
{
	aoStop = 1;
	if (aoAlive)
		pthread_join(aoThread, NULL);
	aoAlive = 0;
	aoCtx = NULL;
}
#endif