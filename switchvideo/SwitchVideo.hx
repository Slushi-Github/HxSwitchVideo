// By DgM (@doggywatty on GitHub) and Slushi (@Slushi-Github on GitHub)

package switchvideo;

import cpp.Pointer;
import cpp.UInt8;
import cpp.Void as CppVoid;
import cpp.Float32;
import haxe.io.Bytes;

import openfl.display.BitmapData;
import openfl.geom.Rectangle;
import openfl.utils.ByteArray;

import flixel.FlxSprite;
import flixel.FlxG;
import flixel.graphics.FlxGraphic;
import flixel.util.FlxColor;

/**
 * MPV-based video sprite for Nintendo Switch.
 */
@:access(switchvideo.backend.SwitchVideoLog)
class SwitchVideo extends FlxSprite
{
	/**
	 * Indicates whether the video is currently playing.
	 */
	public var isPlaying(get, never):Bool;

	/**
	 * Callback fired when the video format configuration is ready.
	 */
	public var onFormatSetup:Void->Void;

	/**
	 * Callback fired when the video reaches the end.
	 */
	public var onEndReached:Void->Void;

	/**
	 * Compatibility wrapper to match hxvlc's bitmap API.
	 */
	public var bitmap(default, null):VideoBitmapWrapper;

	/**
	 * Current video playback time in milliseconds.
	 */
	public var time(get, set):Int;

	/**
	 * Total duration of the video in milliseconds.
	 */
	public var duration(get, never):Int;

	/**
	 * Current video position represented as a percentage (from 0.0 to 1.0).
	 */
	public var position(get, set):Float32;

	/**
	 * Indicates whether the video supports seeking to different points in time.
	 */
	public var isSeekable(get, never):Bool;

	////////////////////////////////////////

	/**
	 * Pointer to the main MPV instance context.
	 */
	private var mpvPtr:Pointer<CppVoid> = null;

	/**
	 * Pointer to the native C++ MPV render context.
	 */
	private var renderPtr:Pointer<CppVoid> = null;

	/**
	 * Pointer to the contiguous memory buffer in RGBA format.
	 */
	private var rgbaPtr:Pointer<UInt8> = null;

	/**
	 * Pointer to the contiguous memory buffer in ARGB format.
	 */
	private var argbPtr:Pointer<UInt8> = null;

	/**
	 * Byte buffer holding image data in RGBA format.
	 */
	private var rgbaBytes:Bytes;

	/**
	 * Byte buffer holding image data in ARGB format.
	 */
	private var argbBytes:Bytes;

	/**
	 * OpenGL Framebuffer Object (FBO) ID used by MPV for rendering.
	 */
	private var fbo:Int = 0;

	/**
	 * OpenGL Texture ID associated with the Framebuffer Object.
	 */
	private var texture:Int = 0;

	/**
	 * BitmapData instance where decoded video frames are copied.
	 */
	private var videoBitmapData:BitmapData;

	/**
	 * Rectangular bounds defining frame dimensions for pixel updates.
	 */
	private var rect:Rectangle;

	/**
	 * Internal state flag tracking active playback.
	 */
	private var _isPlaying:Bool = false;

	/**
	 * Internal state flag tracking paused state.
	 */
	private var _isPaused:Bool = false;

	/**
	 * Flag indicating whether the video should automatically loop upon completion.
	 */
	private var _shouldLoop:Bool = false;

	/**
	 * Flag confirming whether video dimensions and format have been initialized.
	 */
	private var _formatReady:Bool = false;

	/**
	 * Controls whether the video and audio stream has officially started playback.
	 */
	private var _started:Bool = false;

	/**
	 * Tracks whether a stop request was issued.
	 */
	private var _stopRequested:Bool = false;

	/**
	 * Stores the playback state prior to losing window focus so it can resume upon regain.
	 */
	private var resumeOnFocus:Bool = false;

	/**
	 * Logging control flag to prevent duplicate output regarding screen and camera bounds.
	 */
	private var _hasLoggedScreen:Bool = false;

	/**
	 * Internal timer used to throttle periodic audio/video sync logs.
	 */
	private var _startWait:Float = 0;

	/**
	 * Flags whether MPV resource cleanup and disposal is scheduled for the next frame.
	 */
	private var _teardownQueued:Bool = false;

	/**
	 * File path of the currently loaded video file.
	 */
	private var _videoPath:String = "";

