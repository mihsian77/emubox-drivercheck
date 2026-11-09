package com.emubox.drivercheck

import android.graphics.Bitmap
import org.json.JSONObject

/** 单个渲染步骤的成败，失败时 detail 直接展示给用户定位驱动问题。 */
data class RenderStep(
    val name: String,
    val ok: Boolean,
    val detail: String,
)

/** 一次渲染实测的完整结果（图像之外的元数据）。 */
data class RenderReport(
    val ok: Boolean,
    val error: String,
    val width: Int,
    val height: Int,
    val hash: String,
    val hashRepeat: String,
    val consistent: Boolean,
    val renderMs: Double,
    val deviceName: String,
    val driverVersion: String,
    val apiVersion: String,
    val vendorId: Int,
    val deviceId: Int,
    val steps: List<RenderStep>,
    /** 换驱动时从驱动包内预加载的依赖库（绝对路径）。走系统驱动时为空。 */
    val depsLoaded: List<String> = emptyList(),
    /** 驱动 DT_NEEDED 里声明、但包内没找到的库名。 */
    val depsMissing: List<String> = emptyList(),
) {
    val passedSteps: Int get() = steps.count { it.ok }
    val failedSteps: List<RenderStep> get() = steps.filter { !it.ok }
}

/** 图案自检项：把「人眼看得懂」变成「程序判得准」。 */
data class PatternCheck(
    val name: String,
    val ok: Boolean,
    val expected: String,
    val actual: String,
)

/**
 * 自研驱动实测内核（闭源，仅 EmuBox 内部使用）。
 *
 * 做法：离屏渲染一张确定性的测试图案 → 回读像素 → 同时给人看（Bitmap）和给程序判
 * （[checkPattern] 逐区域断言）。不依赖 Surface，因此不会因窗口生命周期、旋转、
 * 前后台切换引入假失败；同一份内核也能在桌面 lavapipe 上逐像素验证。
 */
object DriverCheck {

    init {
        System.loadLibrary("drivercheck")
    }

    private external fun nativeRun(width: Int, height: Int, frames: Int, driverPath: String?): ByteArray?
    private external fun nativeReport(): String

    /**
     * 跑一次渲染实测。耗时约 10–100 ms（取决于驱动与分辨率）。
     *
     * @param driverPath 传 .so 绝对路径则绕过系统 loader 直接加载该驱动（换驱动实测）；
     *   传 null 走系统驱动。dlopen 失败时报告里的 open_driver 步骤会带 dlerror 原文。
     * @return 首位为渲染图像（失败为 null），次位为报告（永远非空）
     */
    fun run(
        width: Int = 512,
        height: Int = 512,
        frames: Int = 3,
        driverPath: String? = null,
    ): Pair<Bitmap?, RenderReport> {
        val rgba = nativeRun(width, height, frames, driverPath?.takeIf { it.isNotBlank() })
        val report = parseReport(nativeReport())
        if (rgba == null) return null to report

        val w = report.width.coerceAtLeast(1)
        val h = report.height.coerceAtLeast(1)
        val pixels = IntArray(w * h)
        var i = 0
        var p = 0
        while (i < pixels.size && p + 3 < rgba.size) {
            val r = rgba[p].toInt() and 0xFF
            val g = rgba[p + 1].toInt() and 0xFF
            val b = rgba[p + 2].toInt() and 0xFF
            val a = rgba[p + 3].toInt() and 0xFF
            pixels[i] = (a shl 24) or (r shl 16) or (g shl 8) or b
            i++
            p += 4
        }
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        bmp.setPixels(pixels, 0, w, 0, 0, w, h)
        return bmp to report
    }

