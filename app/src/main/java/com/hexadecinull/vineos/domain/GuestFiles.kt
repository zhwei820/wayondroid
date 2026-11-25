package com.hexadecinull.vineos.domain

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

// An entry of the guest's shared storage; path is relative to it ("" is the root, "Download/a.txt")
data class GuestFile(val name: String, val path: String, val isDirectory: Boolean, val size: Long, val modified: Long)

/**
 * Files between the phone and a rootless guest's shared storage (/sdcard, the guest's /data/media/0). That is a
 * directory of this app, so files are read and written directly; the guest only fixes up what tawcroot emulates
 * (owners) and what MediaProvider indexes, which needs it running.
 */
@Singleton
class GuestFiles @Inject constructor(
    @ApplicationContext private val context: Context,
    private val provisioner: InstanceProvisioner,
    private val vm: VineVMManager,
) {
    private fun storageRoot(instanceId: String) = File(provisioner.instanceDir(instanceId), "rootless/data/media/0")

    // The file under the storage root that path names; throws if it would leave the root
    private fun resolve(instanceId: String, path: String): File {
        val root = storageRoot(instanceId).canonicalFile
        val file = File(root, path).canonicalFile
        if (file != root && !file.path.startsWith(root.path + File.separator)) throw IOException("Bad path $path")
        return file
    }

    suspend fun list(instanceId: String, path: String): List<GuestFile> = withContext(Dispatchers.IO) {
        val dir = resolve(instanceId, path)
        val entries = dir.listFiles() ?: throw IOException("Could not read ${path.ifEmpty { "the guest's storage" }}")
        entries
            .filter { !it.name.startsWith(".") }
            .map { GuestFile(it.name, childPath(path, it.name), it.isDirectory, if (it.isDirectory) 0 else it.length(), it.lastModified()) }
            .sortedWith(compareBy<GuestFile>({ !it.isDirectory }, { it.name.lowercase() }))
    }

    // Copies the picked documents into dir; the names they got in the guest
    suspend fun import(instanceId: String, dir: String, uris: List<Uri>, onProgress: (String) -> Unit): List<String> {
        if (instanceId !in vm.runningInstanceIds()) error("The instance is not running")
        if (!vm.isRootless(instanceId)) error("Files need a rootless instance")
        val target = resolve(instanceId, dir)
        if (!target.isDirectory) error("${dir.ifEmpty { "Storage" }} is not a folder")
        val docs = withContext(Dispatchers.IO) { uris.map { describe(it) } }
        val needed = docs.sumOf { it.second.coerceAtLeast(0) }
        if (provisioner.freeBytes() < needed + FREE_MARGIN) error("Not enough free space: the files need ${needed / MB} MB")

        val copied = mutableListOf<File>()
        try {
            withContext(Dispatchers.IO) {
                docs.forEachIndexed { i, (uri, _, name) ->
                    onProgress("Copying $name (${i + 1}/${docs.size})")
                    val out = uniqueFile(target, name)
                    copied += out
                    val input = context.contentResolver.openInputStream(uri) ?: throw IOException("Could not open $name")
                    input.use { src -> out.outputStream().use { src.copyTo(it, BUFFER) } }
                }
            }
        } catch (e: Exception) {
            withContext(Dispatchers.IO) { copied.forEach { it.delete() } }
            throw e
        }

        onProgress("Adding the files to the guest")
        // Owners as the folder's (files written here look like system's to the guest), readable by every app, and
        // in MediaStore for apps that only look there
        val guestDir = "/data/media/0" + (if (dir.isEmpty()) "" else "/$dir")
        val args = mutableListOf("sh", "-c", FIX_UP_SCRIPT, "sh", guestDir)
        for (f in copied) {
            args += "$guestDir/${f.name}"
            args += "file:///storage/emulated/0/" + childPath(dir, f.name).split('/').joinToString("/") { Uri.encode(it) }
        }
        vm.guestExec(instanceId, FIX_UP_TIMEOUT_MS, *args.toTypedArray())
            .getOrElse { throw IllegalStateException("The files were copied but the guest could not index them: ${it.message}") }
        return copied.map { it.name }
    }

    suspend fun export(instanceId: String, path: String, destination: Uri) = withContext(Dispatchers.IO) {
        val file = resolve(instanceId, path)
        if (!file.isFile) throw IOException("$path is not a file")
        val out = context.contentResolver.openOutputStream(destination, "wt") ?: throw IOException("Could not write the destination")
        out.use { dst -> file.inputStream().use { it.copyTo(dst, BUFFER) } }
    }

    // (uri, size or -1, a name safe for the guest's filesystem)
    private fun describe(uri: Uri): Triple<Uri, Long, String> {
        var name: String? = null
        var size = -1L
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 && !c.isNull(it) }?.let { name = c.getString(it) }
                c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !c.isNull(it) }?.let { size = c.getLong(it) }
            }
        }
        val safe = (name ?: uri.lastPathSegment ?: "file")
            .replace(Regex("[/\\x00-\\x1f]"), "_")
            .trim()
            .take(MAX_NAME)
            .ifEmpty { "file" }
            .let { if (it == "." || it == "..") "file" else it }
        return Triple(uri, size, safe)
    }

    // "name (1).ext" and so on when the folder already has name
    private fun uniqueFile(dir: File, name: String): File {
        val first = File(dir, name)
        if (first.createNewFile()) return first
        val dot = name.lastIndexOf('.').takeIf { it > 0 } ?: name.length
        for (n in 1..999) {
            val f = File(dir, "${name.substring(0, dot)} ($n)${name.substring(dot)}")
            if (f.createNewFile()) return f
        }
        throw IOException("Could not find a free name for $name")
    }

    private fun childPath(parent: String, name: String) = if (parent.isEmpty()) name else "$parent/$name"

    private companion object {
        const val MB = 1024L * 1024
        const val FREE_MARGIN = 200 * MB
        const val BUFFER = 1024 * 1024
        const val MAX_NAME = 200
        const val FIX_UP_TIMEOUT_MS = 60_000
        // $1 the folder, then pairs of (file, its file:// uri under /storage/emulated/0)
        const val FIX_UP_SCRIPT =
            "o=\$(stat -c %u:%g \"\$1\") || exit 1; shift; " +
                "while [ \$# -ge 2 ]; do chown \"\$o\" \"\$1\" && chmod 0664 \"\$1\" || exit 1; " +
                "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d \"\$2\" >/dev/null; shift 2; done"
    }
}
