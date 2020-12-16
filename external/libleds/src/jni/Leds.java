package co.megafone.leds;

import android.util.Log;

public class Leds {

	static String TAG = "Leds";

	static {
		Log.d(TAG, "libleds.so jni to load.");
		System.loadLibrary("leds");
	}

	public Leds() {
		// <<Just init for native, nothing to do.
		setLed(0, 0);
		getLed(0);
		// Just init for native, nothing to do.>>
	}

	// index:[1,10], rgb:0xRRGGBB.
	public native int setLed(int index, int rgb);
	public native int getLed(int index);
}
