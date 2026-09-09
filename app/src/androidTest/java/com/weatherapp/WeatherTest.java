package com.weatherapp;

import static org.junit.Assert.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import androidx.test.ext.junit.runners.AndroidJUnit4;

@RunWith(AndroidJUnit4.class)
public class WeatherTest {
    static {
        System.loadLibrary("nativehelper");
    }

    @Test
    public void testGetWeatherData() {
        String result = MainActivity.getWeatherData("London");
        assertNotNull(result);
        assertTrue(result.contains("London"));
    }

    @Test
    public void testMultipleCities() {
        String[] cities = {"London", "Paris", "Tokyo", "New York"};
        for (String city : cities) {
            String result = MainActivity.getWeatherData(city);
            assertNotNull(result);
            assertTrue(result.contains(city));
        }
    }
}
