package com.emubox.drivercheck.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.emubox.drivercheck.PatternCheck
import com.emubox.drivercheck.RenderReport

/** 抽屉内的界面状态。 */
sealed interface DriverCheckState {
    data object Idle : DriverCheckState
    data object Running : DriverCheckState
    data class Done(
        val image: android.graphics.Bitmap?,
        val report: RenderReport,
        val checks: List<PatternCheck>,
    ) : DriverCheckState
    data class Failed(val message: String) : DriverCheckState
}

/**
 * D 方案：底部抽屉承载主体，徽章常驻在画面内。
 * H 方案：首卡是「参考对照」——左边真渲染、右边标准答案，用户不用记图案含义。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DriverCheckSheet(
    state: DriverCheckState,
    onRun: () -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .padding(bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Header(state, onRun)
            when (state) {
                is DriverCheckState.Done -> {
                    RenderCard(state)
                    ReferenceCard(state)
                    StepsCard(state)
                }
                is DriverCheckState.Failed -> Text(
                    state.message,
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium,
                )
                else -> Unit
            }
        }
    }
}

@Composable
private fun Header(state: DriverCheckState, onRun: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text("驱动渲染实测", style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold)
            val sub = when (state) {
                is DriverCheckState.Done -> buildString {
                    append(state.report.deviceName.ifBlank { "未知设备" })
                    append(" · ")
                    append(state.report.apiVersion)
                }
                is DriverCheckState.Running -> "渲染中…"
                else -> "离屏渲染 + 像素回读，不依赖窗口"
            }
            Text(sub, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
        }
        TextButton(onClick = onRun, enabled = state !is DriverCheckState.Running) {
            Text(if (state is DriverCheckState.Idle) "开始" else "重测")
        }
    }
}

@Composable
private fun RenderCard(state: DriverCheckState.Done) {
    val r = state.report
    val passed = state.checks.count { it.ok }
    val total = state.checks.size
    val allOk = r.ok && r.consistent && passed == total
    val badgeColor = when {
        !r.ok -> Color(0xFFB3261E)
        allOk -> Color(0xFF1B6E3C)
        else -> Color(0xFF8A5A00)
    }
    val badgeText = when {
        !r.ok -> "渲染失败"
        !r.consistent -> "帧间不一致"
        passed < total -> "图案异常 ${total - passed} 项"
        else -> "全部通过"
    }

    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp)),
    ) {
        if (state.image != null) {
            Image(
                bitmap = state.image.asImageBitmap(),
                contentDescription = "驱动渲染测试图案",
                modifier = Modifier.fillMaxWidth(),
                contentScale = ContentScale.Fit,
            )
        } else {
            Box(Modifier.fillMaxWidth().height(180.dp)) {
                CircularProgressIndicator(Modifier.align(Alignment.Center))
            }
        }
        // 徽章常驻画面内（D 方案的关键：不滚动也能看到结论）
        Row(
            Modifier
                .align(Alignment.TopStart)
                .padding(8.dp)
                .clip(RoundedCornerShape(6.dp))
                .background(badgeColor)
                .padding(horizontal = 8.dp, vertical = 3.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(badgeText, color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.Medium)
        }
        Row(
            Modifier
                .align(Alignment.BottomEnd)
                .padding(8.dp)
                .clip(RoundedCornerShape(6.dp))
                .background(Color(0xCC000000))
                .padding(horizontal = 8.dp, vertical = 3.dp),
        ) {
            Text(
                "${passed}/${total} 项 · ${"%.0f".format(r.renderMs)} ms/帧",
                color = Color.White, fontSize = 11.sp,
            )
        }
    }

    // 一致性横条：每帧一个色块，全同则整条绿
    Row(Modifier.fillMaxWidth().height(6.dp)) {
        repeat(3) {
            Box(
                Modifier
                    .weight(1f)
                    .height(6.dp)
                    .background(if (r.consistent) Color(0xFF1B6E3C) else Color(0xFFB3261E)),
            )
            if (it < 2) Spacer(Modifier.width(2.dp))
        }
    }
    Text(
        "指纹 ${r.hash.take(12)} · 重复帧 ${if (r.consistent) "一致" else "不一致"}",
        style = MaterialTheme.typography.labelSmall,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/** H 方案：参考对照——渲染图 + 每个区域「应该长什么样」，异常项标红。 */
@Composable
private fun ReferenceCard(state: DriverCheckState.Done) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f))
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text("参考对照", style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.SemiBold)
        state.checks.forEach { c ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(6.dp)
                        .clip(RoundedCornerShape(3.dp))
                        .background(if (c.ok) Color(0xFF1B6E3C) else Color(0xFFB3261E)),
                )
                Spacer(Modifier.width(8.dp))
                Text(c.name, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
                Text(
                    if (c.ok) "应 ${c.expected}" else "应 ${c.expected} · 实际 ${c.actual}",
                    style = MaterialTheme.typography.labelSmall,
                    color = if (c.ok) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.error,
                    maxLines = 1,
                )
            }
        }
    }
}

@Composable
private fun StepsCard(state: DriverCheckState.Done) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(12.dp))
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text("执行步骤", style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.SemiBold)
        state.report.steps.forEach { s ->
            Row {
                Text(
                    if (s.ok) "✓" else "✕",
                    color = if (s.ok) Color(0xFF1B6E3C) else Color(0xFFB3261E),
                    fontSize = 12.sp,
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    "${s.name}  ${s.detail}",
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
