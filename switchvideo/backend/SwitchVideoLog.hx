package switchvideo.backend;

import cpp.Callable;
import cpp.Pointer;
import cpp.ConstCharStar;
import cpp.Char;

import haxe.PosInfos;
import haxe.Log;
import haxe.io.Bytes;

/**
 * Minimal debug logger for the SwitchVideo mpv video stack.
 */
class SwitchVideoLog
{
	/**
	 * The function to call to log messages.
	 * 
	 * You can override this to redirect the output to your own logger.
	 */
	public static var logCallBack:(String, ?pos:PosInfos)->Void = Log.trace;

	private static var internalPosInfos:PosInfos = null;

	private static function log(msg:String, ?pos:PosInfos):Void
	{
		#if SWITCHVIDEO_LOGGING
		if (logCallBack == null) logCallBack = Log.trace;
		logCallBack(msg, pos);
		#end
	}

	////////////////////////////////////

	private static function prepare():Void
	{
		SwitchVideoBackend.switchvideo_mpv_set_log_callback(
			Callable.fromStaticFunction(onNativeLog)
		);

		// Workaround for missing PosInfos on the C++ side
		internalPosInfos = {
			fileName: "switchvideo_mpv_native.cpp",
			lineNumber: -1,
			className: "switchvideo_mpv_native",
			methodName: "CFunction"
		}
	}

	private static function pump():Void
	{
		final buf:Bytes = Bytes.alloc(512);
		// I hate using "untyped __cpp__"... 
		final rawPtr:Pointer<Char> = untyped __cpp__("(char*){0}->getBase()", buf.getData());
		while (SwitchVideoBackend.switchvideo_mpv_poll_pending_log(rawPtr, 512) != 0)
		{
			final end:Int = buf.getData().indexOf(0, 0);
			log(buf.getString(0, end < 0 ? 512 : end));
		}
	}

	private static function onNativeLog(msg:ConstCharStar):Void
	{
		log(msg.toString(), internalPosInfos);
	}

	////////////////////////////////////
}
