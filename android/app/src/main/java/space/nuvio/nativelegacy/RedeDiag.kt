package space.nuvio.nativelegacy

import android.os.SystemClock
import android.util.Log
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.URL
import javax.net.ssl.SSLException

/** Network diagnostics deliberately log hosts only: never paths, queries or headers. */
internal object RedeDiag {
    private const val TAG = "nuvio"

    fun host(url: URL?): String = url?.host?.lowercase().orEmpty()

    fun classe(erro: Throwable, conectando: Boolean = false): String {
        var causa: Throwable? = erro
        repeat(16) {
            when (causa) {
                is SocketTimeoutException -> return "timeout"
                is ConnectException -> return "refused"
                is SSLException -> return "tls"
            }
            val texto = causa?.message.orEmpty().lowercase()
            if ("econnrefused" in texto || "ehostunreach" in texto || "connection refused" in texto)
                return "refused"
            if ("reset" in texto || "broken pipe" in texto) return "reset"
            causa = causa?.cause ?: return if (conectando) "connect" else "io"
        }
        return if (conectando) "connect" else "io"
    }

    fun abertura(modo: String, pos: Long, origem: String, quente: Boolean) =
        Log.i(TAG, "[rede] open mode=$modo pos=$pos origin_host=$origem session=${if (quente) "warm" else "cold"}")

    fun resolvida(finalHost: String, codigo: Int, ms: Long) =
        Log.i(TAG, "[rede] resolved final_host=$finalHost via=$codigo ms=$ms")

    fun conexao(host: String, ms: Long) =
        Log.i(TAG, "[rede] conn new host=$host ms_connect=$ms reused=0")

    fun falha(host: String, erro: Throwable, ms: Long, conectando: Boolean = false) =
        Log.w(TAG, "[rede] conn fail host=$host class=${classe(erro, conectando)} ms=$ms")

    fun http(host: String, codigo: Int, ms: Long) =
        Log.w(TAG, "[rede] conn fail host=$host class=http_$codigo ms=$ms")

    fun pedaco(indice: Long, host: String, erro: Throwable, tentativa: Int, max: Int) =
        Log.w(TAG, "[rede] chunk fail idx=$indice host=$host class=${classe(erro)} attempt=$tentativa/$max")

    inline fun <T> medir(bloco: () -> T): Pair<T, Long> {
        val inicio = SystemClock.elapsedRealtime()
        return bloco() to (SystemClock.elapsedRealtime() - inicio)
    }
}