	/**
	 * Native width of the video file in pixels.
	 */
	private var _videoWidth:Int = 0;

	/**
	 * Native height of the video file in pixels.
	 */
	private var _videoHeight:Int = 0;

	/**
	 * Multiplier factor used to adjust relative video volume.
	 */
	private var _volumeAdjust:Float = 1.0;

	/**
	 * Current video playback speed rate.
	 */
	private var _rate:Float = 1.0;

	/**
	 * Static flag ensuring the SwitchVideoLog bridge is registered only once.
	 */
	private static var _logBridgeDone:Bool = false;

	/**
	 * Initializes the SwitchVideo instance, sets up the bitmap wrapper, and registers focus signals.
	 */
	public function new()
	{
		super();
		bitmap = new VideoBitmapWrapper(this);
		makeGraphic(1, 1, FlxColor.TRANSPARENT);

		if (!_logBridgeDone)
		{
			SwitchVideoLog.prepare();
			_logBridgeDone = true;
		}

		if (!FlxG.signals.focusGained.has(onFocusGained))
			FlxG.signals.focusGained.add(onFocusGained);
		if (!FlxG.signals.focusLost.has(onFocusLost))
			FlxG.signals.focusLost.add(onFocusLost);
	}

	/**
	 * Loads and initializes a video file using MPV with hardware acceleration.
	 * @param path The relative or absolute file path to the video file to load.
	 * @param options Optional array of configuration options and flags passed directly to MPV.
	 * @return Bool `true` if the video was successfully loaded and initialized; `false` otherwise.
	 */
	public function load(path:String, ?options:Array<String>):Bool
	{
		if (path == null || path == "") {
			SwitchVideoLog.log('SwitchVideo.load: Path is null or empty');
			return false;
		} 

		SwitchVideoLog.log('SwitchVideo.load: Path: $path');
		try
		{
			dispose();

			_videoPath = path;
			_shouldLoop = options != null && options.indexOf('input-repeat=65545') != -1;
			_formatReady = false;
			_stopRequested = true;

			mpvPtr = Mpv.mpv_create();
			if (mpvPtr == null)
			{
				SwitchVideoLog.log('SwitchVideo.load: Mpv.mpv_create() returned null');
				return false;
			}

			// SwitchWave style thing setup; hardware decode plus OpenGL render API
			Mpv.mpv_set_option_string(mpvPtr, 'vo', 'libmpv');
			Mpv.mpv_set_option_string(mpvPtr, 'ao', 'null');
			Mpv.mpv_set_option_string(mpvPtr, 'msg-level', 'all=v');
			Mpv.mpv_set_property_string(mpvPtr, 'keepaspect', 'no');

			if (_shouldLoop)
				Mpv.mpv_set_property_string(mpvPtr, 'loop-file', 'inf');

			if (Mpv.mpv_initialize(mpvPtr) < 0)
			{
				SwitchVideoLog.log('SwitchVideo.load: mpv_initialize FAILED');
				dispose();
				return false;
			}
			
			SwitchVideoLog.log('SwitchVideo.load: mpv_initialize OK');

			Mpv.mpv_request_log_messages(mpvPtr, 'info');

			var err:Int = 0;
			renderPtr = SwitchVideoBackend.switchvideo_mpv_render_context_create(mpvPtr, Pointer.addressOf(err));
			if (renderPtr == null)
			{
				SwitchVideoLog.log('SwitchVideo.load: Render context FAILED (err=$err)');
				dispose();
				return false;
			}
			SwitchVideoLog.log('SwitchVideo.load: Render context OK');

			if (SwitchVideoBackend.switchvideo_mpv_loadfile(mpvPtr, path) < 0)
			{
				SwitchVideoLog.log('SwitchVideo.load: Loadfile FAILED for "$path"');
				dispose();
				return false;
			}
			SwitchVideoLog.log('SwitchVideo.load: Loadfile OK, waiting for events');

			// keep the movie clock frozen while mpv prerolls its first frame,
			// the game unpauses when the audio thread reports ready
			Mpv.mpv_set_property_string(mpvPtr, 'pause', 'yes');
			_started = false;

			_stopRequested = false;
			_isPlaying = false;
			_isPaused = false;
			return true;
		}
		catch (e:Dynamic)
		{
			SwitchVideoLog.log('SwitchVideo: Load error: $e');
			return false;
		}
	}

