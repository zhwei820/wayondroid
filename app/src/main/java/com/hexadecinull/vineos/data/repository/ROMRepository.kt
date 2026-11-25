package com.hexadecinull.vineos.data.repository

import android.content.Context
import android.net.Uri
import android.os.StatFs
import com.hexadecinull.vineos.data.models.DownloadProgress
import com.hexadecinull.vineos.data.models.ROMDownloadState
import com.hexadecinull.vineos.data.models.ROMImage
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request

@Singleton
class ROMRepository @Inject constructor(@ApplicationContext private val context: Context, private val httpClient: OkHttpClient) {
    private val romsDir = File(context.filesDir, "roms").also { it.mkdirs() }
    private val localRomsFile = File(romsDir, "local_roms.json")
    private val json = Json { ignoreUnknownKeys = true }
    private val verifyCache = ConcurrentHashMap<String, Boolean>()

    private val _manifestRoms = MutableStateFlow(BUILTIN_ROMS.map(::withLocalState))
    private val _localRoms = MutableStateFlow(loadLocalRoms())
    val roms: Flow<List<ROMImage>> = combine(_manifestRoms, _localRoms) { manifest, local -> manifest + local }

    private val _downloadProgress = MutableStateFlow<Map<String, DownloadProgress>>(emptyMap())
    val downloadProgress: Flow<Map<String, DownloadProgress>> = _downloadProgress.asStateFlow()

