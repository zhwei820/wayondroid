package com.android.server.pm;
import com.android.server.pm.pkg.PackageStateInternal;
import com.android.server.pm.snapshot.PackageDataSnapshot;
public interface AppsFilterSnapshot {
    boolean shouldFilterApplication(PackageDataSnapshot snapshot, int callingUid, Object callingSetting,
            PackageStateInternal targetPkgSetting, int userId);
}
