# WeatherApp

A simple Android weather application with a native helper library for data parsing.

## Build

```bash
./gradlew assembleDebug
```

## Test

```bash
./gradlew connectedDebugAndroidTest
```

## Architecture

- `app/src/main/java/com/weatherapp/` — Java UI layer
- `app/src/main/cpp/` — Native helper library (`libnativehelper.so`)
- `app/src/androidTest/` — Instrumented tests
