/*
	write by lenge.wan@megafone.co at 2017.
*/

#define LOG_TAG "Leds"

#include "jni.h"
#include "JNIHelp.h"

#include <utils/misc.h>
#include <utils/Log.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

namespace android {

#define switch_file	"/sys/bus/i2c/drivers/aw9523b_leds/switch"
static FILE *switchFile = NULL;
#define trigger_file	"/sys/bus/i2c/drivers/aw9523b_leds/trigger"
//static FILE *triggerFile = NULL;
static char matchStr[256];

static int set_led(JNIEnv* env , jclass clazz, jint index, jint rgb)
{
	jint ret = -1;

	if (index <= 0 || index >10) return ret;

	if (!switchFile)
		switchFile = fopen(switch_file, "r+");
	if (!switchFile) {
		ALOGE("fopen error(%s).\n", strerror(errno));
		return ret;
	}

	errno = 0;

	snprintf(matchStr, sizeof(matchStr), "led%d_R %d \n", index, (rgb&0xff0000)>>16);
	fwrite(matchStr, 1, strlen(matchStr), switchFile);
	fflush(switchFile);
	snprintf(matchStr, sizeof(matchStr), "led%d_G %d \n", index, (rgb&0x00ff00)>>8);
	fwrite(matchStr, 1, strlen(matchStr), switchFile);
	fflush(switchFile);
	snprintf(matchStr, sizeof(matchStr), "led%d_B %d \n", index, (rgb&0x0000ff));
	fwrite(matchStr, 1, strlen(matchStr), switchFile);
	fflush(switchFile);

	return 0;
}

static jint get_led(JNIEnv* env, jclass clazz, jint index)
{
	FILE *f_R = NULL;
	jint R = 0;
	FILE *f_G = NULL;
	jint G = 0;
	FILE *f_B = NULL;
	jint B = 0;
	jint ret = -1;

	if (index <= 0 || index >10) return ret;

	snprintf(matchStr, sizeof(matchStr), "/sys/class/leds/aw9523b_led%d_R/brightness", index);
	f_R = fopen(matchStr, "r+");
	if (!f_R) {
		ALOGE("fopen error(%s).\n", strerror(errno));
		return ret;
	}
	fread(matchStr, 4, 1, f_R);
	R = atoi(matchStr);

	snprintf(matchStr, sizeof(matchStr), "/sys/class/leds/aw9523b_led%d_G/brightness", index);
	f_G = fopen(matchStr, "r+");
	if (!f_G) {
		ALOGE("fopen error(%s).\n", strerror(errno));
		return ret;
	}
	fread(matchStr, 4, 1, f_G);
	G = atoi(matchStr);

	snprintf(matchStr, sizeof(matchStr), "/sys/class/leds/aw9523b_led%d_B/brightness", index);
	f_B = fopen(matchStr, "r+");
	if (!f_B) {
		ALOGE("fopen error(%s).\n", strerror(errno));
		return ret;
	}
	fread(matchStr, 4, 1, f_B);
	B = atoi(matchStr);

	return (jint)((R<<16)|(G<<8)|(B));
}

static const JNINativeMethod method_table[] = {
	{ "setLed", "(II)I", (void*)set_led},
	{ "getLed", "(I)I", (void*)get_led},
};

int register_co_megafone_leds_Leds(JNIEnv *env)
{
	ALOGW("Leds native methods to register.\n");
	return jniRegisterNativeMethods(env, "co/megafone/leds/Leds",
            method_table, NELEM(method_table));
}

}
