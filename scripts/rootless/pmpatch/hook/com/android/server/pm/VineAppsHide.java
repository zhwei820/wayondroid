package com.android.server.pm;

import android.content.pm.ApplicationInfo;
import android.os.Process;

import com.android.server.pm.pkg.PackageStateInternal;
import com.android.server.pm.snapshot.PackageDataSnapshot;

/**
 * Added to the guest's services.jar by dev.vine.pmpatch.Main, which points
 * ComputerEngine's call of AppsFilterSnapshot.shouldFilterApplication here:
 * apps that are not the system's do not see LineageOS's packages, which a
 * Pixel lacks. The browser and camera stay: they are the guest's only
 * handlers of those intents, and a filtered package resolves no intent.
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
                && hidden(target.getPackageName())) {
            return true;
        }
        return filter.shouldFilterApplication(snapshot, callingUid, caller, target, userId);
    }

    static boolean hidden(String name) {
        if (name == null) return false;
        if (name.equals("lineageos.platform")) return true;
        return name.startsWith("org.lineageos.") && !name.equals("org.lineageos.jelly")
                && !name.equals("org.lineageos.aperture");
    }
}