	/**
	 * Starts playing the loaded video and initiates native audio playback.
	 * @return Bool `true` if the play command was successfully issued; `false` if the MPV instance is invalid.
	 */
	public function play():Bool
	{
		if (mpvPtr == null)
			return false;

		SwitchVideoBackend.switchvideo_mpv_audio_start(_videoPath);
		_isPlaying = true;
		_isPaused = false;
		_startWait = 0;
		return true;
	}

	/**
	 * Pauses video playback.
	 */
	public function pause():Void
	{
		if (mpvPtr != null && _isPlaying)
		{
			Mpv.mpv_set_property_string(mpvPtr, 'pause', 'yes');
			_isPaused = true;
		}
	}

	/**
	 * Resumes video playback if currently paused.
	 */
	public function resume():Void
	{
		if (mpvPtr != null && _isPaused)
		{
			Mpv.mpv_set_property_string(mpvPtr, 'pause', 'no');
			_isPaused = false;
		}
	}

	/**
	 * Stops video playback and queues cleanup for the next update cycle.
	 */
	public function stop():Void
	{
		if (mpvPtr != null)
		{
			_stopRequested = true;
			Mpv.mpv_command_string(mpvPtr, 'stop');
			_isPlaying = false;
			_isPaused = false;
			_teardownQueued = true;
		}
	}

	/**
	 * Sets the playback speed rate of the video.
	 * @param value Speed multiplier (e.g., 1.0 for normal speed, 0.5 for half speed, 2.0 for double speed).
	 */
	public function setRate(value:Float):Void
	{
		_rate = value;
		if (mpvPtr != null)
			SwitchVideoBackend.switchvideo_mpv_set_double(mpvPtr, 'speed', value);
	}

	/**
	 * Adjusts the volume level of the video within MPV.
	 * @param volume Volume level from 0.0 (muted) to 1.0 (maximum volume).
	 */
	public function setVolume(volume:Float):Void
	{
		if (mpvPtr != null)
		{
			final mpvVolume:Int = Math.round(Math.max(0, Math.min(100, volume * 100 * _volumeAdjust)));
			Mpv.mpv_set_property_string(mpvPtr, 'volume', Std.string(mpvVolume));
		}
	}

	/**
	 * Adjusts the internal volume multiplier factor and updates master playback volume.
	 * @param value Volume scaling multiplier.
	 */
	public function setVolumeAdjust(value:Float):Void
	{
		_volumeAdjust = value;
		#if FLX_SOUND_SYSTEM
		setVolume((FlxG.sound.muted ? 0 : 1) * FlxG.sound.volume);
		#else
		setVolume(1);
		#end
	}

	public function setLooping(mode:Bool):Void
	{
		_shouldLoop = mode;
	}

	/**
	 * Disposes of MPV instances, render contexts, OpenGL Framebuffers, and allocated memory buffers.
	 */
	public function dispose():Void
	{
		SwitchVideoBackend.switchvideo_mpv_audio_stop_func();
		SwitchVideoLog.log('dispose: audio stopped');
		if (mpvPtr != null)
		{
			SwitchVideoLog.log('dispose: skipping mpv_terminate_destroy (crashes Switch mixer thread)');
			mpvPtr = null;
		}
		if (renderPtr != null)
		{
			SwitchVideoLog.log('dispose: skipping render_context_free (mpv not terminated, would deadlock)');
			renderPtr = null;
		}
		SwitchVideoLog.log('dispose: GL cleanup');
		if (fbo != 0)
		{
			SwitchVideoBackend.switchvideo_mpv_gl_delete_fbo(fbo, texture);
			fbo = 0;
			texture = 0;
		}
		SwitchVideoBackend.switchvideo_mpv_gl_clear_default();
		rgbaBytes = null;
		argbBytes = null;
		rgbaPtr = null;
		argbPtr = null;
		rect = null;
		/*
			Do NOT dispose videoBitmapData directly, it is owned by "graphic" (which is, FlxGraphic).
			Disposing it here then letting super.destroy() destroy "graphic" causes double-free
			and heap corruption that later aborts at a memory address, that is "Instruction Abort".
		*/
		if (graphic != null)
		{
			graphic.destroy();
			graphic = null;
		}
		videoBitmapData = null;
		_isPlaying = false;
		_isPaused = false;
		_formatReady = false;
	}

