package switchvideo.bindings;

class CPPHelpers
{
	public static function bytesToPointer(bytes:Bytes):Pointer<UInt8> {
        if (bytes == null)
            return null;
        return Pointer.ofArray(bytes.getData());
    }

    
}