#include <jni.h>
#include <string.h>

JNIEXPORT jstring JNICALL
Java_com_weatherapp_MainActivity_getWeatherData(JNIEnv *env, jclass cls, jstring location) {
    const char *loc = (*env)->GetStringUTFChars(env, location, NULL);
    char buf[256];
    snprintf(buf, sizeof(buf), "Sunny, 22°C in %s", loc);
    (*env)->ReleaseStringUTFChars(env, location, loc);
    return (*env)->NewStringUTF(env, buf);
}
