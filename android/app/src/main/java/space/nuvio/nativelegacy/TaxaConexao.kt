package space.nuvio.nativelegacy

import java.io.InterruptedIOException
import java.util.ArrayDeque
import java.util.concurrent.ConcurrentHashMap

/** Per-host new-connection budget and temporary refusal/back-pressure memory. */
internal object TaxaConexao {
    private const val JANELA_MS = 2_000L
    private const val RAJADA = 4
    private const val PAUSA_INICIAL_MS = 10_000L
    private const val PAUSA_MAX_MS = 60_000L

    private data class Estado(
        val aberturas: ArrayDeque<Long> = ArrayDeque(),
        var falhas: Int = 0,
        var ultimaFalha: Long = 0,
        var pausadoAte: Long = 0,
        var proximaPausa: Long = PAUSA_INICIAL_MS,
    )

    internal class HostPausado(val host: String, val restanteMs: Long) :
        InterruptedIOException("host $host paused for ${restanteMs}ms")

    private val estados = ConcurrentHashMap<String, Estado>()
    internal var relogio: () -> Long = { android.os.SystemClock.elapsedRealtime() }
    internal var dormir: (Long) -> Unit = { Thread.sleep(it) }

    fun adquirir(hostEntrada: String, cancelado: () -> Boolean = { false }) {
        val host = hostEntrada.lowercase()
        if (host.isEmpty()) return
        val estado = estados.computeIfAbsent(host) { Estado() }
        while (true) {
            if (cancelado() || Thread.currentThread().isInterrupted)
                throw InterruptedIOException("cancelled while waiting for connection budget")
            val espera = synchronized(estado) {
                val agora = relogio()
                if (estado.pausadoAte > agora) throw HostPausado(host, estado.pausadoAte - agora)
                while (estado.aberturas.isNotEmpty() && agora - estado.aberturas.first() >= JANELA_MS)
                    estado.aberturas.removeFirst()
                if (estado.aberturas.size < RAJADA) {
                    estado.aberturas.addLast(agora)
                    0L
                } else (JANELA_MS - (agora - estado.aberturas.first())).coerceAtLeast(1L)
            }
            if (espera == 0L) return
            try { dormir(espera.coerceAtMost(250L)) }
            catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                throw InterruptedIOException("cancelled while waiting for connection budget")
            }
        }
    }

    /** Pause after a confirming second refusal/timeout; 429/503 pause immediately. */
    fun falhou(hostEntrada: String, classe: String, retryAfterMs: Long = 0L): Boolean {
        val host = hostEntrada.lowercase()
        if (host.isEmpty()) return false
        val estado = estados.computeIfAbsent(host) { Estado() }
        synchronized(estado) {
            val agora = relogio()
            if (agora - estado.ultimaFalha > 30_000L) estado.falhas = 0
            estado.ultimaFalha = agora
            estado.falhas++
            val imediato = classe == "http_429" || classe == "http_503"
            if (!imediato && classe != "refused" && classe != "timeout") return false
            if (!imediato && estado.falhas < 2) return false
            // Retry-After is externally supplied; cap it at the design's 15 s.
            // Refusal/timeout pauses keep their independent 10 -> 60 s backoff.
            val pausa = if (retryAfterMs > 0) retryAfterMs.coerceAtMost(15_000L)
                        else estado.proximaPausa
            estado.pausadoAte = maxOf(estado.pausadoAte, agora + pausa)
            estado.proximaPausa = (pausa * 2).coerceIn(PAUSA_INICIAL_MS, PAUSA_MAX_MS)
            return true
        }
    }

    fun sucesso(hostEntrada: String) {
        val estado = estados[hostEntrada.lowercase()] ?: return
        synchronized(estado) { estado.falhas = 0 }
    }

    fun pausado(hostEntrada: String): Boolean {
        val estado = estados[hostEntrada.lowercase()] ?: return false
        return synchronized(estado) { estado.pausadoAte > relogio() }
    }

    internal fun limparTestes() {
        estados.clear()
        relogio = { android.os.SystemClock.elapsedRealtime() }
        dormir = { Thread.sleep(it) }
    }
}
