# HxSwitchVideo

A [HaxeFlixel](https://haxeflixel.com/) library for `@:native` video playback using [MPV](https://github.com/mpv-player/mpv) for the Nintendo Switch.

> [!IMPORTANT]
> This library only works with Nintendo Switch target. It is not compatible with other targets.

## Installation

Install through the haxelib (Not available yet):

```
haxelib install hxswitchvideo
```

or with git for the latest updates.

```
haxelib git hxswitchvideo https://github.com/Slushi-Github/HxSwitchVideo.git
```

Then add it to your `Project.xml` file:

```xml
<haxelib name="hxswitchvideo" if="switch" />
```

You will need the following libraries from DevKitPro:

(If you are on Linux/macOS, you will most likely need to use `sudo dkp-pacman` instead of `pacman`)

```bash
pacman -S switch-dav1d switch-ffmpeg switch-libarchive switch-libass switch-libexpat switch-libfribidi switch-libmpv switch-libplacebo switch-libzstd switch-lz4
```

## Usage

Here is a simple example:

```haxe
// Import the library
import switchvideo.SwitchVideo;

// Create a SwitchVideo object
var switchVideo = new SwitchVideo();

// Set the video to loop (Before loading the video)
switchvideo.setLooping(true);

// Load a video
switchVideo.load("path/to/video.mp4");

switchVideo.onEndReached = function() {
    // Do something when the video ends
}

// And just play the video
switchVideo.play();
```

## Custom settings

- Define `SWITCHVIDEO_LOGGING` on your `Project.xml` file to enable logging.

- You can override the `switchvideo.backend.SwitchVideoLog.logCallBack` variable, by default it uses `haxe.Log.trace` but you can change the logging behavior, for example:

    ```haxe
    switchvideo.backend.SwitchVideoLog.logCallBack = function(msg:String, ?pos:haxe.PosInfos) {
        yourLogger(msg, pos); // Add pos if your logger supports it
    }
    ```

## Credits

- [@doggywatty](https://github.com/doggywatty): For initially creating the entire integration between [MPV](https://github.com/mpv-player/mpv) and [HaxeFlixel](https://haxeflixel.com) to enable video playback on [Switch Funkin'](https://github.com/Slushi-Github/Switch-Funkin), literally did everything I didn't dare to do, heh.

- [hxvlc](https://github.com/MAJigsaw77/hxvlc): This library takes inspiration from hxvlc for mantaining a similar API.

## License

This project is released under the [MIT license](./LICENSE.md).

[MPV](https://github.com/mpv-player/mpv) is released under the [GPLv2 license](https://github.com/mpv-player/mpv/blob/master/LICENSE.LGPL).