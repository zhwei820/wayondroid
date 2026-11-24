package com.android.server.pm;

import android.content.pm.ApplicationInfo;
import android.os.Process;

import com.android.server.pm.pkg.PackageStateInternal;
import com.android.server.pm.snapshot.PackageDataSnapshot;

/**
 * Added to the guest's services.jar by dev.vine.pmpatch.Main, which points
 * ComputerEngine's call of AppsFilterSnapshot.shouldFilterApplication here:
 * apps that are not the system's do not see LineageOS's packages, which a
 * Pixel lacks. The browser and camera, the guest's only handlers of those
 * intents, are hidden from apps' queries too, but still resolve the
 * implicit intents apps start (a filtered package would resolve none).
 */
public final class VineAppsHide {
    /** UserHandle.PER_USER_RANGE */
    private static final int PER_USER_RANGE = 100000;

    private VineAppsHide() {}

    /** The caller's setting is its SettingBase (a PackageSetting or SharedUserSetting), or null */
    public static boolean shouldFilterApplication(AppsFilterSnapshot filter, PackageDataSnapshot snapshot,
            int callingUid, Object caller, PackageStateInternal target, int userId) {
        if (target != null && callingUid % PER_USER_RANGE >= Process.FIRST_APPLICATION_UID
                && !(caller instanceof SettingBase && (((SettingBase) caller).getFlags() & ApplicationInfo.FLAG_SYSTEM) != 0)
                && hidden(target.getPackageName())
                && !(HANDLERS.contains(target.getPackageName()) && startingActivity())) {
            return true;
        }
        return filter.shouldFilterApplication(snapshot, callingUid, caller, target, userId);
    }

    /** The browser and camera */
    private static final java.util.Set<String> HANDLERS =
            java.util.Set.of("org.lineageos.jelly", "org.lineageos.aperture");

    static boolean hidden(String name) {
        if (name == null) return false;
        return name.equals("lineageos.platform") || name.startsWith("org.lineageos.");
    }

    /** Whether this is the activity manager resolving an activity an app starts */
    private static boolean startingActivity() {
        for (StackTraceElement e : Thread.currentThread().getStackTrace()) {
            final String c = e.getClassName();
            if (c.equals("com.android.server.wm.ActivityStarter")
                    || c.equals("com.android.server.wm.ActivityTaskSupervisor")) {
                return true;
            }
        }
        return false;
    }
}
