package com.hexadecinull.vineos.domain

import java.io.File
import java.security.MessageDigest
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.OkHttpClient
import okhttp3.Request

// GrapheneOS's own apps from its app repository, for a guest presenting GrapheneOS: rootless prep installs
// <instance>/system-apps/<dir>/ as /system/product/app/<dir>/. Vanadium is left out: its builds need Android 14.
@Singleton
class GrapheneApps @Inject constructor(private val httpClient: OkHttpClient) {
    suspend fun download(dest: File, onProgress: (String) -> Unit) = withContext(Dispatchers.IO) {
        val text = get(METADATA_URL).use { it.body.string() }
        // The repository's .sjson: the JSON, then its signature
        val packages = Json.parseToJsonElement(text.substring(0, text.lastIndexOf('}') + 1)).jsonObject["packages"]!!.jsonObject
        for ((pkg, dir) in APPS) {
            ensureActive()
            onProgress("Downloading GrapheneOS app $dir")
            val (version, variant) = packages[pkg]?.jsonObject?.get("variants")?.jsonObject.orEmpty()
                .mapValues { it.value.jsonObject }
                .filter { (_, v) -> usable(v) }
                .maxByOrNull { it.key.toLong() }
                ?: error("No Android $GUEST_SDK build of $pkg in the GrapheneOS app repository")
            val sha256 = variant["apkHashes"]!!.jsonArray[0].jsonPrimitive.content
            val target = File(dest, "$dir/$dir.apk")
            if (target.isFile && sha256Of(target).equals(sha256, ignoreCase = true)) continue
            target.parentFile!!.mkdirs()
            val part = File(target.path + ".part")
            get("$BASE_URL/packages/$pkg/$version/base.apk").use { response ->
                part.outputStream().use { out -> response.body.byteStream().use { it.copyTo(out, BUFFER) } }
            }
            if (!sha256Of(part).equals(sha256, ignoreCase = true)) {
                part.delete()
                error("$pkg $version failed SHA-256 verification")
            }
            if (!part.renameTo(target)) error("Could not save $pkg")
        }
    }

    private fun usable(v: JsonObject): Boolean {
        val channel = v["channel"]?.jsonPrimitive?.content ?: "stable"
        val minSdk = v["minSdk"]?.jsonPrimitive?.intOrNull ?: 0
        val abis = v["abis"]?.jsonArray?.map { it.jsonPrimitive.content }
        val apks = v["apks"]?.jsonArray?.map { it.jsonPrimitive.content }
        return channel == "stable" && minSdk <= GUEST_SDK && (abis == null || "arm64-v8a" in abis) && apks == listOf("base.apk")
    }

    private fun get(url: String) = httpClient.newCall(Request.Builder().url(url).build()).execute().also {
        if (!it.isSuccessful) {
            it.close()
            error("HTTP ${it.code} for $url")
        }
    }

    private fun sha256Of(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buf = ByteArray(BUFFER)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                digest.update(buf, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    companion object {
        private const val BASE_URL = "https://apps.grapheneos.org"
        private const val METADATA_URL = "$BASE_URL/metadata.1.0.sjson"
        private const val GUEST_SDK = 33
        private const val BUFFER = 64 * 1024

        // Package to its /system/product/app/<dir> (dir names unverified against a GrapheneOS image)
        private val APPS = listOf(
            "app.grapheneos.camera" to "GrapheneOSCamera",
            "app.grapheneos.apps" to "AppStore",
            "app.attestation.auditor" to "Auditor",
            "app.grapheneos.pdfviewer" to "PdfViewer",
            "app.grapheneos.info" to "GrapheneOSInfo",
        )
    }
}
