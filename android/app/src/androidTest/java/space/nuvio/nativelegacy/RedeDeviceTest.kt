package space.nuvio.nativelegacy

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.media3.common.C
import androidx.media3.datasource.DataSource
import androidx.media3.datasource.DataSpec
import androidx.media3.datasource.DefaultHttpDataSource
import android.net.Uri
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import java.io.IOException

/** Small on-device transport benchmark. It must only target the local fault server. */
@RunWith(AndroidJUnit4::class)
class RedeDeviceTest {
    private fun fixture(): String {
        val value = InstrumentationRegistry.getArguments().getString("url")
            ?: error("pass -e url http://LAN-IP:port/file/large.mkv")
        val lan = Regex("""^http://(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)""")
        check(lan.containsMatchIn(value)) {
            "device benchmark accepts private-LAN HTTP URLs only"
        }
        return value
    }

    private data class Resultado(val bytes: Long, val ms: Long) {
        val mbps: Double get() = if (ms == 0L) 0.0 else bytes * 8.0 / ms / 1000.0
    }

    private fun ler(factory: DataSource.Factory, url: String, pos: Long, bytes: Int): Resultado {
        val source = factory.createDataSource()
        val spec = DataSpec.Builder().setUri(Uri.parse(url)).setPosition(pos)
            .setLength(bytes.toLong()).build()
        val buffer = ByteArray(256 * 1024)
        var total = 0L
        val inicio = android.os.SystemClock.elapsedRealtime()
        try {
            source.open(spec)
            while (total < bytes) {
                val n = source.read(buffer, 0, minOf(buffer.size.toLong(), bytes - total).toInt())
                if (n == C.RESULT_END_OF_INPUT) break
                total += n
            }
        } finally {
            source.close()
        }
        return Resultado(total, android.os.SystemClock.elapsedRealtime() - inicio)
    }

    @Test fun singleAndParallelThroughput() {
        val url = fixture()
        val single = DefaultHttpDataSource.Factory().setConnectTimeoutMs(3_500)
            .setReadTimeoutMs(20_000).setAllowCrossProtocolRedirects(true)
        val parallel = ParaleloDataSource.Factory(null, emptyMap(), single)
        val bytes = 32 * 1024 * 1024
        val a = ler(single, url, 0, bytes)
        val b = ler(parallel, url, 0, bytes)
        Log.i("REDE_BENCH", "single bytes=${a.bytes} ms=${a.ms} mbps=${"%.1f".format(a.mbps)}")
        Log.i("REDE_BENCH", "parallel bytes=${b.bytes} ms=${b.ms} mbps=${"%.1f".format(b.mbps)}")
        assertEquals(bytes.toLong(), a.bytes)
        assertEquals(bytes.toLong(), b.bytes)
    }

    @Test fun warmBackwardSeek() {
        val url = fixture()
        val single = DefaultHttpDataSource.Factory().setConnectTimeoutMs(3_500)
            .setReadTimeoutMs(20_000).setAllowCrossProtocolRedirects(true)
        val parallel = ParaleloDataSource.Factory(null, emptyMap(), single)
        val first = ler(parallel, url, 0, 1024 * 1024)
        val warm = ler(parallel, url, 512 * 1024L, 256 * 1024)
        Log.i("REDE_BENCH", "warm-seek first_ms=${first.ms} warm_ms=${warm.ms} bytes=${warm.bytes}")
        assertEquals(256L * 1024L, warm.bytes)
    }

    @Test fun refusalSurfacesWithinTenSeconds() {
        val fixture = Uri.parse(fixture())
        val refused = fixture.buildUpon().encodedAuthority("${fixture.host}:18766").build()
        val single = DefaultHttpDataSource.Factory().setConnectTimeoutMs(3_500)
            .setReadTimeoutMs(20_000).setAllowCrossProtocolRedirects(true)
        val source = ParaleloDataSource.Factory(null, emptyMap(), single).createDataSource()
        val started = android.os.SystemClock.elapsedRealtime()
        var failure: IOException? = null
        try {
            source.open(DataSpec.Builder().setUri(refused).setLength(1024).build())
        } catch (e: IOException) {
            failure = e
        } finally {
            source.close()
            TaxaConexao.limparTestes()
        }
        val ms = android.os.SystemClock.elapsedRealtime() - started
        Log.i("REDE_BENCH", "refused ms=$ms class=${failure?.javaClass?.simpleName}")
        check(failure is FalhaDownloadParalelo) { "expected bounded parallel failure, got $failure" }
        check(ms <= 10_000) { "refusal took ${ms}ms" }
    }
}
