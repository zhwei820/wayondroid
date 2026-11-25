package com.hexadecinull.vineos.ui.viewmodel

import android.graphics.Bitmap
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.domain.HostApp
import com.hexadecinull.vineos.domain.HostAppInstaller
import com.hexadecinull.vineos.domain.VineVMManager
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class HostAppsUiState(
    val instanceId: String = "",
    val status: VMStatus = VMStatus.STOPPED,
    val rootless: Boolean = true,
    val apps: List<HostApp> = emptyList(),
    val icons: Map<String, Bitmap> = emptyMap(),
    val loading: Boolean = true,
    val query: String = "",
    val progress: String? = null,
    val message: String? = null,
) {
    val visibleApps: List<HostApp>
        get() = if (query.isBlank()) {
            apps
        } else {
            apps.filter { it.label.contains(query, ignoreCase = true) || it.packageName.contains(query, ignoreCase = true) }
        }
}

@HiltViewModel
class HostAppsViewModel @Inject constructor(
    private val installer: HostAppInstaller,
    private val vmManager: VineVMManager,
) : ViewModel() {
    private val state = MutableStateFlow(HostAppsUiState())
    val uiState: StateFlow<HostAppsUiState> = state.asStateFlow()
    private var statusJob: Job? = null

    fun load(instanceId: String) {
        if (state.value.instanceId == instanceId) return
        state.update { it.copy(instanceId = instanceId, rootless = vmManager.isRootless(instanceId)) }
        statusJob?.cancel()
        statusJob = viewModelScope.launch {
            vmManager.statusFlow(instanceId).collect { status -> state.update { it.copy(status = status) } }
        }
        viewModelScope.launch {
            val apps = runCatching { installer.listHostApps() }.getOrElse {
                state.update { s -> s.copy(loading = false, message = "Could not list the phone's apps: ${it.message}") }
                return@launch
            }
            state.update { it.copy(apps = apps, loading = false) }
            for (app in apps) {
                val icon = installer.loadIcon(app.packageName, ICON_PX) ?: continue
                state.update { it.copy(icons = it.icons + (app.packageName to icon)) }
            }
        }
    }

    fun setQuery(query: String) = state.update { it.copy(query = query) }

    fun install(app: HostApp) {
        val s = state.value
        if (s.progress != null) return
        state.update { it.copy(progress = "Preparing ${app.label}") }
        viewModelScope.launch {
            val result = runCatching {
                installer.install(s.instanceId, app) { line -> state.update { it.copy(progress = line) } }
            }
            state.update {
                it.copy(
                    progress = null,
                    message = result.fold(
                        onSuccess = { "${app.label} installed in the guest" },
                        onFailure = { e -> "${app.label}: ${e.message}" },
                    ),
                )
            }
        }
    }

    fun clearMessage() = state.update { it.copy(message = null) }

    private companion object {
        const val ICON_PX = 96
    }
}
