/*
 * switchvideo_mpv_native.h - Header for Switch-Funkin cutscene backend
 *
 * Declarations for MPV rendering glue, OpenGL/FBO helpers, and
 * FFmpeg -> OpenAL audio streaming functions defined in switchvideo_mpv_native.cpp.
 */

#ifndef SWITCHVIDEO_MPV_NATIVE_H
#define SWITCHVIDEO_MPV_NATIVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif
    /* =========================================================================
     * MPV Logging Callback
     * ========================================================================= */

    /**
     * Function pointer (from Haxe) type for logging callback.
     * @param msg Log message string.
     */
    typedef void (*SwitchVideoLogFn)(const char *msg);

    /**
     * Sets the MPV logging callback.
     * @param fn Logging callback function.
     */
    void switchvideo_mpv_set_log_callback(SwitchVideoLogFn fn);

    /**
     * Polls for pending log messages from MPV.
     * @param out Output buffer to receive the log message.
     * @param out_size Size of the output buffer.
     * @return 1 if a log message was retrieved, 0 if no messages are pending.
     */
    int switchvideo_mpv_poll_pending_log(char *out, int out_size);

    /* =========================================================================
     * MPV Render Context Management
     * ========================================================================= */

    /**
     * Creates an MPV render context with OpenGL initialization parameters.
     * @param mpv Pointer to the mpv_handle.
     * @param error Pointer to receive the mpv error code (optional).
     * @return Pointer to the created mpv_render_context.
     */
    void *switchvideo_mpv_render_context_create(void *mpv, int *error);

    /**
     * Updates the MPV render context.
     * @param ctx Pointer to the mpv_render_context.
     * @return Non-zero if a new frame should be rendered.
     */
    int switchvideo_mpv_render_context_update(void *ctx);

    /**
     * Renders an MPV frame into the specified Framebuffer Object (FBO).
     * @param ctx Pointer to the mpv_render_context.
     * @param fbo Target FBO ID.
     * @param w Width of the viewport.
     * @param h Height of the viewport.
     */
    void switchvideo_mpv_render_frame(void *ctx, int fbo, int w, int h);

    /**
     * Frees the MPV render context.
     * @param ctx Pointer to the mpv_render_context.
     */
    void switchvideo_mpv_render_context_free(void *ctx);

    /* =========================================================================
     * MPV Commands & Property Getters/Setters
     * ========================================================================= */

    /**
     * Loads a video file into MPV.
     * @param ctx Pointer to the mpv_handle context.
     * @param path Path to the media file.
     * @return MPV error code.
     */
    int switchvideo_mpv_loadfile(void *ctx, const char *path);

    /**
     * Sets a double floating point property in MPV.
     * @param ctx Pointer to the mpv_handle context.
     * @param name Property name.
     * @param v Value to set.
     */
    void switchvideo_mpv_set_double(void *ctx, const char *name, double v);

    /**
     * Gets an integer property from MPV.
     * @param ctx Pointer to the mpv_handle context.
     * @param name Property name.
     * @return Property value as an integer, or 0 on error.
     */
    int switchvideo_mpv_get_int(void *ctx, const char *name);

    /**
     * Gets a double floating point property from MPV.
     * @param ctx Pointer to the mpv_handle context.
     * @param name Property name.
     * @return Property value as double, or 0.0 on error.
     */
    double switchvideo_mpv_get_double(void *ctx, const char *name);

    /**
     * Gets a boolean flag property from MPV.
     * @param ctx Pointer to the mpv_handle context.
     * @param name Property name.
     * @return 1 if true, 0 if false or on error.
     */
    int switchvideo_mpv_get_boolean(void *ctx, const char *name);

    /* =========================================================================
     * MPV Event & Log Inspection
     * ========================================================================= */

    /**
     * Retrieves the end file reason from event data.
     * @param event_data Pointer to mpv_event_end_file.
     * @return Reason ID or -1 if NULL.
     */
    int switchvideo_mpv_end_file_reason(const void *event_data);

    /**
     * Gets the event ID from an MPV event structure.
     * @param evt Pointer to mpv_event.
     * @return Event ID or 0 if NULL.
     */
    int switchvideo_mpv_event_id(const void *evt);

    /**
     * Gets the event data pointer from an MPV event.
     * @param evt Pointer to mpv_event.
     * @return Pointer to data or NULL.
     */
    const void *switchvideo_mpv_event_data(const void *evt);

    /**
     * Gets the log message prefix string.
     * @param data Pointer to mpv_event_log_message.
     * @return Prefix string or NULL.
     */
    const char *switchvideo_mpv_log_message_prefix(const void *data);

    /**
     * Gets the log message text string.
     * @param data Pointer to mpv_event_log_message.
     * @return Text string or NULL.
     */
    const char *switchvideo_mpv_log_message_text(const void *data);

    /* =========================================================================
     * OpenGL / Framebuffer Helpers
     * ========================================================================= */

    /**
     * Creates an OpenGL FBO and associated RGBA texture.
     * @param fbo Pointer to output FBO handle.
     * @param tex Pointer to output Texture handle.
     * @param w Width of the texture/FBO.
     * @param h Height of the texture/FBO.
     */
    void switchvideo_mpv_gl_create_fbo(int *fbo, int *tex, int w, int h);

    /**
     * Reads RGBA pixels from an FBO into a buffer.
     * @param fbo FBO ID to read from.
     * @param w Width of region.
     * @param h Height of region.
     * @param out Output buffer.
     */
    void switchvideo_mpv_gl_read_pixels(int fbo, int w, int h, unsigned char *out);

    /**
     * Deletes an OpenGL FBO and its associated texture.
     * @param fbo FBO handle to delete.
     * @param tex Texture handle to delete.
     */
    void switchvideo_mpv_gl_delete_fbo(int fbo, int tex);

    /**
     * Clears the default framebuffer to black (0,0,0,1).
     */
    void switchvideo_mpv_gl_clear_default(void);

    /**
     * Swaps red and blue channels (BGRA <-> RGBA / ARGB pixel buffer channel swap).
     * @param src Source pixel buffer.
     * @param dst Destination pixel buffer.
     * @param pixels Number of pixels.
     */
    void switchvideo_mpv_rgba_to_argb(const unsigned char *src, unsigned char *dst, int pixels);

    /* =========================================================================
     * Audio Streaming (FFmpeg -> OpenAL)
     * ========================================================================= */

    /**
     * Starts audio decoding and playback thread for a given file path.
     * @param cpath Media file path (e.g. romfs:/...).
     */
    void switchvideo_mpv_audio_start(const char *cpath);

    /**
     * Unlocks the audio gate, signaling playback to begin once video's first frame is ready.
     */
    void switchvideo_mpv_audio_gate_open(void);

    /**
     * Checks if initial audio buffers are decoded and ready.
     * @return Non-zero if ready, 0 otherwise.
     */
    int switchvideo_mpv_ao_is_ready(void);

    /**
     * Gets current audio playback position in seconds.
     * @return Seconds played.
     */
    double switchvideo_mpv_ao_get_pos(void);

    /**
     * Stops the audio thread and releases audio resources.
     */
    void switchvideo_mpv_audio_stop_func(void);

#ifdef __cplusplus
}
#endif

#endif /* SWITCHVIDEO_MPV_NATIVE_H */