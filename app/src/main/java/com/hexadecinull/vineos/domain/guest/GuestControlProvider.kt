package com.hexadecinull.vineos.domain.guest

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import com.hexadecinull.vineos.domain.GuestMedia
import com.hexadecinull.vineos.domain.VineVMManager
import com.hexadecinull.vineos.domain.shortcuts.ShortcutTarget
import com.hexadecinull.vineos.helper.HelperProtocol
import dagger.hilt.EntryPoint
import dagger.hilt.InstallIn
import dagger.hilt.android.EntryPointAccessors
import dagger.hilt.components.SingletonComponent
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Lets an allowed app on the phone use a running guest's media sessions, so it can show what a guest music app plays,
 * control it, and open links in it without an accessibility service. Calls go through ContentResolver.call():
 *
 * - `status` → instanceId of the running guest, or error
 * - `nowPlaying` (extras: package) → package, title, artist, album, state, playing
 * - `control` (arg: play | pause | play-pause | next | previous | stop)
 * - `openUri` (arg: uri, extras: package) → ACTION_VIEW in the guest
 *
 * Every reply carries `ok`, and `error` when false. To show the guest's screen afterwards the caller starts
 * MainActivity with ShortcutTarget.ACTION_LAUNCH_APP and the instanceId from `status`.
 *
 * Commands run as root in the guest, so callers are checked by package name (the system verifies it against the
 * calling uid). An extra (`instance`) picks an instance; the default is the first running one.
 */
class GuestControlProvider : ContentProvider() {

    @EntryPoint
    @InstallIn(SingletonComponent::class)
    interface Deps {
        fun vmManager(): VineVMManager
    }

    private val vm: VineVMManager by lazy {
        EntryPointAccessors.fromApplication(context!!.applicationContext, Deps::class.java).vmManager()
    }

    override fun onCreate(): Boolean = true

    override fun call(method: String, arg: String?, extras: Bundle?): Bundle {
        if (callingPackage !in ALLOWED_CALLERS) throw SecurityException("$callingPackage may not control Wayondroid guests")
        val requested = extras?.getString(EXTRA_INSTANCE)
        val instanceId = requested?.takeIf { ShortcutTarget.isValidInstanceId(it) && it in vm.runningInstanceIds() }
            ?: vm.runningInstanceIds().firstOrNull()
            ?: return error("No Wayondroid instance is running")
        val pkg = extras?.getString(EXTRA_PACKAGE)?.takeIf { HelperProtocol.isValidPackageName(it) }
        return runBlocking {
            withTimeoutOrNull(CALL_TIMEOUT_MS) {
                when (method) {
                    "status" -> ok().apply { putString("instanceId", instanceId) }
                    "nowPlaying" -> nowPlaying(instanceId, pkg)
                    "control" -> control(instanceId, arg)
                    "openUri" -> openUri(instanceId, arg, pkg)
                    else -> error("Unknown method $method")
                }
            } ?: error("The guest did not answer in time")
        }
    }

    private suspend fun nowPlaying(instanceId: String, pkg: String?): Bundle {
        val dump = vm.guestExec(instanceId, 8_000, "dumpsys", "media_session").getOrElse { return error(it.message) }
        val s = GuestMedia.pick(GuestMedia.parse(dump), pkg) ?: return error("Nothing is playing")
        return ok().apply {
            putString("instanceId", instanceId)
            putString("package", s.packageName)
            putString("title", s.title)
            putString("artist", s.artist)
            putString("album", s.album)
            putInt("state", s.state)
            putBoolean("playing", s.playing)
        }
    }

    private suspend fun control(instanceId: String, action: String?): Bundle {
        if (action !in GuestMedia.CONTROLS) return error("Unknown control $action")
        vm.guestExec(instanceId, 8_000, "cmd", "media_session", "dispatch", action!!).getOrElse { return error(it.message) }
        return ok()
    }

    private suspend fun openUri(instanceId: String, uri: String?, pkg: String?): Bundle {
        // No leading '-': it would read as an option of `cmd activity start-activity`
        if (uri.isNullOrBlank() || uri.startsWith("-") || Uri.parse(uri).scheme.isNullOrEmpty()) return error("Bad uri")
        val argv = mutableListOf("cmd", "activity", "start-activity", "-a", "android.intent.action.VIEW", "-d", uri)
        if (pkg != null) argv += listOf("-p", pkg)
        val out = vm.guestExec(instanceId, 15_000, *argv.toTypedArray()).getOrElse { return error(it.message) }
        if (out.contains("Error")) return error(out.trim().take(200))
        return ok().apply { putString("instanceId", instanceId) }
    }

    private fun ok() = Bundle().apply { putBoolean("ok", true) }

    private fun error(message: String?) = Bundle().apply {
        putBoolean("ok", false)
        putString("error", message ?: "Failed")
    }

    override fun query(uri: Uri, p: Array<out String>?, s: String?, a: Array<out String>?, o: String?): Cursor? = null
    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, s: String?, a: Array<out String>?): Int = 0
    override fun update(uri: Uri, values: ContentValues?, s: String?, a: Array<out String>?): Int = 0

    companion object {
        const val EXTRA_INSTANCE = "instance"
        const val EXTRA_PACKAGE = "package"
        private const val CALL_TIMEOUT_MS = 20_000L

        // Apps that may run commands in the guest; the guest is a whole second phone, so this stays explicit
        private val ALLOWED_CALLERS = setOf("com.guozhigq.pilipala")
    }
}
