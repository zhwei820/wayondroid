package com.hexadecinull.vineos.ui.screens

import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.outlined.Android
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.domain.HostApp
import com.hexadecinull.vineos.ui.viewmodel.HostAppsViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HostAppsScreen(instanceId: String, onBack: () -> Unit, modifier: Modifier = Modifier, vm: HostAppsViewModel = hiltViewModel()) {
    val state by vm.uiState.collectAsStateWithLifecycle()
    val snackbarHost = remember { SnackbarHostState() }
    var pending by remember { mutableStateOf<HostApp?>(null) }

    LaunchedEffect(instanceId) { vm.load(instanceId) }
    LaunchedEffect(state.message) {
        val message = state.message ?: return@LaunchedEffect
        snackbarHost.showSnackbar(message)
        vm.clearMessage()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Install from phone") },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost) },
        modifier = modifier,
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            val blocker = when {
                !state.rootless -> "Installing from the phone needs a rootless instance."
                state.status == VMStatus.BOOTING -> "Starting the instance. You can install once it has booted."
                state.status != VMStatus.RUNNING -> "Apps are installed into the running guest. Start the instance first."
                else -> null
            }
            if (blocker != null) {
                Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(blocker, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (state.rootless && state.status in listOf(VMStatus.STOPPED, VMStatus.ERROR)) {
                        Button(onClick = vm::start) { Text("Start instance") }
                    }
                }
            }
            state.progress?.let { line ->
                Row(
                    Modifier.fillMaxWidth().padding(16.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text(line, style = MaterialTheme.typography.bodyMedium)
                }
            }
            if (state.loading) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                return@Column
            }
            OutlinedTextField(
                value = state.query,
                onValueChange = vm::setQuery,
                label = { Text("Search apps") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
            val canInstall = blocker == null && state.progress == null
            LazyColumn(Modifier.fillMaxSize()) {
                items(state.visibleApps, key = { it.packageName }) { app ->
                    ListItem(
                        headlineContent = { Text(app.label) },
                        supportingContent = {
                            Text(
                                listOfNotNull(app.packageName, app.versionName, "${app.totalBytes / (1024 * 1024)} MB")
                                    .joinToString(" · "),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        },
                        leadingContent = {
                            val icon = state.icons[app.packageName]
                            if (icon != null) {
                                Image(icon.asImageBitmap(), contentDescription = null, modifier = Modifier.size(40.dp))
                            } else {
                                Icon(Icons.Outlined.Android, contentDescription = null, modifier = Modifier.size(40.dp))
                            }
                        },
                        trailingContent = { Icon(Icons.Outlined.Download, contentDescription = "Install in the guest") },
                        modifier = Modifier.clickable(enabled = canInstall) { pending = app },
                    )
                }
            }
        }
    }

    pending?.let { app ->
        AlertDialog(
            onDismissRequest = { pending = null },
            title = { Text("Install ${app.label}?") },
            text = { Text("The app is copied from this phone into the guest. Its data stays on the phone.") },
            confirmButton = {
                TextButton(onClick = {
                    pending = null
                    vm.install(app)
                }) { Text("Install") }
            },
            dismissButton = { TextButton(onClick = { pending = null }) { Text("Cancel") } },
        )
    }
}
