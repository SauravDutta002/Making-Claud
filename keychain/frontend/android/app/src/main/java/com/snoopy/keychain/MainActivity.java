package com.snoopy.keychain;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        registerPlugin(NowPlayingPlugin.class);   // what's playing on the phone, from any app
        super.onCreate(savedInstanceState);
    }
}
