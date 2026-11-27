package com.snoopy.keychain;

import android.service.notification.NotificationListenerService;

/**
 * Android only lets an app read other apps' "now playing" (media sessions) if
 * it has a notification listener the user switched on (Settings > Notification
 * access). This empty service is that switch; NowPlayingPlugin does the reading.
 * It never reads or keeps your notifications.
 */
public class MediaListener extends NotificationListenerService {}
