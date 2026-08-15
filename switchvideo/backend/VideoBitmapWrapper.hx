package switchvideo.backend;

/**
 * Wrapper object to maintain compatibility with hxvlc's bitmap API
 */
class VideoBitmapWrapper
{
	public var rate(default, set):Float = 1.0;

	private var owner:SwitchVideo;

	public function new(owner:SwitchVideo)
	{
		this.owner = owner;
	}

	private function set_rate(value:Float):Float
	{
		owner?.setRate(value);
		return this.rate = value;
	}
}