    /** 对渲染结果做区域断言，返回逐项结论。 */
    fun checkPattern(bitmap: Bitmap): List<PatternCheck> {
        val w = bitmap.width
        val h = bitmap.height
        val px = IntArray(w * h)
        bitmap.getPixels(px, 0, w, 0, 0, w, h)

        fun at(x: Int, y: Int): Int = px[y.coerceIn(0, h - 1) * w + x.coerceIn(0, w - 1)]
        fun r(c: Int) = (c shr 16) and 0xFF
        fun g(c: Int) = (c shr 8) and 0xFF
        fun b(c: Int) = c and 0xFF

        val out = ArrayList<PatternCheck>(16)
        fun check(name: String, ok: Boolean, expected: String, actual: String) {
            out += PatternCheck(name, ok, expected, actual)
        }

        // 顶部三色条：通道顺序错位（BGR/RGB 颠倒）会立刻失败
        val yBar = h / 20
        val left = at(w / 6, yBar)
        val mid = at(w / 2, yBar)
        val right = at(w * 5 / 6, yBar)
        check("色条·红在左", r(left) > 240 && g(left) < 12 && b(left) < 12, "R≈255 G/B≈0",
            "R=${r(left)} G=${g(left)} B=${b(left)}")
        check("色条·绿居中", g(mid) > 240 && r(mid) < 12 && b(mid) < 12, "G≈255 R/B≈0",
            "R=${r(mid)} G=${g(mid)} B=${b(mid)}")
        check("色条·蓝在右", b(right) > 240 && r(right) < 12 && g(right) < 12, "B≈255 R/G≈0",
            "R=${r(right)} G=${g(right)} B=${b(right)}")

        // 8 级灰阶：位置在左下（翻转会跑到左上），且必须中性灰
        for (i in 0 until 8) {
            val c = at(i * 16 + 8, h * 85 / 100)
            val want = i * 255 / 7
            val neutral = r(c) == g(c) && g(c) == b(c)
            check("灰阶·第${i + 1}级", kotlin.math.abs(r(c) - want) <= 4 && neutral,
                "$want / 中性", "${r(c)}${if (neutral) "" else " 非中性(=${g(c)},${b(c)})"}")
        }

        // 纯白 / 纯黑：clamp 与范围压缩异常
        val white = at(w * 85 / 100, h * 21 / 100)
        val black = at(w * 85 / 100, h * 38 / 100)
        check("纯白块", r(white) > 250 && g(white) > 250 && b(white) > 250, "255/255/255",
            "${r(white)}/${g(white)}/${b(white)}")
        check("纯黑块", r(black) < 5 && g(black) < 5 && b(black) < 5, "0/0/0",
            "${r(black)}/${g(black)}/${b(black)}")

        return out
    }

    private fun parseReport(json: String): RenderReport {
        val o = JSONObject(json)
        val steps = ArrayList<RenderStep>()
        val arr = o.optJSONArray("steps")
        if (arr != null) {
            for (i in 0 until arr.length()) {
                val s = arr.getJSONObject(i)
                steps += RenderStep(
                    s.optString("name"),
                    s.optBoolean("ok"),
                    s.optString("detail"),
                )
            }
        }
        return RenderReport(
            ok = o.optBoolean("ok"),
            error = o.optString("error"),
            width = o.optInt("width"),
            height = o.optInt("height"),
            hash = o.optString("hash"),
            hashRepeat = o.optString("hashRepeat"),
            consistent = o.optBoolean("consistent"),
            renderMs = o.optDouble("renderMs", 0.0),
            deviceName = o.optString("deviceName"),
            driverVersion = o.optString("driverVersion"),
            apiVersion = o.optString("apiVersion"),
            vendorId = o.optInt("vendorId"),
            deviceId = o.optInt("deviceId"),
            steps = steps,
            depsLoaded = strings(o.optJSONArray("depsLoaded")),
            depsMissing = strings(o.optJSONArray("depsMissing")),
        )
    }

    private fun strings(a: org.json.JSONArray?): List<String> {
        if (a == null) return emptyList()
        return List(a.length()) { a.optString(it) }
    }
}
