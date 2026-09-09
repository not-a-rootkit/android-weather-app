#include <jni.h>
#include <string.h>
#include <stdlib.h>

// v2.1.3: Optimized string formatting with locale-aware temperature conversion
static const char* format_temp(double celsius, int use_fahrenheit) {
    static char temp_buf[32];
    if (use_fahrenheit) {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°F", celsius * 9.0 / 5.0 + 32.0);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", celsius);
    }
    return temp_buf;
}

JNIEXPORT jstring JNICALL
Java_com_weatherapp_MainActivity_getWeatherData(JNIEnv *env, jclass cls, jstring location) {
    const char *loc = (*env)->GetStringUTFChars(env, location, NULL);
    const char *temp = format_temp(22.0, 0);
    char buf[256];
    snprintf(buf, sizeof(buf), "Sunny, %s in %s", temp, loc);
    (*env)->ReleaseStringUTFChars(env, location, loc);
    return (*env)->NewStringUTF(env, buf);
}