	/**
	 * @deprecated kept for reference
	 */
	public function dispose_old():Void
	{
		SwitchVideoBackend.switchvideo_mpv_audio_stop_func();

		// teardown order matters: the mpv core must be terminated BEFORE the
		// render context is freed - mpv_render_context_free() blocks until the
		// core releases it, and the core (mid-uninit at EOF) never does if we
		// free the context first = deadlock = frozen game + stuck last frame.
		if (mpvPtr != null)
		{
			SwitchVideoLog.log('dispose: Terminating mpv');
			Mpv.mpv_terminate_destroy(mpvPtr);
			mpvPtr = null;
		}

		if (renderPtr != null)
		{
			SwitchVideoLog.log('dispose: Freeing render ctx');
			SwitchVideoBackend.switchvideo_mpv_render_context_free(renderPtr);
			renderPtr = null;
		}

		SwitchVideoLog.log('dispose: GL cleanup');

		if (fbo != 0)
		{
			SwitchVideoBackend.switchvideo_mpv_gl_delete_fbo(fbo, texture);
			fbo = 0;
			texture = 0;
		}

		// insurance: clear the default framebuffer so no stale video pixels
		// survive into the next frame/state (ghost-frame fix)
		SwitchVideoBackend.switchvideo_mpv_gl_clear_default();

		rgbaBytes = null;
		argbBytes = null;
		rgbaPtr = null;
		argbPtr = null;
		rect = null;

		if (videoBitmapData != null)
		{
			videoBitmapData.dispose();
			videoBitmapData = null;
		}

		_isPlaying = false;
		_isPaused = false;
		_formatReady = false;
	}

	/**
	 * Polls and processes pending event messages emitted by the internal MPV engine.
	 */
	private function pollEvents():Void
	{
		if (mpvPtr == null)
			return;
		
		var evt:Pointer<CppVoid> = Mpv.mpv_wait_event(mpvPtr, 0.0);
		while (evt != null && SwitchVideoBackend.switchvideo_mpv_event_id(evt) != Mpv.EVENT_NONE)
		{
			switch (SwitchVideoBackend.switchvideo_mpv_event_id(evt))
			{
				case Mpv.EVENT_FILE_LOADED:
					SwitchVideoLog.log('mpv event: FILE_LOADED');
					setupVideo();
				case Mpv.EVENT_VIDEO_RECONFIG:
					SwitchVideoLog.log('mpv event: VIDEO_RECONFIG');
					if (_formatReady)
						setupVideo();
				case Mpv.EVENT_AUDIO_RECONFIG:
					SwitchVideoLog.log('mpv event: AUDIO_RECONFIG');
				case Mpv.EVENT_LOG_MESSAGE:
					final prefix:String = SwitchVideoBackend.switchvideo_mpv_log_message_prefix(SwitchVideoBackend.switchvideo_mpv_event_data(evt));
					final text:String = SwitchVideoBackend.switchvideo_mpv_log_message_text(SwitchVideoBackend.switchvideo_mpv_event_data(evt));
					SwitchVideoLog.log('mpv[$prefix] ${StringTools.trim(text)}');
				case Mpv.EVENT_END_FILE:
					final reason:Int = SwitchVideoBackend.switchvideo_mpv_end_file_reason(SwitchVideoBackend.switchvideo_mpv_event_data(evt));
					SwitchVideoLog.log('mpv event: END_FILE (Reason=$reason)');
					if (reason == Mpv.END_FILE_REASON_EOF || reason == Mpv.END_FILE_REASON_ERROR)
					{
						handleVideoEnd();
						if (mpvPtr == null)
							return;
					}
				case Mpv.EVENT_SHUTDOWN:
					mpvPtr = null;
					return;
			}
			if (mpvPtr != null)
				evt = Mpv.mpv_wait_event(mpvPtr, 0.0);
		}
	}

