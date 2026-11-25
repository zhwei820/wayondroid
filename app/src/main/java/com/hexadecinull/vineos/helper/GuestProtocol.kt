package com.hexadecinull.vineos.helper

import java.util.Locale

// pressure < 0: none known; majorMm/minorMm: the contact's axes in millimetres
data class TouchPoint(
    val id: Int,
    val x: Float,
    val y: Float,
    val pressure: Float = -1f,
    val majorMm: Float = 0f,
    val minorMm: Float = 0f,
)

// Lines for the guest agent, sent through the helper's agent_send command
object GuestProtocol {
    const val ACTION_DOWN = 0
    const val ACTION_UP = 1
    const val ACTION_MOVE = 2
    const val ACTION_CANCEL = 3
    const val ACTION_POINTER_DOWN = 5
    const val ACTION_POINTER_UP = 6

    // Coordinates are fractions of the guest screen (0..1), so the guest resolution is not needed
    // "touchx" when the points carry pressure and contact size: ID X Y PRESSURE MAJOR_MM MINOR_MM each
    fun touchLine(action: Int, actionIndex: Int, points: List<TouchPoint>): String {
        val extended = points.isNotEmpty() && points.all { it.pressure >= 0f }
        val sb = StringBuilder(if (extended) "touchx " else "touch ")
            .append(action).append(' ').append(actionIndex).append(' ').append(points.size)
        for (p in points) {
            sb.append(' ').append(p.id).append(' ')
            sb.append(String.format(Locale.ROOT, "%.4f %.4f", p.x.coerceIn(0f, 1f), p.y.coerceIn(0f, 1f)))
            if (extended) {
                sb.append(String.format(Locale.ROOT, " %.3f %.1f %.1f", p.pressure.coerceIn(0f, 1f),
                    p.majorMm.coerceAtLeast(0f), p.minorMm.coerceAtLeast(0f)))
            }
        }
        return sb.toString()
    }

    fun keyLine(keycode: Int, down: Boolean): String = "key $keycode ${if (down) 1 else 0}"

    fun textLine(text: String): String = "text " + HelperProtocol.escape(text)
}
