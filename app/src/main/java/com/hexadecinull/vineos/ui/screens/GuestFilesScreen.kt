package com.hexadecinull.vineos.ui.screens

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.text.format.Formatter
import android.webkit.MimeTypeMap
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContract
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material.icons.automirrored.outlined.InsertDriveFile
import androidx.compose.material.icons.outlined.FileUpload
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.SaveAlt
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.hexadecinull.vineos.data.models.VMStatus
import com.hexadecinull.vineos.domain.GuestFile
import com.hexadecinull.vineos.ui.viewmodel.GuestFilesViewModel
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GuestFilesScreen(instanceId: String, onBack: () -> Unit, modifier: Modifier = Modifier, vm: GuestFilesViewModel = hiltViewModel()) {
    val state by vm.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val snackbarHost = remember { SnackbarHostState() }
    var exporting by remember { mutableStateOf<GuestFile?>(null) }

    val pickImports = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> vm.import(uris) }
    val pickExport = rememberLauncherForActivityResult(CreateDocumentOfType()) { uri ->
        val file = exporting
        exporting = null
        if (file != null && uri != null) vm.export(file, uri)
    }

    LaunchedEffect(instanceId) { vm.load(instanceId) }
    LaunchedEffect(state.message) {
        val message = state.message ?: return@LaunchedEffect
        snackbarHost.showSnackbar(message)
        vm.clearMessage()
    }
    BackHandler(enabled = state.path.isNotEmpty()) { vm.up() }

    val canImport = state.rootless && state.status == VMStatus.RUNNING && state.progress == null && !state.loading
    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("Files")
                        Text(
                            "/sdcard" + (if (state.path.isEmpty()) "" else "/${state.path}"),
                            style = MaterialTheme.typography.bodySmall,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                },
                navigationIcon = {
                    IconButton(onClick = { if (!vm.up()) onBack() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
                },
                actions = {
                    IconButton(onClick = { pickImports.launch(arrayOf("*/*")) }, enabled = canImport) {
                        Icon(Icons.Outlined.FileUpload, contentDescription = "Import from phone")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHost) },
        modifier = modifier,
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            val notice = when {
                !state.rootless -> "Files need a rootless instance."
                state.status == VMStatus.BOOTING -> "Starting the instance. Importing works once it has booted; exporting works now."
                state.status != VMStatus.RUNNING -> "Importing needs the instance running; exporting works now."
                else -> null
            }
            if (notice != null) {
                Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(notice, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
            if (!state.rootless) return@Column
            if (state.loading) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
                return@Column
            }
            if (state.entries.isEmpty()) {
                Text(
                    "This folder is empty",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(16.dp),
                )
            }
            LazyColumn(Modifier.fillMaxSize()) {
                items(state.entries, key = { it.path }) { entry ->
                    ListItem(
                        headlineContent = { Text(entry.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                        supportingContent = { Text(describe(context, entry), style = MaterialTheme.typography.bodySmall) },
                        leadingContent = {
                            Icon(
                                if (entry.isDirectory) Icons.Outlined.Folder else Icons.AutoMirrored.Outlined.InsertDriveFile,
                                contentDescription = null,
                            )
                        },
                        trailingContent = if (entry.isDirectory) null else {
                            { Icon(Icons.Outlined.SaveAlt, contentDescription = "Export to phone") }
                        },
                        modifier = Modifier.clickable(enabled = state.progress == null) {
                            if (entry.isDirectory) {
                                vm.open(entry.path)
                            } else {
                                exporting = entry
                                pickExport.launch(entry.name to mimeOf(entry.name))
                            }
                        },
                    )
                }
            }
        }
    }
}

private fun describe(context: Context, entry: GuestFile): String {
    val date = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(entry.modified))
    return if (entry.isDirectory) date else "${Formatter.formatShortFileSize(context, entry.size)} · $date"
}

private fun mimeOf(name: String): String =
    MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase()) ?: "application/octet-stream"

// ACTION_CREATE_DOCUMENT with the type chosen per launch: (suggested name, mime type)
private class CreateDocumentOfType : ActivityResultContract<Pair<String, String>, Uri?>() {
    override fun createIntent(context: Context, input: Pair<String, String>): Intent =
        Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(input.second)
            .putExtra(Intent.EXTRA_TITLE, input.first)

    override fun parseResult(resultCode: Int, intent: Intent?): Uri? = intent?.data.takeIf { resultCode == Activity.RESULT_OK }
}
