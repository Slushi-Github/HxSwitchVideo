package switchvideo.bindings;

/**
 * Minimal mpv C API bindings for Nintendo Switch
 */
@:buildXml('<include name="${haxelib:hxswitchvideo}/project/build.xml"/>')
@:include("mpv/client.h")
@:include("mpv/render.h")
@:include("mpv/render_gl.h")
extern class Mpv
{
	// event ids (mpv_event_id, see mpv/client.h)
	public static inline final EVENT_NONE:Int = 0;
	public static inline final EVENT_SHUTDOWN:Int = 1;
	public static inline final EVENT_LOG_MESSAGE:Int = 2;
	public static inline final EVENT_END_FILE:Int = 7;
	public static inline final EVENT_FILE_LOADED:Int = 8;
	public static inline final EVENT_VIDEO_RECONFIG:Int = 17;
	public static inline final EVENT_AUDIO_RECONFIG:Int = 18;

	// end_file reasons (mpv_end_file_reason)
	public static inline final END_FILE_REASON_EOF:Int = 0;
	public static inline final END_FILE_REASON_STOP:Int = 2;
	public static inline final END_FILE_REASON_ERROR:Int = 4;
	
	/////////////////////////////////////

	// instance lifecycle
	@:native("mpv_create")
	public static function mpv_create():Pointer<CppVoid>;

	@:native("mpv_initialize")
	public static function mpv_initialize(ctx:Pointer<CppVoid>):Int;

	@:native("mpv_terminate_destroy")
	public static function mpv_terminate_destroy(ctx:Pointer<CppVoid>):Void;

	// commands/options
	@:native("mpv_command_string")
	public static function mpv_command_string(ctx:Pointer<CppVoid>, args:ConstCharStar):Int;

	@:native("mpv_set_option_string")
	public static function mpv_set_option_string(ctx:Pointer<CppVoid>, name:ConstCharStar, data:ConstCharStar):Int;

	@:native("mpv_set_property_string")
	public static function mpv_set_property_string(ctx:Pointer<CppVoid>, name:ConstCharStar, data:ConstCharStar):Int;

	// properties
	@:native("mpv_get_property_string")
	public static function mpv_get_property_string(ctx:Pointer<CppVoid>, name:ConstCharStar):ConstCharStar;

	@:native("mpv_free")
	public static function mpv_free(data:Pointer<CppVoid>):Void;

	// events
	@:native("mpv_wait_event")
	public static function mpv_wait_event(ctx:Pointer<CppVoid>, timeout:Float):Pointer<CppVoid>;

	@:native("mpv_request_log_messages")
	public static function mpv_request_log_messages(ctx:Pointer<CppVoid>, level:ConstCharStar):Int;
}