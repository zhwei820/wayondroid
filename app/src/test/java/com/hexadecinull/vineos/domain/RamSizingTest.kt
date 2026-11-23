package com.hexadecinull.vineos.domain

import com.google.common.truth.Truth.assertThat
import org.junit.Test

class RamSizingTest {
    private val mb = 1024L * 1024L

    @Test
    fun `limit is the phone RAM minus a reserve, in whole steps`() {
        // A 12 GB phone reports a little less than 12 GB as MemTotal
        assertThat(RamSizing.maxMb(11_200 * mb)).isEqualTo(8_704)
        assertThat(RamSizing.maxMb(16_384 * mb)).isEqualTo(14_336)
    }

    @Test
    fun `small phones keep the old 4 GB limit`() {
        assertThat(RamSizing.maxMb(3_800 * mb)).isEqualTo(4_096)
        assertThat(RamSizing.maxMb(0)).isEqualTo(4_096)
    }

    @Test
    fun `huge totals do not overflow`() {
        assertThat(RamSizing.maxMb(Long.MAX_VALUE)).isGreaterThan(0)
    }

    @Test
    fun `clamp snaps to a step and stays in range`() {
        assertThat(RamSizing.clamp(3_000, 8_192)).isEqualTo(2_560)
        assertThat(RamSizing.clamp(100, 8_192)).isEqualTo(RamSizing.MIN_MB)
        assertThat(RamSizing.clamp(20_000, 8_192)).isEqualTo(8_192)
    }
}
