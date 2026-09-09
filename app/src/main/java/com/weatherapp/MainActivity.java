package com.weatherapp;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("nativehelper");
    }

    public static native String getWeatherData(String location);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        String data = getWeatherData("London");
        tv.setText("Weather: " + data);
        setContentView(tv);
    }
}
