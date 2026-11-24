package com.hexadecinull.vineos.ui.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hexadecinull.vineos.data.models.AbiCompat
import com.hexadecinull.vineos.data.models.DownloadProgress
import com.hexadecinull.vineos.data.models.ROMImage
import com.hexadecinull.vineos.data.repository.ROMRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class ROMDetailUiState(
    val rom: ROMImage? = null,
    val progress: DownloadProgress? = null,
    val runMode: AbiCompat.RunMode = AbiCompat.RunMode.UNAVAILABLE,
    val isLoading: Boolean = true,
    val deleted: Boolean = false,
    val deleteError: String? = null,
)

private data class DetailFlags(val isLoading: Boolean = true, val deleted: Boolean = false, val deleteError: String? = null)

@HiltViewModel
class ROMDetailViewModel @Inject constructor(private val romRepo: ROMRepository) : ViewModel() {
    private val romIdFlow = MutableStateFlow("")
    private val flags = MutableStateFlow(DetailFlags())

    // No grace period on uiState: sources are already-cached hot flows, so a rebuild on resubscribe is cheap
    val uiState: StateFlow<ROMDetailUiState> = combine(
        romIdFlow,
        romRepo.roms,
        romRepo.downloadProgress,
        flags,
    ) { romId, roms, progress, f ->
        val rom = roms.find { it.id == romId }
        ROMDetailUiState(
            rom = rom,
            progress = progress[romId],
            runMode = rom?.let { AbiCompat.romRunMode(it) } ?: AbiCompat.RunMode.UNAVAILABLE,
            isLoading = f.isLoading,
            deleted = f.deleted,
            deleteError = f.deleteError,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(), ROMDetailUiState())

    fun load(id: String) {
        romIdFlow.value = id
        if (romRepo.getRom(id) != null) {
            flags.value = flags.value.copy(isLoading = false)
            return
        }
        viewModelScope.launch {
            try {
                romRepo.fetchManifest()
            } finally {
                flags.value = flags.value.copy(isLoading = false)
            }
        }
    }

    fun download() {
        val rom = romRepo.getRom(romIdFlow.value) ?: return
        viewModelScope.launch { romRepo.download(rom, onProgress = {}) }
    }

    fun delete() {
        val rom = romRepo.getRom(romIdFlow.value) ?: return
        viewModelScope.launch {
            flags.value = if (romRepo.delete(rom)) {
                flags.value.copy(deleted = true, deleteError = null)
            } else {
                flags.value.copy(deleteError = "Could not delete ${rom.displayName}")
            }
        }
    }

    fun clearDeleteError() {
        flags.value = flags.value.copy(deleteError = null)
    }
}
