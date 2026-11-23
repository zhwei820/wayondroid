package com.hexadecinull.vineos.domain

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class GuestMediaTest {
    // Trimmed from the guest's `dumpsys media_session` with 汽水音乐 playing
    private val dump = """
        Sessions Stack - have 2 sessions:
          com.android.server.telecom/HeadsetMediaButton (userId=0)
            ownerPid=1, ownerUid=1000, userId=0
            package=com.android.server.telecom
            launchIntent=null
            mediaButtonReceiver=null
            active=false
            state=null
            metadata: null
          com.luna.music/MusicPlaybackService (userId=0)
            ownerPid=2, ownerUid=10120, userId=0
            package=com.luna.music
            active=true
            state=PlaybackState {state=3, position=389, speed=1.0, actions=822, custom actions=[], error=null}
            metadata: size=8, description=小半, 陈粒, 小梦大半
    """.trimIndent()

    @Test
    fun `reads title, artist and album of each session`() {
        val sessions = GuestMedia.parse(dump)
        assertThat(sessions).hasSize(2)
        assertThat(sessions[0].packageName).isEqualTo("com.android.server.telecom")
        assertThat(sessions[0].active).isFalse()
        assertThat(sessions[0].title).isEmpty()
        val luna = sessions[1]
        assertThat(luna.packageName).isEqualTo("com.luna.music")
        assertThat(luna.active).isTrue()
        assertThat(luna.playing).isTrue()
        assertThat(luna.title).isEqualTo("小半")
        assertThat(luna.artist).isEqualTo("陈粒")
        assertThat(luna.album).isEqualTo("小梦大半")
    }

    @Test
    fun `a comma in the title stays in the title`() {
        val s = GuestMedia.parse("package=a\nactive=true\nmetadata: size=3, description=Hello, World, Someone, Album").single()
        assertThat(s.title).isEqualTo("Hello, World")
        assertThat(s.artist).isEqualTo("Someone")
        assertThat(s.album).isEqualTo("Album")
    }

    @Test
    fun `missing parts are empty, not the word null`() {
        val s = GuestMedia.parse("package=a\nactive=true\nmetadata: size=1, description=Song, null, null").single()
        assertThat(s.title).isEqualTo("Song")
        assertThat(s.artist).isEmpty()
        assertThat(s.album).isEmpty()
        assertThat(s.state).isEqualTo(0)
    }

    @Test
    fun `pick prefers the named package, else what is playing`() {
        val sessions = GuestMedia.parse(dump)
        assertThat(GuestMedia.pick(sessions, "com.luna.music")?.title).isEqualTo("小半")
        assertThat(GuestMedia.pick(sessions, null)?.packageName).isEqualTo("com.luna.music")
        assertThat(GuestMedia.pick(sessions, "com.android.server.telecom")).isNull()
    }
}
