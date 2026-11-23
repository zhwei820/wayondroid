package com.hexadecinull.vineos.domain

// The guest's media sessions, read from `dumpsys media_session`: what other apps on the phone can show and control
// without reaching into the guest's UI.
data class GuestMediaSession(
    val packageName: String,
    val active: Boolean,
    // PlaybackState.STATE_*: 3 playing, 2 paused, 1 stopped, 0 none
    val state: Int,
    val title: String,
    val artist: String,
    val album: String,
) {
    val playing: Boolean get() = state == 3
}

object GuestMedia {
    // `cmd media_session dispatch` verbs another app may send
    val CONTROLS = setOf("play", "pause", "play-pause", "next", "previous", "stop")

    private val STATE = Regex("""state=PlaybackState \{state=(\d+)""")

    fun parse(dump: String): List<GuestMediaSession> {
        val out = ArrayList<GuestMediaSession>()
        var pkg: String? = null
        var active = false
        var state = 0
        var description: String? = null
        fun flush() {
            val p = pkg ?: return
            val (title, artist, album) = splitDescription(description)
            out += GuestMediaSession(p, active, state, title, artist, album)
        }
        for (raw in dump.lineSequence()) {
            val line = raw.trim()
            when {
                line.startsWith("package=") -> {
                    flush()
                    pkg = line.removePrefix("package=")
                    active = false
                    state = 0
                    description = null
                }
                line.startsWith("active=") -> active = line == "active=true"
                line.startsWith("state=PlaybackState") -> state = STATE.find(line)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                line.startsWith("metadata:") -> description = line.substringAfter("description=", "").ifEmpty { null }
            }
        }
        flush()
        return out
    }

    // The session another app means: the given package's, else the playing one, else any active one
    fun pick(sessions: List<GuestMediaSession>, packageName: String?): GuestMediaSession? {
        if (packageName != null) return sessions.firstOrNull { it.packageName == packageName && it.active }
        return sessions.firstOrNull { it.active && it.playing } ?: sessions.firstOrNull { it.active && it.title.isNotEmpty() }
    }

    // MediaDescription.toString(): "title, subtitle, description", i.e. title, artist and album for music apps.
    // A comma in the title is far likelier than in the album, so split from the right.
    private fun splitDescription(d: String?): Triple<String, String, String> {
        if (d == null || d == "null, null, null") return Triple("", "", "")
        val last = d.lastIndexOf(", ")
        val mid = if (last > 0) d.lastIndexOf(", ", last - 1) else -1
        fun clean(s: String) = s.trim().takeUnless { it == "null" }.orEmpty()
        return when {
            mid >= 0 -> Triple(clean(d.substring(0, mid)), clean(d.substring(mid + 2, last)), clean(d.substring(last + 2)))
            last >= 0 -> Triple(clean(d.substring(0, last)), clean(d.substring(last + 2)), "")
            else -> Triple(clean(d), "", "")
        }
    }
}
