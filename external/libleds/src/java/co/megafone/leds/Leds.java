/*
	write by lenge.wan@megafone.co at 2017.
*/

package co.megafone.leds;

import android.util.Log;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.FileInputStream;
import java.io.IOException;

public class Leds {
	String TAG = "Leds";
	final String switchFile = "/sys/bus/i2c/drivers/aw9523b_leds/switch";
	final String triggerFile = "/sys/bus/i2c/drivers/aw9523b_leds/trigger";

	// voice led status
	public static final int VOICE_EXIT = -1;
	public static final int VOICE_LISTENING = 1;
	public static final int VOICE_ACTIVE_LISTENING = 2;
	public static final int VOICE_THINKING = 3;
	public static final int VOICE_SPEAKING = 4;
	public static final int VOICE_MIC_OFF = 5;
	public static final int VOICE_SYSTEM_ERROR = 6;
	public static final int VOICE_BLUE_BACKGROUND = 7;
	public static final int VOICE_BG_BACKGROUND = 8;

	public Leds() {
	}

	// index:[1,10], rgb:0xRRGGBB.
	public int setLed(int index, int rgb) {
		try {
			FileOutputStream fos = new FileOutputStream(switchFile);

			String str = "led" + index + "_R " + ((rgb&0xff0000)>>16);
			fos.write(str.getBytes());

			str = "led" + index + "_G " + ((rgb&0x00ff00)>>8);
			fos.write(str.getBytes());

			str = "led" + index + "_B " + (rgb&0x0000ff);
			fos.write(str.getBytes());

			fos.close();

			return 0;
		} catch (FileNotFoundException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
		return -1;
	}

	// index:[1,10], return rgb:0xRRGGBB.
	public int getLed(int index) {
		try {
			String fileName = "/sys/class/leds/aw9523b_led" + index + "_R/brightness";
			FileInputStream fin = new FileInputStream(fileName);
			byte buf_R[] = new byte[10];
			fin.read(buf_R);
			int R = 0, i = -1;
			while(buf_R[++i]>='0') R = R*10 + (buf_R[i]-'0');
			fin.close();

			fileName = "/sys/class/leds/aw9523b_led" + index + "_G/brightness";
			fin = new FileInputStream(fileName);
			byte buf_G[] = new byte[10];
			fin.read(buf_G);
			int G = 0; i = -1;
			while(buf_G[++i]>='0') G = G*10 + (buf_G[i]-'0');
			fin.close();

			fileName = "/sys/class/leds/aw9523b_led" + index + "_B/brightness";
			fin = new FileInputStream(fileName);
			byte buf_B[] = new byte[10];
			fin.read(buf_B);
			int B = 0; i = -1;
			while(buf_B[++i]>='0') B = B*10 + (buf_B[i]-'0');
			fin.close();

			return ((R<<16)|(G<<8)|B);
		} catch (FileNotFoundException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
		return -1;
	}

	// tri:"breath", etc; on:swtich; args:optional parameters.
	public int setTrigger(String tri, boolean on, String args) {
		try {
			FileOutputStream fos = new FileOutputStream(triggerFile);

			String str = tri + (on?" 1 ":" 0 ") + args;

			Log.d(TAG, str);

			fos.write(str.getBytes());

			fos.close();

			return 0;
		} catch (FileNotFoundException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
		return -1;
	}

	// status:VOICE_LISTENING,etc; index:focus led index,[1,10], optional.
	public int setVoice(int status, int index) {
		if (status == VOICE_EXIT) return setTrigger("voice", false, "");
		else return setTrigger("voice", true, status+" "+index);
	}
}