	/**
	 * Configures video dimensions, creates OpenGL Framebuffers (FBOs), allocates pixel buffers, and resizes sprite graphics.
	 */
	private function setupVideo():Void
	{
		if (mpvPtr == null)
			return;

		final w:Int = SwitchVideoBackend.switchvideo_mpv_get_int(mpvPtr, 'width');
		final h:Int = SwitchVideoBackend.switchvideo_mpv_get_int(mpvPtr, 'height');

		if (w <= 0 || h <= 0)
		{
			SwitchVideoLog.log('setupVideo: Bad dims w=$w h=$h, bailing');
			return;
		}

		SwitchVideoLog.log('setupVideo: Video=${x}${h}, Screen=${FlxG.width}x${FlxG.height}, OnFormatSetup=${onFormatSetup != null}');

		_videoWidth = w;
		_videoHeight = h;

		if (fbo != 0)
			SwitchVideoBackend.switchvideo_mpv_gl_delete_fbo(fbo, texture);

		var newFbo:Int = 0;
		var newTex:Int = 0;
		SwitchVideoBackend.switchvideo_mpv_gl_create_fbo(Pointer.addressOf(newFbo), Pointer.addressOf(newTex), w, h);
		fbo = newFbo;
		texture = newTex;

		rgbaBytes = Bytes.alloc(w * h * 4);
		argbBytes = Bytes.alloc(w * h * 4);

		rgbaPtr = CPPHelpers.bytesToPointer(rgbaBytes);
		argbPtr = CPPHelpers.bytesToPointer(argbBytes);
		rect = new Rectangle(0, 0, w, h);

		if (videoBitmapData != null)
			videoBitmapData.dispose();

		videoBitmapData = new BitmapData(w, h, true, 0x00000000);
		loadGraphic(FlxGraphic.fromBitmapData(videoBitmapData, false, null, false));
		setGraphicSize(FlxG.width, FlxG.height);
		updateHitbox();

		_formatReady = true;

		if (_rate != 1.0)
			setRate(_rate);
		if (onFormatSetup != null)
			onFormatSetup();

		SwitchVideoLog.log('setupVideo: Done: scale=${scale.x}x${scale.y}, pos=$x, $y, frame=${frameWidth}x${frameHeight}');
	}

	/**
	 * Handles application focus gain signal to resume playback if suspended previously.
	 */
	private function onFocusGained():Void
	{
		if (!FlxG.autoPause)
			return;

		if (resumeOnFocus)
		{
			resumeOnFocus = false;
			resume();
		}
	}

	/**
	 * Handles application focus loss signal to pause video playback automatically.
	 */
	private function onFocusLost():Void
	{
		if (!FlxG.autoPause)
			return;

		resumeOnFocus = isPlaying;
		pause();
	}

	/**
	 * Handles video completion logic (restarts file if loop is enabled or queues teardown).
	 */
	private function handleVideoEnd():Void
	{
		if (_shouldLoop)
		{
			// loop-file handles this internally, but fall back to a manual restart
			if (mpvPtr != null && _videoPath != null)
			{
				SwitchVideoBackend.switchvideo_mpv_loadfile(mpvPtr, _videoPath);
				Mpv.mpv_set_property_string(mpvPtr, 'pause', 'no');
				_isPlaying = true;
				_isPaused = false;
			}
		}
		else
		{
			_isPlaying = false;
			// defer dispose() until the next update: mpv is still unwinding
			// after END_FILE, and tearing it down from inside the event
			// handler can block forever (see dispose())
			_teardownQueued = true;
			SwitchVideoLog.log('handleVideoEnd: EOF, teardown queued');
			if (onEndReached != null)
				onEndReached();
		}
	}

