package com.snoopy.keychain;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.Manifest;
import android.media.MediaMetadata;
import android.media.audiofx.Visualizer;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.PermissionState;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.annotation.Permission;
import com.getcapacitor.annotation.PermissionCallback;

import java.util.ArrayList;
import java.util.List;

/**
 * What's playing on the phone, from any app (YouTube, YouTube Music, Spotify...):
 * Android's media sessions - the same info as the lock-screen music controls.
 * Sends a "change" event to the web app whenever the song, play/pause or position
 * changes. Needs Notification access once (see MediaListener).
 */
@CapacitorPlugin(name = "NowPlaying", permissions = {
    @Permission(alias = "audio", strings = { Manifest.permission.RECORD_AUDIO, Manifest.permission.MODIFY_AUDIO_SETTINGS })
})
public class NowPlayingPlugin extends Plugin {
    private MediaSessionManager msm;
    private final List<MediaController> watched = new ArrayList<>();
    private final Handler main = new Handler(Looper.getMainLooper());
    private boolean started = false;

    private final MediaController.Callback cb = new MediaController.Callback() {
        @Override public void onMetadataChanged(MediaMetadata m) { emit(); }
        @Override public void onPlaybackStateChanged(PlaybackState s) { emit(); }
        @Override public void onSessionDestroyed() { emit(); }
    };

    @Override
    public void load() { start(); }

    @Override
    protected void handleOnResume() { start(); emit(); }   // came back from the settings screen

    private boolean granted() {
        String flat = Settings.Secure.getString(getContext().getContentResolver(), "enabled_notification_listeners");
        return flat != null && flat.contains(getContext().getPackageName());
    }

    private void start() {
        if (started || !granted()) return;
        try {
            msm = (MediaSessionManager) getContext().getSystemService(Context.MEDIA_SESSION_SERVICE);
            ComponentName me = new ComponentName(getContext(), MediaListener.class);
            msm.addOnActiveSessionsChangedListener(this::watch, me, main);
            watch(msm.getActiveSessions(me));
            started = true;
        } catch (SecurityException e) {
            started = false;
        }
    }

    private void watch(List<MediaController> list) {
        for (MediaController c : watched) c.unregisterCallback(cb);
        watched.clear();
        if (list != null) for (MediaController c : list) { c.registerCallback(cb, main); watched.add(c); }
        emit();
    }

    /** the session to show: one that's playing, else the most recent one */
    private MediaController pick() {
        for (MediaController c : watched) {
            PlaybackState s = c.getPlaybackState();
            if (s != null && s.getState() == PlaybackState.STATE_PLAYING) return c;
        }
        return watched.isEmpty() ? null : watched.get(0);
    }

    private String appName(String pkg) {
        try {
            PackageManager pm = getContext().getPackageManager();
            return pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
        } catch (Exception e) { return pkg; }
    }

    private JSObject snapshot() {
        JSObject o = new JSObject();
        o.put("granted", granted());
        MediaController c = pick();
        if (c == null) { o.put("active", false); return o; }
        MediaMetadata m = c.getMetadata();
        PlaybackState s = c.getPlaybackState();
        String title = "", artist = "";
        long dur = 0;
        if (m != null) {
            title = first(m.getString(MediaMetadata.METADATA_KEY_TITLE), m.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE));
            artist = first(m.getString(MediaMetadata.METADATA_KEY_ARTIST), m.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST),
                           m.getString(MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE));
            dur = m.getLong(MediaMetadata.METADATA_KEY_DURATION);
        }
        boolean playing = s != null && s.getState() == PlaybackState.STATE_PLAYING;
        long pos = 0;
        if (s != null) {
            pos = s.getPosition();
            if (playing && s.getLastPositionUpdateTime() > 0)
                pos += (long) ((SystemClock.elapsedRealtime() - s.getLastPositionUpdateTime()) * s.getPlaybackSpeed());
        }
        o.put("active", !title.isEmpty());
        o.put("title", title);
        o.put("artist", artist);
        o.put("playing", playing);
        o.put("position", Math.max(0, pos) / 1000.0);
        o.put("duration", Math.max(0, dur) / 1000.0);
        o.put("app", appName(c.getPackageName()));
        return o;
    }

    private static String first(String... v) {
        for (String s : v) if (s != null && !s.trim().isEmpty()) return s.trim();
        return "";
    }

    private void emit() { notifyListeners("change", snapshot()); }

    @PluginMethod
    public void current(PluginCall call) { start(); call.resolve(snapshot()); }

    /* ---- the beat: how loud the phone's music is right now, ~10x a second
       ("level" events, 0-99), like Claude Buddy's bridge does on the laptop.
       Android's Visualizer on the phone's mixed output (session 0); it needs the
       RECORD_AUDIO permission, but only reads what the phone itself plays. */
    private Visualizer vis;
    private float peak = 20;

    private boolean audioGranted() { return getPermissionState("audio") == PermissionState.GRANTED; }

    @PluginMethod
    public void beat(PluginCall call) {               // { on: true|false } -> { granted, running }
        boolean on = call.getBoolean("on", true);
        if (!on) { stopBeat(); call.resolve(beatState()); return; }
        if (!audioGranted()) { requestPermissionForAlias("audio", call, "beatPerm"); return; }
        startBeat();
        call.resolve(beatState());
    }

    @PermissionCallback
    private void beatPerm(PluginCall call) {
        if (audioGranted()) startBeat();
        call.resolve(beatState());
    }

    private JSObject beatState() {
        JSObject o = new JSObject();
        o.put("granted", audioGranted());
        o.put("running", vis != null);
        return o;
    }

    private void startBeat() {
        if (vis != null) return;
        try {
            vis = new Visualizer(0);
            vis.setEnabled(false);
            vis.setCaptureSize(Visualizer.getCaptureSizeRange()[0] < 256 ? 256 : Visualizer.getCaptureSizeRange()[0]);
            int rate = Math.min(Visualizer.getMaxCaptureRate(), 12000);   // ~12 a second
            vis.setDataCaptureListener(new Visualizer.OnDataCaptureListener() {
                @Override public void onWaveFormDataCapture(Visualizer v, byte[] w, int sr) {
                    double sum = 0;
                    for (byte b : w) { int x = (b & 0xff) - 128; sum += x * x; }
                    float rms = (float) Math.sqrt(sum / Math.max(1, w.length));
                    peak = Math.max(rms, peak * 0.995f);           // follows the volume, so quiet songs still move
                    if (peak < 6) peak = 6;
                    JSObject o = new JSObject();
                    o.put("level", Math.min(99, Math.round(rms / peak * 90)));
                    notifyListeners("level", o);
                }
                @Override public void onFftDataCapture(Visualizer v, byte[] f, int sr) {}
            }, rate, true, false);
            vis.setEnabled(true);
        } catch (Exception e) {                        // some phones don't allow it: the keychain fakes a gentle beat
            stopBeat();
        }
    }

    private void stopBeat() {
        if (vis == null) return;
        try { vis.setEnabled(false); vis.release(); } catch (Exception ignored) {}
        vis = null;
    }

    @Override
    protected void handleOnDestroy() { stopBeat(); }

    @PluginMethod
    public void openSettings(PluginCall call) {
        Intent i = new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        getContext().startActivity(i);
        call.resolve();
    }
}
