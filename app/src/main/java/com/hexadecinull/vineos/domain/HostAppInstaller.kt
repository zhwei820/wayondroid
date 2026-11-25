package com.hexadecinull.vineos.domain

import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Bitmap
import androidx.core.graphics.drawable.toBitmap
import com.hexadecinull.vineos.helper.HelperProtocol
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

// An app installed on the phone; apks are its base APK first, then its splits
data class HostApp(val packageName: String, val label: String, val versionName: String?, val apks: List<File>) {
    val totalBytes: Long get() = apks.sumOf { it.length() }
}

/**
 * Installs an app the phone already has into a running rootless guest. The guest's /data is a directory of this app,
 * so the APKs are copied into its /data/local/tmp and installed by the guest's own `pm` through one install session,
 * streaming each APK on stdin as scripts/install-qishui.sh does.
 */
@Singleton
class HostAppInstaller @Inject constructor(
    @ApplicationContext private val context: Context,
    private val provisioner: InstanceProvisioner,
    private val vm: VineVMManager,
) {
    // Launchable apps the user installed or updated; the manifest's <queries> makes them visible on Android 11+
    suspend fun listHostApps(): List<HostApp> = withContext(Dispatchers.IO) {
        val pm = context.packageManager
        val launcher = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        pm.queryIntentActivities(launcher, 0)
            .map { it.activityInfo.applicationInfo }
            .distinctBy { it.packageName }
            .filter { it.packageName != context.packageName && isUserFacing(it) }
            .mapNotNull { info ->
                val apks = (listOf(info.sourceDir) + info.splitSourceDirs.orEmpty()).map(::File)
                if (apks.any { !it.canRead() }) return@mapNotNull null
                HostApp(
                    packageName = info.packageName,
                    label = info.loadLabel(pm).toString(),
                    versionName = runCatching { pm.getPackageInfo(info.packageName, 0).versionName }.getOrNull(),
                    apks = apks,
                )
            }
            .sortedBy { it.label.lowercase() }
    }

    suspend fun loadIcon(packageName: String, sizePx: Int): Bitmap? = withContext(Dispatchers.IO) {
        runCatching { context.packageManager.getApplicationIcon(packageName).toBitmap(sizePx, sizePx) }.getOrNull()
    }

    // The guest pm's output on success; throws with the reason otherwise
    suspend fun install(instanceId: String, app: HostApp, onProgress: (String) -> Unit): String {
        require(HelperProtocol.isValidPackageName(app.packageName)) { "Bad package name ${app.packageName}" }
        if (instanceId !in vm.runningInstanceIds()) error("The instance is not running")
        if (!vm.isRootless(instanceId)) error("Installing from the phone needs a rootless instance")

        // Guest path /data/local/tmp/hostinstall-<pkg>; package names are [A-Za-z0-9_.] so it needs no shell quoting
        val guestDir = "/data/local/tmp/hostinstall-${app.packageName}"
        val hostDir = File(provisioner.instanceDir(instanceId), "rootless/data$guestDir")
        val needed = app.totalBytes
        if (provisioner.freeBytes() < needed + FREE_MARGIN) {
            error("Not enough free space: ${app.label} needs ${needed / MB} MB")
        }
        try {
            val names = withContext(Dispatchers.IO) {
                hostDir.deleteRecursively()
                if (!hostDir.mkdirs()) error("Could not create $hostDir")
                app.apks.mapIndexed { i, apk ->
                    onProgress("Copying ${app.label} (${i + 1}/${app.apks.size}, ${apk.length() / MB} MB)")
                    val name = if (i == 0) "base.apk" else "split$i.apk"
                    apk.copyTo(File(hostDir, name), overwrite = true, bufferSize = BUFFER)
                    name to apk.length()
                }
            }
            onProgress("Installing ${app.label} in the guest")
            return vm.guestExec(instanceId, INSTALL_TIMEOUT_MS, "sh", "-c", sessionScript(guestDir, names))
                .getOrElse { throw IllegalStateException("pm install failed: ${it.message}") }
                .trim()
        } finally {
            withContext(NonCancellable + Dispatchers.IO) { hostDir.deleteRecursively() }
        }
    }

    // One session for base + splits: pm installs splits only together with their base
    private fun sessionScript(guestDir: String, apks: List<Pair<String, Long>>): String = buildString {
        append("s=\$(pm install-create -r -g) || exit 1; s=\${s##*[}; s=\${s%%]*}; ")
        for ((name, size) in apks) {
            append("pm install-write -S $size \$s $name - < $guestDir/$name || { pm install-abandon \$s; exit 1; }; ")
        }
        append("pm install-commit \$s")
    }

    private fun isUserFacing(info: ApplicationInfo): Boolean =
        info.flags and ApplicationInfo.FLAG_SYSTEM == 0 || info.flags and ApplicationInfo.FLAG_UPDATED_SYSTEM_APP != 0

    private companion object {
        const val MB = 1024L * 1024
        const val FREE_MARGIN = 200 * MB
        const val BUFFER = 1024 * 1024
        const val INSTALL_TIMEOUT_MS = 5 * 60_000
    }
}
