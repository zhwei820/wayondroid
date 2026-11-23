package com.hexadecinull.vineos.domain

// The rootless guest's RAM setting is vine_lmkd's budget, not an allocation: above it the guest kills its cached apps.
// The phone's own lmkd still kills the whole app when the phone runs out, so the budget stops short of the phone's RAM.
object RamSizing {
    const val MIN_MB = 512
    const val STEP_MB = 512
    private const val RESERVE_MB = 2048

    // Phone RAM minus a reserve for the phone itself, in whole steps, never below the old 4 GB cap
    fun maxMb(totalBytes: Long): Int {
        val totalMb = (totalBytes / (1024L * 1024L)).coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
        val usable = (totalMb - RESERVE_MB).coerceAtLeast(4096)
        return usable / STEP_MB * STEP_MB
    }

    fun clamp(mb: Int, maxMb: Int): Int = (mb / STEP_MB * STEP_MB).coerceIn(MIN_MB, maxMb.coerceAtLeast(MIN_MB))
}
