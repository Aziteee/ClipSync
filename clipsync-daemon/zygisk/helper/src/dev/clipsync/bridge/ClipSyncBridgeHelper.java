package dev.clipsync.bridge;

import android.content.ComponentName;
import android.content.Context;
import android.content.IOnPrimaryClipChangedListener;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.pm.PackageManager;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.NotificationChannel;
import android.app.PendingIntent;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import java.security.SecureRandom;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public final class ClipSyncBridgeHelper {
    private static final String TAG = "ClipSyncBridge";
    private static final String ACTION_NOTIFICATION = "dev.clipsync.NOTIF_ACTION";
    private static final String EXTRA_ACTION_ID = "action_id";
    private static final String EXTRA_NOTIF_ID = "notif_id";
    private static final String EXTRA_TOKEN = "token";
    private static final int RECEIVER_NOT_EXPORTED_FLAG = 0x4;
    private static final long sActionToken = makeActionToken();
    private static volatile boolean sReceiverRegistered = false;

    private ClipSyncBridgeHelper() {}

    public static native void nativeOnClipboardChanged();
    public static native void nativeOnNotificationAction(int actionId);

    public static IOnPrimaryClipChangedListener makeListener() {
        return new IOnPrimaryClipChangedListener.Stub() {
            @Override
            public void dispatchPrimaryClipChanged() {
                nativeOnClipboardChanged();
            }
        };
    }

    /* --- Notification action support --- */

    private static long makeActionToken() {
        long token = new SecureRandom().nextLong();
        return token != 0 ? token : 1L;
    }

    /* Build a PendingIntent that, when triggered, broadcasts only to this package
     * and carries a process-local token that external broadcasts cannot know. */
    public static PendingIntent buildActionPendingIntent(Context context,
            int notifId, int actionIndex, int actionId) {
        String packageName = context.getPackageName();
        if (packageName == null || packageName.length() == 0) {
            packageName = "android";
        }
        Intent intent = new Intent(ACTION_NOTIFICATION)
                .setPackage(packageName)
                .putExtra(EXTRA_ACTION_ID, actionId)
                .putExtra(EXTRA_NOTIF_ID, notifId)
                .putExtra(EXTRA_TOKEN, sActionToken);
        int requestCode = 0x43530000 ^ (notifId * 31) ^ (actionIndex * 131) ^ actionId;
        return PendingIntent.getBroadcast(context, requestCode, intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    /* Register the receiver that listens for ALL notification action broadcasts.
     * Idempotent; safe to call multiple times. Must be called with a valid Context
     * (e.g. system_server's Application context) before any action PendingIntent
     * is triggered. */
    public static synchronized boolean registerActionReceiver(Context context) {
        if (sReceiverRegistered) return true;
        if (context == null) return false;
        try {
            IntentFilter filter = new IntentFilter(ACTION_NOTIFICATION);
            if (Build.VERSION.SDK_INT >= 33) {
                context.registerReceiver(new NotificationActionReceiver(), filter,
                        RECEIVER_NOT_EXPORTED_FLAG);
            } else {
                context.registerReceiver(new NotificationActionReceiver(), filter);
            }
            sReceiverRegistered = true;
            Log.i(TAG, "notification action receiver registered");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "registerActionReceiver failed", t);
            return false;
        }
    }

    /* Post a parameterised notification with optional action buttons.
     * @param notifId   Android notification ID (reuse same ID to update/replace)
     * @param title     notification title (may be null)
     * @param text      notification body (may be null)
     * @param actionLabels  button labels, length == actionIds.length (may be null)
     * @param actionIds     action IDs returned in callback when button tapped (may be null)
     */
    public static boolean postNotification(Context context, int notifId,
            String title, String text, String[] actionLabels, int[] actionIds) {
        if (context == null) return false;

        NotificationManager nm = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return false;

        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(
                    "clipsync", "ClipSync", NotificationManager.IMPORTANCE_HIGH);
            nm.createNotificationChannel(channel);
        }

        String safeTitle = (title != null && title.length() > 0) ? title : "ClipSync";
        Notification.Builder builder = new Notification.Builder(context, "clipsync")
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle(safeTitle)
                .setAutoCancel(true);

        if (text != null && text.length() > 0) {
            builder.setContentText(text);
        }

        int btnCount = 0;
        if (actionLabels != null && actionIds != null) {
            int n = Math.min(actionLabels.length, actionIds.length);
            for (int i = 0; i < n && i < 10; i++) {
                if (actionLabels[i] == null) continue;
                if (btnCount == 0 && !registerActionReceiver(context)) {
                    Log.e(TAG, "notification action receiver unavailable");
                    return false;
                }
                PendingIntent pi = buildActionPendingIntent(context, notifId, i, actionIds[i]);
                if (pi != null) {
                    Notification.Action action = new Notification.Action.Builder(
                            0, actionLabels[i], pi).build();
                    builder.addAction(action);
                    btnCount++;
                }
            }
        }

        nm.notify(notifId, builder.build());
        Log.i(TAG, "notification posted: id=" + notifId
                + " title=\"" + safeTitle + "\" actions=" + btnCount);
        return true;
    }

    /* ------ original clipboard listener code (unchanged) ------ */

    static final class NotificationActionReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null || !ACTION_NOTIFICATION.equals(intent.getAction())) {
                return;
            }
            long token = intent.getLongExtra(EXTRA_TOKEN, 0L);
            if (token != sActionToken) {
                Log.e(TAG, "rejecting notification action with invalid token");
                return;
            }
            int actionId = intent.getIntExtra(EXTRA_ACTION_ID, -1);
            Log.i(TAG, "NotificationActionReceiver.onReceive action=" + intent.getAction()
                    + " action_id=" + actionId);
            if (actionId >= 0) {
                nativeOnNotificationAction(actionId);
            }
        }
    }

    public static boolean registerDirect(Object clipboardImpl,
                                         IOnPrimaryClipChangedListener listener,
                                         int userId,
                                         int deviceId) {
        if (clipboardImpl == null || listener == null) {
            return false;
        }

        try {
            Object service = getClipboardService(clipboardImpl);
            Object perUser = getPerUserState(service, userId, deviceId);
            Object callbacks = getField(perUser.getClass(), perUser, "primaryClipListeners");
            Context context = (Context) getField(service.getClass(), service, "mContext");
            PackageIdentity identity = getListenerIdentity(context, userId);
            Object cookie = newListenerInfo(service, identity.packageName, identity.uid, userId, deviceId);

            Method register = callbacks.getClass().getMethod("register", android.os.IInterface.class, Object.class);
            Boolean ok = (Boolean) register.invoke(callbacks, listener, cookie);
            return Boolean.TRUE.equals(ok);
        } catch (Throwable t) {
            Log.e(TAG, "direct listener register failed", t);
            return false;
        }
    }

    private static Object getClipboardService(Object clipboardImpl) throws Exception {
        Class<?> cls = clipboardImpl.getClass();
        if ("com.android.server.clipboard.ClipboardService".equals(cls.getName())) {
            return clipboardImpl;
        }
        return getField(cls, clipboardImpl, "this$0");
    }

    private static Object getPerUserState(Object service, int userId, int deviceId) throws Exception {
        Object clipboards = getField(service.getClass(), service, "mClipboards");
        Class<?> cls = clipboards.getClass();
        if ("android.util.SparseArray".equals(cls.getName())) {
            Method get = cls.getMethod("get", int.class);
            return requireNonNull(get.invoke(clipboards, userId), "SparseArray clipboard state");
        }

        Method get = cls.getMethod("get", int.class, Object.class);
        return requireNonNull(get.invoke(clipboards, userId, Integer.valueOf(deviceId)), "SparseArrayMap clipboard state");
    }

    private static Object newListenerInfo(Object service,
                                          String packageName,
                                          int uid,
                                          int userId,
                                          int deviceId) throws Exception {
        Class<?> serviceClass = service.getClass();
        Class<?> listenerInfoClass = null;
        try {
            listenerInfoClass = Class.forName(serviceClass.getName() + "$ListenerInfo", false, serviceClass.getClassLoader());
        } catch (ClassNotFoundException ignored) {
            for (Class<?> nested : serviceClass.getDeclaredClasses()) {
                if ("ListenerInfo".equals(nested.getSimpleName())
                        || nested.getName().endsWith("$ListenerInfo")) {
                    listenerInfoClass = nested;
                    break;
                }
            }
        }
        if (listenerInfoClass == null) {
            throw new NoSuchFieldException("ClipboardService.ListenerInfo");
        }

        for (Constructor<?> ctor : listenerInfoClass.getDeclaredConstructors()) {
            Class<?>[] types = ctor.getParameterTypes();
            ctor.setAccessible(true);
            if (types.length == 5
                    && types[0] == String.class
                    && types[1] == String.class
                    && types[2] == int.class
                    && types[3] == int.class
                    && types[4] == int.class) {
                return ctor.newInstance(packageName, null, uid, userId, deviceId);
            }
            if (types.length == 3
                    && types[0] == String.class
                    && types[1] == int.class
                    && types[2] == int.class) {
                return ctor.newInstance(packageName, uid, userId);
            }
            if (types.length == 4
                    && types[0].isAssignableFrom(serviceClass)
                    && types[1] == int.class
                    && types[2] == String.class
                    && types[3] == String.class) {
                return ctor.newInstance(service, uid, packageName, null);
            }
        }
        throw new NoSuchMethodException("ClipboardService.ListenerInfo constructor");
    }

    private static PackageIdentity getListenerIdentity(Context context, int userId) throws Exception {
        String packageName = null;
        try {
            Method getStringForUser = Settings.Secure.class.getMethod(
                    "getStringForUser",
                    android.content.ContentResolver.class,
                    String.class,
                    int.class);
            String ime = (String) getStringForUser.invoke(
                    null,
                    context.getContentResolver(),
                    Settings.Secure.DEFAULT_INPUT_METHOD,
                    userId);
            ComponentName component = ime != null ? ComponentName.unflattenFromString(ime) : null;
            packageName = component != null ? component.getPackageName() : null;
        } catch (Throwable t) {
            Log.e(TAG, "default IME lookup failed", t);
        }
        if (packageName == null || packageName.isEmpty()) {
            packageName = "android";
        }

        int uid = android.os.Process.SYSTEM_UID;
        try {
            PackageManager pm = context.getPackageManager();
            Method getPackageUidAsUser = pm.getClass().getMethod("getPackageUidAsUser", String.class, int.class);
            uid = (Integer) getPackageUidAsUser.invoke(pm, packageName, userId);
        } catch (Throwable t) {
            Log.e(TAG, "package uid lookup failed for " + packageName, t);
            packageName = "android";
            uid = android.os.Process.SYSTEM_UID;
        }

        return new PackageIdentity(packageName, uid);
    }

    private static Object getField(Class<?> cls, Object target, String name) throws Exception {
        Class<?> current = cls;
        while (current != null) {
            try {
                Field field = current.getDeclaredField(name);
                field.setAccessible(true);
                return requireNonNull(field.get(target), name);
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Object requireNonNull(Object value, String name) {
        if (value == null) {
            throw new NullPointerException(name);
        }
        return value;
    }

    private static final class PackageIdentity {
        final String packageName;
        final int uid;

        PackageIdentity(String packageName, int uid) {
            this.packageName = packageName;
            this.uid = uid;
        }
    }
}