    suspend fun fetchManifest(): Result<List<ROMImage>> = withContext(Dispatchers.IO) {
        runCatching {
            val request = Request.Builder().url(MANIFEST_URL).build()
            val body = httpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) error("HTTP ${response.code}")
                response.body.string()
            }
            val manifest = json.decodeFromString<ROMManifest>(body)
            val enriched = (BUILTIN_ROMS + manifest.roms).map(::withLocalState)
            _manifestRoms.value = enriched
            enriched
        }
    }

    private fun fileOf(rom: ROMImage): File = File(romsDir, "${rom.id}.vrom")

    // A built-in ROM's file is packed here and renamed into place whole, so it is valid when present
    private fun withLocalState(rom: ROMImage): ROMImage {
        val localFile = fileOf(rom)
        val present = localFile.exists()
        val valid = present && (rom.sha256.isEmpty() || verifyFile(localFile, rom.sha256))
        return rom.copy(
            localPath = if (present) localFile.absolutePath else null,
            isDownloaded = valid,
            downloadState = when {
                valid -> ROMDownloadState.READY
                present -> ROMDownloadState.CORRUPTED
                else -> ROMDownloadState.NOT_DOWNLOADED
            },
        )
    }

    suspend fun download(rom: ROMImage, onProgress: (DownloadProgress) -> Unit): Result<File> {
        val ota = rom.downloadUrl.removePrefix(WAYDROID_OTA)
        if (ota != rom.downloadUrl) return downloadWaydroid(rom, ota)
        return downloadVrom(rom, onProgress)
    }

    private suspend fun downloadVrom(rom: ROMImage, onProgress: (DownloadProgress) -> Unit): Result<File> = withContext(Dispatchers.IO) {
        val dest = fileOf(rom)
        val part = File(dest.path + ".part")
        runCatching {
            val free = StatFs(romsDir.path).availableBytes
            if (rom.sizeBytes > 0 && free < rom.sizeBytes + MIN_FREE_BYTES) {
                error("Not enough free storage. This ROM needs ${rom.sizeBytes / MB} MB and ${free / MB} MB are free.")
            }
            updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.DOWNLOADING)

            val digest = MessageDigest.getInstance("SHA-256")
            val request = Request.Builder().url(rom.downloadUrl).build()
            httpClient.newCall(request).execute().use { response ->
                if (!response.isSuccessful) error("HTTP ${response.code}")
                val body = response.body
                val totalBytes = body.contentLength().takeIf { it > 0 } ?: rom.sizeBytes

                part.outputStream().buffered(BUFFER_SIZE).use { out ->
                    body.byteStream().buffered(BUFFER_SIZE).use { input ->
                        var downloaded = 0L
                        var lastReportMs = 0L
                        val buf = ByteArray(BUFFER_SIZE)
                        var read: Int
                        while (input.read(buf).also { read = it } != -1) {
                            out.write(buf, 0, read)
                            digest.update(buf, 0, read)
                            downloaded += read
                            val now = System.currentTimeMillis()
                            if (now - lastReportMs >= PROGRESS_INTERVAL_MS) {
                                lastReportMs = now
                                updateProgress(rom.id, downloaded, totalBytes, ROMDownloadState.DOWNLOADING)
                                onProgress(DownloadProgress(rom.id, downloaded, totalBytes, ROMDownloadState.DOWNLOADING))
                            }
                        }
                    }
                }
            }

            updateProgress(rom.id, rom.sizeBytes, rom.sizeBytes, ROMDownloadState.VERIFYING)
            val actual = digest.digest().joinToString("") { "%02x".format(it) }
            if (!actual.equals(rom.sha256, ignoreCase = true)) {
                part.delete()
                updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.CORRUPTED)
                error("SHA-256 verification failed for ${rom.id}")
            }
            dest.delete()
            if (!part.renameTo(dest)) error("Could not save the downloaded ROM")

            updateProgress(rom.id, rom.sizeBytes, rom.sizeBytes, ROMDownloadState.READY)
            refreshROM(rom, dest)
            dest
        }.onFailure {
            part.delete()
            if (_downloadProgress.value[rom.id]?.state != ROMDownloadState.CORRUPTED) {
                updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.NOT_DOWNLOADED)
            }
        }
    }

    // False when the file is still on disk; the ROM stays listed so the user can retry
    suspend fun delete(rom: ROMImage): Boolean = withContext(Dispatchers.IO) {
        val file = if (rom.isLocal) rom.localPath?.let(::File) else fileOf(rom)
        if (file != null && file.exists() && !file.delete() && file.exists()) return@withContext false
        if (rom.isLocal) {
            _localRoms.value = _localRoms.value.filterNot { it.id == rom.id }
            saveLocalRoms()
        } else {
            refreshROM(rom, null)
        }
        true
    }

    // Copies a picked .vrom into app storage, reads its embedded manifest.json for metadata, and registers it alongside the manifest-fetched ROMs
    suspend fun importLocalRom(sourceUri: Uri): Result<ROMImage> = withContext(Dispatchers.IO) {
        runCatching {
            val id = "local-${UUID.randomUUID()}"
            val dest = File(romsDir, "$id.vrom")

            val digest = MessageDigest.getInstance("SHA-256")
            context.contentResolver.openInputStream(sourceUri)?.use { input ->
                dest.outputStream().buffered(BUFFER_SIZE).use { output ->
                    val buf = ByteArray(BUFFER_SIZE)
                    var read: Int
                    while (input.read(buf).also { read = it } != -1) {
                        output.write(buf, 0, read)
                        digest.update(buf, 0, read)
                    }
                }
            } ?: error("Could not open the selected file")

            val entry = runCatching { readVromManifest(dest) }.getOrNull()
                ?: run {
                    dest.delete()
                    error("Not a valid .vrom file (missing or unreadable manifest.json)")
                }

            val rom = ROMImage(
                id = id,
                displayName = entry.displayName,
                androidVersion = entry.androidVersion,
                apiLevel = entry.apiLevel,
                description = "Imported from device storage",
                downloadUrl = "",
                sha256 = digest.digest().joinToString("") { "%02x".format(it) },
                sizeBytes = dest.length(),
                supportedAbis = entry.supportedAbis,
                has32BitSupport = entry.has32BitSupport,
                releaseDate = entry.releaseDate,
                isLocal = true,
                localPath = dest.absolutePath,
                isDownloaded = true,
                downloadState = ROMDownloadState.READY,
            )

            _localRoms.value = _localRoms.value + rom
            saveLocalRoms()
            rom
        }
    }

    private fun readVromManifest(vromFile: File): VromManifestEntry {
        ZipFile(vromFile).use { zip ->
            val entry = zip.getEntry("manifest.json") ?: error("manifest.json not found in archive")
            val text = zip.getInputStream(entry).bufferedReader().use { it.readText() }
            return json.decodeFromString(text)
        }
    }

    private fun loadLocalRoms(): List<ROMImage> {
        if (!localRomsFile.exists()) return emptyList()
        return runCatching { json.decodeFromString<List<ROMImage>>(localRomsFile.readText()) }.getOrDefault(emptyList())
    }

    private fun saveLocalRoms() {
        runCatching { localRomsFile.writeText(json.encodeToString(_localRoms.value)) }
    }

    fun getROMFile(romId: String): File? {
        val f = getRom(romId)?.let(::fileOf) ?: File(romsDir, "$romId.vrom")
        return if (f.exists()) f else null
    }

    fun getRom(romId: String): ROMImage? = (_manifestRoms.value + _localRoms.value).find { it.id == romId }

    // Waydroid's newest arm64-only system image of this type and its MAINLINE vendor image, from their OTA
    // listings, unpacked and repacked on the phone as a waydroid-kind .vrom (what rom/package_vrom.py makes)
    private suspend fun downloadWaydroid(rom: ROMImage, systemType: String): Result<File> = withContext(Dispatchers.IO) {
        val dest = fileOf(rom)
        val part = File(dest.path + ".part")
        val zips = listOf(File(romsDir, "${dest.nameWithoutExtension}.system.zip"), File(romsDir, "${dest.nameWithoutExtension}.vendor.zip"))
        if (!activeDownloads.add(dest.path)) return@withContext Result.failure(IllegalStateException("Already downloading"))
        runCatching {
            val images = listOf(
                newestOtaImage("$WAYDROID_OTA_BASE/system/lineage/waydroid_arm64_only/$systemType.json"),
                newestOtaImage("$WAYDROID_OTA_BASE/vendor/waydroid_arm64_only/MAINLINE.json"),
            )
            val zipBytes = images.sumOf { it.size }
            // The zips, the images packed from them, and the instance unpacked later
            val free = StatFs(romsDir.path).availableBytes
            if (free < zipBytes * 4 + MIN_FREE_BYTES) {
                error("Not enough free storage. This ROM needs about ${zipBytes * 4 / MB} MB and ${free / MB} MB are free.")
            }
            var done = 0L
            var lastReportMs = 0L
            updateProgress(rom.id, 0L, zipBytes, ROMDownloadState.DOWNLOADING)
            for ((image, zip) in images.zip(zips)) {
                if (zip.length() == image.size && hashFile(zip).equals(image.sha256, ignoreCase = true)) {
                    done += image.size
                    continue
                }
                val digest = MessageDigest.getInstance("SHA-256")
                httpClient.newCall(Request.Builder().url(image.url).build()).execute().use { response ->
                    if (!response.isSuccessful) error("HTTP ${response.code} for ${image.filename}")
                    zip.outputStream().buffered(BUFFER_SIZE).use { out ->
                        response.body.byteStream().use { input ->
                            val buf = ByteArray(BUFFER_SIZE)
                            while (true) {
                                val read = input.read(buf)
                                if (read < 0) break
                                out.write(buf, 0, read)
                                digest.update(buf, 0, read)
                                done += read
                                val now = System.currentTimeMillis()
                                if (now - lastReportMs >= PROGRESS_INTERVAL_MS) {
                                    lastReportMs = now
                                    updateProgress(rom.id, done, zipBytes, ROMDownloadState.DOWNLOADING)
                                }
                            }
                        }
                    }
                }
                val actual = digest.digest().joinToString("") { "%02x".format(it) }
                if (!actual.equals(image.sha256, ignoreCase = true)) {
                    zip.delete()
                    error("SHA-256 verification failed for ${image.filename}")
                }
            }

            updateProgress(rom.id, zipBytes, zipBytes, ROMDownloadState.VERIFYING)
            val sha256 = linkedMapOf<String, String>()
            ZipOutputStream(part.outputStream().buffered(BUFFER_SIZE)).use { out ->
                out.setLevel(Deflater.NO_COMPRESSION)
                for (zip in zips) {
                    ZipFile(zip).use { zf ->
                        val entry = zf.entries().asSequence().firstOrNull { it.name == "system.img" || it.name == "vendor.img" }
                            ?: error("${zip.name} has no system.img or vendor.img")
                        val digest = MessageDigest.getInstance("SHA-256")
                        out.putNextEntry(ZipEntry(entry.name))
                        zf.getInputStream(entry).use { input ->
                            val buf = ByteArray(BUFFER_SIZE)
                            while (true) {
                                val read = input.read(buf)
                                if (read < 0) break
                                out.write(buf, 0, read)
                                digest.update(buf, 0, read)
                            }
                        }
                        out.closeEntry()
                        sha256[entry.name] = digest.digest().joinToString("") { "%02x".format(it) }
                    }
                }
                val manifest = buildJsonObject {
                    put("kind", "waydroid")
                    put("displayName", rom.displayName)
                    put("androidVersion", rom.androidVersion)
                    put("apiLevel", rom.apiLevel)
                    put("description", images.joinToString(" + ") { it.filename })
                    put("supportedAbis", JsonArray(rom.supportedAbis.map(::JsonPrimitive)))
                    put("sha256", JsonObject(sha256.mapValues { JsonPrimitive(it.value) }))
                }
                out.putNextEntry(ZipEntry("manifest.json"))
                out.write((manifest.toString() + "\n").toByteArray())
                out.closeEntry()
            }
            dest.delete()
            if (!part.renameTo(dest)) error("Could not save the downloaded ROM")
            zips.forEach(File::delete)
            updateProgress(rom.id, zipBytes, zipBytes, ROMDownloadState.READY)
            refreshROM(rom, dest)
            dest
        }.onFailure {
            part.delete()
            updateProgress(rom.id, 0L, rom.sizeBytes, ROMDownloadState.NOT_DOWNLOADED)
        }.also { activeDownloads.remove(dest.path) }
    }

    private data class OtaImage(val filename: String, val url: String, val sha256: String, val size: Long, val datetime: Long)

    private fun newestOtaImage(listing: String): OtaImage {
        val body = httpClient.newCall(Request.Builder().url(listing).build()).execute().use { response ->
            if (!response.isSuccessful) error("HTTP ${response.code} for $listing")
            response.body.string()
        }
        return json.parseToJsonElement(body).jsonObject["response"]!!.jsonArray.map { it.jsonObject }.map {
            OtaImage(
                filename = it["filename"]!!.jsonPrimitive.content,
                url = it["url"]!!.jsonPrimitive.content,
                sha256 = it["id"]!!.jsonPrimitive.content,
                size = it["size"]!!.jsonPrimitive.long,
                datetime = it["datetime"]!!.jsonPrimitive.long,
            )
        }.maxByOrNull { it.datetime } ?: error("No images listed in $listing")
    }

    private fun hashFile(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered(BUFFER_SIZE).use { stream ->
            val buf = ByteArray(BUFFER_SIZE)
            var read: Int
            while (stream.read(buf).also { read = it } != -1) digest.update(buf, 0, read)
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    // Hashing a multi-GB ROM takes seconds, so the answer is remembered until the file or the expected hash changes
    private fun verifyFile(file: File, expectedSha256: String): Boolean {
        if (!file.exists()) return false
        val key = "${file.absolutePath}|${file.length()}|${file.lastModified()}|$expectedSha256"
        return verifyCache.getOrPut(key) { hashFile(file).equals(expectedSha256, ignoreCase = true) }
    }

    private fun updateProgress(romId: String, downloaded: Long, total: Long, state: ROMDownloadState) {
        _downloadProgress.value = _downloadProgress.value + (romId to DownloadProgress(romId, downloaded, total, state))
    }

    private fun refreshROM(changed: ROMImage, localFile: File?) {
        _manifestRoms.value = _manifestRoms.value.map { rom ->
            if (rom.id == changed.id) {
                rom.copy(
                    localPath = localFile?.absolutePath,
                    isDownloaded = localFile != null,
                    downloadState = if (localFile != null) ROMDownloadState.READY else ROMDownloadState.NOT_DOWNLOADED,
                )
            } else {
                rom
            }
        }
    }

    @Serializable
    private data class VromManifestEntry(
        val displayName: String,
        val androidVersion: String,
        val apiLevel: Int,
        val supportedAbis: List<String>,
        val has32BitSupport: Boolean = false,
        val releaseDate: String = "",
    )

    companion object {
        // downloadUrl of a ROM built from Waydroid's OTA: the prefix, then the system image type (VANILLA, GAPPS)
        private const val WAYDROID_OTA = "waydroid-ota:"
        private const val WAYDROID_OTA_BASE = "https://ota.waydro.id"
        private val activeDownloads = ConcurrentHashMap.newKeySet<String>()

        // Waydroid LineageOS 20 presented as a Pixel 5, with Google apps or without
        private val BUILTIN_ROMS = listOf("GAPPS" to "Pixel 5 (GMS)", "VANILLA" to "Pixel 5 (Vanilla)").map { (type, name) ->
            ROMImage(
                id = "waydroid-lineage20-${type.lowercase()}",
                displayName = name,
                androidVersion = "13",
                apiLevel = 33,
                description = if (type == "GAPPS") {
                    "Waydroid LineageOS 20 with Google apps (GMS), presented as a Pixel 5"
                } else {
                    "Waydroid LineageOS 20 without Google apps, presented as a Pixel 5"
                },
                downloadUrl = WAYDROID_OTA + type,
                sha256 = "",
                sizeBytes = 0L,
                supportedAbis = listOf("arm64-v8a"),
                has32BitSupport = false,
                releaseDate = "",
            )
        }

        private const val MANIFEST_URL = "https://vineos.hexadecinull.dpdns.org/roms/manifest.json"
        private const val BUFFER_SIZE = 64 * 1024
        private const val PROGRESS_INTERVAL_MS = 250L
        private const val MB = 1024L * 1024L
        private const val MIN_FREE_BYTES = 256L * MB
    }
}
