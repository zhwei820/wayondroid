package com.hexadecinull.vineos.domain

import android.content.Intent
import android.os.BatteryManager
import java.io.File

// The phone's battery as a Pixel 5's /sys/class/power_supply, which the guest's healthd reads (rootless_prep binds
// <instance>/rootless/power_supply there). healthd polls it every minute or so; nothing sends it uevents.
object GuestBattery {
    private const val DESIGN_UAH = 4_080_000 // the Pixel 5's 4080 mAh

    fun files(battery: Intent, currentMicroamps: Int): Map<String, String> {
        val level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, 50)
        val scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100).takeIf { it > 0 } ?: 100
        val capacity = (level * 100 / scale).coerceIn(0, 100)
        val plugged = battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
        val status = when (battery.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN)) {
            BatteryManager.BATTERY_STATUS_CHARGING -> "Charging"
            BatteryManager.BATTERY_STATUS_DISCHARGING -> "Discharging"
            BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "Not charging"
            BatteryManager.BATTERY_STATUS_FULL -> "Full"
            else -> "Unknown"
        }
        val health = when (battery.getIntExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_GOOD)) {
            BatteryManager.BATTERY_HEALTH_OVERHEAT -> "Overheat"
            BatteryManager.BATTERY_HEALTH_DEAD -> "Dead"
            BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE -> "Over voltage"
            BatteryManager.BATTERY_HEALTH_COLD -> "Cold"
            else -> "Good"
        }
        val voltageMv = battery.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 3850).takeIf { it > 0 } ?: 3850
        val wired = (plugged and (BatteryManager.BATTERY_PLUGGED_AC or BatteryManager.BATTERY_PLUGGED_USB)) != 0
        // healthd counts USB_PD as AC, as a Pixel on its wall charger reports
        val usbType = if ((plugged and BatteryManager.BATTERY_PLUGGED_AC) != 0) "USB_PD" else "USB"
        return mapOf(
            "battery/type" to "Battery",
            "battery/present" to "1",
            "battery/status" to status,
            "battery/health" to health,
            "battery/technology" to "Li-ion",
            "battery/capacity" to capacity.toString(),
            "battery/voltage_now" to (voltageMv * 1000).toString(),
            "battery/temp" to battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 250).toString(),
            "battery/current_now" to currentMicroamps.toString(),
            "battery/current_avg" to currentMicroamps.toString(),
            "battery/charge_counter" to (DESIGN_UAH.toLong() * capacity / 100).toString(),
            "battery/charge_full" to DESIGN_UAH.toString(),
            "battery/charge_full_design" to DESIGN_UAH.toString(),
            "battery/cycle_count" to "120",
            "usb/type" to usbType,
            "usb/online" to if (wired) "1" else "0",
            "usb/current_max" to if (wired) "3000000" else "0",
            "usb/voltage_max" to if (wired) "5000000" else "0",
            "wireless/type" to "Wireless",
            "wireless/online" to if ((plugged and BatteryManager.BATTERY_PLUGGED_WIRELESS) != 0) "1" else "0",
        )
    }

    // Into every rootless instance
    fun publish(instancesDir: File, battery: Intent, currentMicroamps: Int) {
        val files = files(battery, currentMicroamps)
        instancesDir.listFiles()?.forEach { dir ->
            if (!File(dir, "rootless").isDirectory) return@forEach
            val root = File(dir, "rootless/power_supply")
            runCatching {
                for ((path, value) in files) {
                    val f = File(root, path)
                    // Readable by healthd (user system), as sysfs is
                    for (d in listOf(root, f.parentFile!!)) {
                        d.mkdirs()
                        d.setReadable(true, false)
                        d.setExecutable(true, false)
                    }
                    if (f.isFile && runCatching { f.readText() }.getOrNull() == value + "\n") {
                        f.setReadable(true, false)
                        continue
                    }
                    val tmp = File(f.parentFile, f.name + ".tmp")
                    tmp.writeText(value + "\n")
                    tmp.setReadable(true, false)
                    tmp.renameTo(f)
                }
            }
        }
    }
}