	override public function update(elapsed:Float):Void
	{
		super.update(elapsed);

		if (_logBridgeDone)
			SwitchVideoLog.pump();

		if (mpvPtr == null)
			return;

		if (_teardownQueued)
		{
			_teardownQueued = false;
			SwitchVideoLog.log('update: Running queued teardown');
			dispose();
			return;
		}

		pollEvents();

		if (_teardownQueued)
			SwitchVideoLog.log('update: Post-poll, teardown still queued');

		if (_isPlaying && !_isPaused)
		{
			if (!_started && _formatReady && mpvPtr != null && SwitchVideoBackend.switchvideo_mpv_ao_is_ready() != 0)
			{
				_started = true;
				Mpv.mpv_set_property_string(mpvPtr, 'pause', 'no');
				SwitchVideoBackend.switchvideo_mpv_audio_gate_open();
				SwitchVideoLog.log('start: Audio ready, unpaused mpv');
			}
		}

		if (_isPlaying && !_isPaused)
			SwitchVideoBackend.switchvideo_mpv_ao_update();

		if (_isPlaying && !_isPaused && _formatReady && renderPtr != null && videoBitmapData != null)
		{
			try
			{
				final flags:Int = SwitchVideoBackend.switchvideo_mpv_render_context_update(renderPtr);
				if ((flags & 1) == 0)
					return;

				SwitchVideoBackend.switchvideo_mpv_render_frame(renderPtr, fbo, _videoWidth, _videoHeight);
				SwitchVideoBackend.switchvideo_mpv_gl_read_pixels(fbo, _videoWidth, _videoHeight, rgbaPtr);
				SwitchVideoBackend.switchvideo_mpv_rgba_to_argb(rgbaPtr, argbPtr, _videoWidth * _videoHeight);

				videoBitmapData.lock();
				videoBitmapData.setPixels(rect, ByteArray.fromBytes(argbBytes));
				videoBitmapData.unlock();

				if (graphic != null && graphic.bitmap == videoBitmapData)
				{
					graphic.bitmap = videoBitmapData;
					dirty = true;
				}

				if (!_hasLoggedScreen)
				{
					_hasLoggedScreen = true;
					if (!_started && mpvPtr != null)
					{
						_started = true;
						Mpv.mpv_set_property_string(mpvPtr, 'pause', 'no');
						SwitchVideoLog.log('First frame: fallback unpause (audio not ready)');
					}
					SwitchVideoBackend.switchvideo_mpv_audio_gate_open();
					SwitchVideoLog.log('First frame: opened audio gate, video_time=${get_time()}ms');
					final cam = cameras != null && cameras.length > 0 ? cameras[0] : null;
					SwitchVideoLog.log('First frame: sprite x=$x y=$y w=$width h=$height scale=${scale.x}x${scale.y}');
					SwitchVideoLog.log('First frame: screenPos=${getScreenPosition()} screenBounds=${getScreenBounds()}');
					SwitchVideoLog.log('First frame: cam=${cam != null ? cam.width + "x" + cam.height + " zoom=" + cam.zoom : "null"}');
					SwitchVideoLog.log('First frame: FlxG.screen=${FlxG.width}x${FlxG.height} zoom=${FlxG.camera.zoom}');
				}

				_startWait += elapsed;
				if (_startWait >= 1.0)
				{
					_startWait = 0;
					SwitchVideoLog.log('Probe w=${Std.int(Sys.time() * 1000000)} v=${get_time()} a=${Std.int(SwitchVideoBackend.switchvideo_mpv_ao_get_pos() * 1000)}');
				}
			}
			catch (e:Dynamic)
			{
				#if debug
				SwitchVideoLog.log('Error playing frame: $e');
				#end
			}
		}
	}

	override public function destroy():Void
	{
		if (FlxG.signals.focusGained.has(onFocusGained))
			FlxG.signals.focusGained.remove(onFocusGained);
		if (FlxG.signals.focusLost.has(onFocusLost))
			FlxG.signals.focusLost.remove(onFocusLost);

		dispose();
		super.destroy();
	}

	//////////////////////////////////////

	private function get_isPlaying():Bool
		return _isPlaying && !_isPaused;

	private function get_time():Int {
		if (mpvPtr == null)
			return 0;
		return Math.round(SwitchVideoBackend.switchvideo_mpv_get_double(mpvPtr, 'time-pos') * 1000);
	}

	private function set_time(value:Int):Int
	{
		if (mpvPtr != null)
			SwitchVideoBackend.switchvideo_mpv_set_double(mpvPtr, 'time-pos', value / 1000);
		return value;
	}

	private function get_duration():Int {
		if (mpvPtr == null)
			return 0;
		return Math.round(SwitchVideoBackend.switchvideo_mpv_get_double(mpvPtr, 'duration') * 1000);
	}

	private function get_position():Float32 {
		if (mpvPtr == null)
			return 0;
		return SwitchVideoBackend.switchvideo_mpv_get_double(mpvPtr, 'percent-pos') / 100;
	}

	private function set_position(value:Float32):Float32
	{
		if (mpvPtr != null)
			SwitchVideoBackend.switchvideo_mpv_set_double(mpvPtr, 'percent-pos', value * 100);
		return value;
	}

	private function get_isSeekable():Bool
		return mpvPtr != null && SwitchVideoBackend.switchvideo_mpv_get_boolean(mpvPtr, 'seekable') != 0;
}
