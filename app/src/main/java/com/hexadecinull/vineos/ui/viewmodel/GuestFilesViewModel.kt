package com.hexadecinull.vineos.ui.viewmodel

import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.data.repository.InstanceRepository
import com.hexadecinull.vineos.domain.GuestFile
import com.hexadecinull.vineos.domain.GuestFiles
import com.hexadecinull.vineos.domain.VineVMManager
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class GuestFilesUiState(
    val instanceId: String = "",
    val status: VMStatus = VMStatus.STOPPED,
    val rootless: Boolean = true,
    val path: String = "",
    val entries: List<GuestFile> = emptyList(),
    val loading: Boolean = true,
    val progress: String? = null,
    val message: String? = null,
)

@HiltViewModel
class GuestFilesViewModel @Inject constructor(
    private val files: GuestFiles,
    private val vmManager: VineVMManager,
    private val instanceRepo: InstanceRepository,
) : ViewModel() {
    private val state = MutableStateFlow(GuestFilesUiState())
    val uiState: StateFlow<GuestFilesUiState> = state.asStateFlow()
    private var statusJob: Job? = null
    private var listJob: Job? = null

    fun load(instanceId: String) {
        if (state.value.instanceId == instanceId) return
        state.update { it.copy(instanceId = instanceId, rootless = vmManager.isRootless(instanceId)) }
        statusJob?.cancel()
        statusJob = viewModelScope.launch {
            vmManager.statusFlow(instanceId).collect { status -> state.update { it.copy(status = status) } }
        }
        open("")
    }

    fun open(path: String) {
        val id = state.value.instanceId
        if (!state.value.rootless) {
            state.update { it.copy(loading = false) }
            return
        }
        state.update { it.copy(path = path, loading = true) }
        listJob?.cancel()
        listJob = viewModelScope.launch {
            val result = runCatching { files.list(id, path) }
            state.update { s ->
                result.fold(
                    onSuccess = { s.copy(entries = it, loading = false) },
                    onFailure = { s.copy(entries = emptyList(), loading = false, message = it.message) },
                )
            }
        }
    }

    // False at the storage root
    fun up(): Boolean {
        val path = state.value.path
        if (path.isEmpty()) return false
        open(path.substringBeforeLast('/', ""))
        return true
    }

    fun start() {
        viewModelScope.launch {
            val instance = instanceRepo.getById(state.value.instanceId) ?: return@launch
            vmManager.startInstance(instance)
        }
    }

    fun import(uris: List<Uri>) {
        val s = state.value
        if (uris.isEmpty() || s.progress != null) return
        state.update { it.copy(progress = "Preparing ${uris.size} file(s)") }
        viewModelScope.launch {
            val result = runCatching { files.import(s.instanceId, s.path, uris) { line -> state.update { it.copy(progress = line) } } }
            state.update {
                it.copy(
                    progress = null,
                    message = result.fold(
                        onSuccess = { names -> if (names.size == 1) "Imported ${names[0]}" else "Imported ${names.size} files" },
                        onFailure = { e -> "Import failed: ${e.message}" },
                    ),
                )
            }
            open(state.value.path)
        }
    }

    fun export(file: GuestFile, destination: Uri) {
        if (state.value.progress != null) return
        state.update { it.copy(progress = "Exporting ${file.name}") }
        viewModelScope.launch {
            val result = runCatching { files.export(state.value.instanceId, file.path, destination) }
            state.update {
                it.copy(
                    progress = null,
                    message = result.fold(onSuccess = { "Exported ${file.name}" }, onFailure = { e -> "Export failed: ${e.message}" }),
                )
            }
        }
    }

    fun clearMessage() = state.update { it.copy(message = null) }
}
