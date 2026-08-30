package switchvideo.bindings;

@:buildXml('<include name="${haxelib:hxswitchvideo}/project/build.xml"/>')
@:include("switchvideo_mpv_native.h")
extern class SwitchVideoBackend {
	// properties
	@:native("switchvideo_mpv_get_int")
	public static function switchvideo_mpv_get_int(ctx:Pointer<CppVoid>, name:ConstCharStar):Int;

	@:native("switchvideo_mpv_get_double")
	public static function switchvideo_mpv_get_double(ctx:Pointer<CppVoid>, name:ConstCharStar):Float;

	@:native("switchvideo_mpv_get_boolean")
	public static function switchvideo_mpv_get_boolean(ctx:Pointer<CppVoid>, name:ConstCharStar):Int;

	// commands/options
	@:native("switchvideo_mpv_set_double")
	public static function switchvideo_mpv_set_double(ctx:Pointer<CppVoid>, name:ConstCharStar, value:Float):Void;

	@:native("switchvideo_mpv_loadfile")
	public static function switchvideo_mpv_loadfile(ctx:Pointer<CppVoid>, path:ConstCharStar):Int;

    @:native("switchvideo_mpv_event_id")
	public static function switchvideo_mpv_event_id(evt:Pointer<CppVoid>):Int;

	@:native("switchvideo_mpv_event_data")
	public static function switchvideo_mpv_event_data(evt:Pointer<CppVoid>):Pointer<CppVoid>;

	@:native("switchvideo_mpv_log_message_prefix")
	public static function switchvideo_mpv_log_message_prefix(data:Pointer<CppVoid>):ConstCharStar;

	@:native("switchvideo_mpv_log_message_text")
	public static function switchvideo_mpv_log_message_text(data:Pointer<CppVoid>):ConstCharStar;

	@:native("switchvideo_mpv_end_file_reason")
	public static function switchvideo_mpv_end_file_reason(data:Pointer<CppVoid>):Int;

	// render context (OpenGL ES)
	@:native("switchvideo_mpv_render_context_create")
	public static function switchvideo_mpv_render_context_create(ctx:Pointer<CppVoid>, error:Pointer<Int>):Pointer<CppVoid>;

	@:native("switchvideo_mpv_render_context_update")
	public static function switchvideo_mpv_render_context_update(ctx:Pointer<CppVoid>):Int;

	@:native("switchvideo_mpv_render_frame")
	public static function switchvideo_mpv_render_frame(ctx:Pointer<CppVoid>, fbo:Int, w:Int, h:Int):Void;

	@:native("switchvideo_mpv_render_context_free")
	public static function switchvideo_mpv_render_context_free(ctx:Pointer<CppVoid>):Void;

	// GL helper glue
	@:native("switchvideo_mpv_gl_create_fbo")
	public static function switchvideo_mpv_gl_create_fbo(fbo:Pointer<Int>, tex:Pointer<Int>, w:Int, h:Int):Void;

	@:native("switchvideo_mpv_gl_read_pixels")
	public static function switchvideo_mpv_gl_read_pixels(fbo:Int, w:Int, h:Int, out:Pointer<UInt8>):Void;

	@:native("switchvideo_mpv_gl_delete_fbo")
	public static function switchvideo_mpv_gl_delete_fbo(fbo:Int, tex:Int):Void;

	@:native("switchvideo_mpv_gl_clear_default")
	public static function switchvideo_mpv_gl_clear_default():Void;

	@:native("switchvideo_mpv_rgba_to_argb")
	public static function switchvideo_mpv_rgba_to_argb(src:Pointer<UInt8>, dst:Pointer<UInt8>, pixels:Int):Void;

	// Audio streaming glue
	@:native("switchvideo_mpv_audio_start")
	public static function switchvideo_mpv_audio_start(path:ConstCharStar):Void;

	@:native("switchvideo_mpv_audio_gate_open")
	public static function switchvideo_mpv_audio_gate_open():Void;

	@:native("switchvideo_mpv_ao_is_ready")
	public static function switchvideo_mpv_ao_is_ready():Int;

	@:native("switchvideo_mpv_ao_get_pos")
	public static function switchvideo_mpv_ao_get_pos():Float;

	@:native("switchvideo_mpv_audio_stop_func")
	public static function switchvideo_mpv_audio_stop_func():Void;

	@:native("switchvideo_mpv_ao_update")
	public static function switchvideo_mpv_ao_update():Void;

	// Logging

	@:native("switchvideo_mpv_set_log_callback")
	public static function switchvideo_mpv_set_log_callback(fn:Callable<ConstCharStar->Void>):Void;

	@:native("switchvideo_mpv_poll_pending_log")
	public static function switchvideo_mpv_poll_pending_log(out:Pointer<Char>, outSize:Int):Int;
}