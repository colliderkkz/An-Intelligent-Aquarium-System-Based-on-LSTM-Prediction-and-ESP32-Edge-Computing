package com.fishtank.console;

import java.util.Locale;
import java.util.UUID;

/** Netlea CBBB manual-light characteristic, using the SG-130-XDb12 normal-light presets. */
final class Sg130Light {
    private static final int[][] RGBW_PRESETS = {
        // Red, white, green, blue in device order. Index 0 is off.
        {0,0,0,0}, {3,10,18,6}, {5,36,58,14}, {5,50,74,16},
        {5,60,94,23}, {5,93,123,32}, {7,100,144,39}, {12,111,167,48},
        {15,122,182,52}, {17,143,220,67}, {19,152,238,74}
    };
    private static final byte[] CHECK_BITS = {76,101,100,115,38,70,117,110};

    static UUID uuid(int suffix) {
        return UUID.fromString(String.format(Locale.ROOT, "0003cbbb-0000-1000-8000-00805f9b%04x", suffix));
    }

    static byte[] manual(int step, boolean encoded, boolean voice, int[] scale) {
        if (step < 0 || step >= RGBW_PRESETS.length) throw new IllegalArgumentException("step");
        byte[] data = new byte[voice ? 15 : 12];
        data[0] = 1;
        // On this model the lowest app setting still emits light.
        // Preserve the tested manual frame. Full power-off is on the fixture.
        data[1] = (byte)(step == 0 ? 0 : 1);
        data[2] = 1;
        data[3] = (byte)200;
        for (int i = 0; i < 4; i++) data[5+i] = (byte)((RGBW_PRESETS[step][i] * scale[i] + 50) / 100);
        data[10] = (byte)0x84;
        data[11] = 3; // restoreActTime = 900, little endian
        if (voice) data[13] = (byte)step;
        if (!encoded) return data;
        byte check = 0;
        for (byte b : data) {
            for (int bit = 0; bit < 8; bit++) if ((b & (1 << bit)) != 0) check ^= CHECK_BITS[bit];
        }
        byte[] wire = new byte[data.length + 1];
        wire[0] = check;
        for (int i = 0; i < data.length; i++) wire[i + 1] = (byte)(data[i] ^ check);
        return wire;
    }
}
