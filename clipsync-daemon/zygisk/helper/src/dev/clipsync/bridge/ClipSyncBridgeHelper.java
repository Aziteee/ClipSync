package dev.clipsync.bridge;

import android.content.ComponentName;
import android.content.Context;
import android.content.IOnPrimaryClipChangedListener;
import android.content.pm.PackageManager;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public final class ClipSyncBridgeHelper {
    private static final String TAG = "ClipSyncBridge";

    private ClipSyncBridgeHelper() {}

    public static native void nativeOnClipboardChanged();

    public static IOnPrimaryClipChangedListener makeListener() {
        return new IOnPrimaryClipChangedListener.Stub() {
            @Override
            public void dispatchPrimaryClipChanged() {
                nativeOnClipboardChanged();
            }
        };
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
