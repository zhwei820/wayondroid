package com.hexadecinull.vineos.domain

import android.content.Context
import android.util.Log
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.InstanceRepository
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * Installs the apps a new instance starts with, on its first boot that gets them all in: APKPure and Edge (the
 * default browser in place of Chrome, which is disabled), copied from the phone when it has them or else downloaded,
 * and FlClash, from the phone only (the guest's proxy, kept running by rootless_prep's proxy keeper).
 * A new instance carries InstanceProvisioner.STARTER_APPS_PENDING until then; a failed one is tried again next boot.
 */
@Singleton
class StarterApps @Inject constructor(
    @ApplicationContext private val context: Context,
    private val instanceRepo: InstanceRepository,
    private val provisioner: InstanceProvisioner,
    private val vm: VineVMManager,
    private val installer: HostAppInstaller,
) {
    // afterInstall: a guest shell command run once it is installed
    private class Starter(val packageName: String, val label: String, val urls: List<String>, val afterInstall: String? = null)

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    private val watched = ConcurrentHashMap.newKeySet<String>()
    private val installing = Mutex()

    fun start() {
        scope.launch {
            instanceRepo.observeAll().collect { list ->
                for (instance in list) if (watched.add(instance.id)) scope.launch { watch(instance.id) }
            }
        }
    }

    private suspend fun watch(instanceId: String) {
        vm.statusFlow(instanceId).collect { status ->
            if (status == VMStatus.RUNNING && pending(instanceId).exists()) installAll(instanceId)
        }
    }

    private fun pending(instanceId: String) =
        File(provisioner.instanceDir(instanceId), InstanceProvisioner.STARTER_APPS_PENDING)

    private suspend fun installAll(instanceId: String) = installing.withLock {
        if (!pending(instanceId).exists() || !vm.isRootless(instanceId) || !packageManagerReady(instanceId)) return
        var allIn = true
        for (app in STARTERS) {
            runCatching { installOne(instanceId, app) }.onFailure {
                allIn = false
                Log.w(TAG, "${app.label} not installed in $instanceId: ${it.message}")
            }
        }
        if (allIn) pending(instanceId).delete()
    }

    // The guest reports running at boot_completed; pm answers then, but give it a minute
    private suspend fun packageManagerReady(instanceId: String): Boolean {
        repeat(30) {
            if (vm.guestExec(instanceId, 10_000, "pm", "path", "android").isSuccess) return true
            if (vm.statusFlow(instanceId).value != VMStatus.RUNNING) return false
            delay(2_000)
        }
        return false
    }

    private suspend fun installOne(instanceId: String, app: Starter) {
        if (vm.guestExec(instanceId, 10_000, "pm", "path", app.packageName).isSuccess) return
        // One only the phone can give is skipped when it hasn't it
        val source = fromPhone(app) ?: (if (app.urls.isEmpty()) return else download(app))
            ?: error("not on the phone and every download failed")
        Log.i(TAG, "installing ${app.label} in $instanceId: " + installer.install(instanceId, source) {})
        app.afterInstall?.let { cmd ->
            vm.guestExec(instanceId, 30_000, "sh", "-c", cmd).onFailure { Log.w(TAG, "${app.label} setup: ${it.message}") }
        }
    }

    private fun fromPhone(app: Starter): HostApp? = runCatching {
        val info = context.packageManager.getApplicationInfo(app.packageName, 0)
        val apks = (listOf(info.sourceDir) + info.splitSourceDirs.orEmpty()).map(::File)
        if (apks.all { it.canRead() }) HostApp(app.packageName, app.label, null, apks) else null
    }.getOrNull()

    // Kept in the cache for the next instance
    private fun download(app: Starter): HostApp? {
        val apk = File(File(context.cacheDir, "starter-apps").apply { mkdirs() }, "${app.packageName}.apk")
        if (!isApk(apk)) {
            val tmp = File(apk.path + ".part")
            for (url in app.urls) {
                val ok = runCatching { fetch(url, tmp) }.onFailure { Log.w(TAG, "download $url: ${it.message}") }.isSuccess
                if (ok && isApk(tmp) && tmp.renameTo(apk)) break
            }
            tmp.delete()
        }
        return if (isApk(apk)) HostApp(app.packageName, app.label, null, listOf(apk)) else null
    }

    private fun fetch(url: String, to: File) {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 30_000
        conn.readTimeout = 60_000
        conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Linux; Android 13)")
        try {
            if (conn.responseCode != HttpURLConnection.HTTP_OK) error("HTTP ${conn.responseCode}")
            conn.inputStream.use { input -> to.outputStream().use { input.copyTo(it, 256 * 1024) } }
        } finally {
            conn.disconnect()
        }
    }

    private fun isApk(f: File): Boolean = f.length() > 4 && f.inputStream().use { it.read() == 'P'.code && it.read() == 'K'.code }

    private companion object {
        const val TAG = "StarterApps"

        // d.apkpure.com answers a script challenge (403) to plain clients; .net serves the APK
        val STARTERS = listOf(
            Starter(
                "com.apkpure.aegon",
                "APKPure",
                listOf(
                    "https://d.apkpure.net/b/APK/com.apkpure.aegon?version=latest",
                    "https://d.apkpure.com/b/APK/com.apkpure.aegon?version=latest",
                ),
            ),
            // A universal APK (every ABI), about 260 MB
            Starter(
                "com.microsoft.emmx",
                "Edge",
                listOf(
                    "https://d.apkpure.net/b/APK/com.microsoft.emmx?version=latest",
                    "https://d.apkpure.com/b/APK/com.microsoft.emmx?version=latest",
                ),
                afterInstall = "cmd role add-role-holder --user 0 android.app.role.BROWSER com.microsoft.emmx && " +
                    "pm disable-user --user 0 com.android.chrome",
            ),
            Starter("com.follow.clash", "FlClash", emptyList()),
        )
    }
}
