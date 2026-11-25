package com.hexadecinull.vineos

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.Build
import com.hexadecinull.vineos.domain.StarterApps
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class VineApplication : Application() {
    @Inject lateinit var starterApps: StarterApps

    override fun onCreate() {
        super.onCreate()
        createNotificationChannels()
        starterApps.start()
    }

    private fun createNotificationChannels() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_VM_RUNNING,
                    "Running VMs",
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Shown while a Wayondroid virtual machine is active"
                    setShowBadge(false)
                },
            )
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ROM_DOWNLOAD,
                    "ROM Downloads",
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = "Progress notifications for ROM image downloads"
                    setShowBadge(false)
                },
            )
        }
    }

    companion object {
        const val CHANNEL_VM_RUNNING = "vine_vm_running"
        const val CHANNEL_ROM_DOWNLOAD = "vine_rom_download"
    }
}
