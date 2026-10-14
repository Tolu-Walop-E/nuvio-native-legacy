package space.nuvio.nativelegacy

import java.net.URL
import java.util.LinkedHashMap

/** Small per-player warm-session cache retained across Media3 seek close/open cycles. */
internal class SessaoParalela(
    private val ttlMs: Long = 45_000L,
    // 2 x 16 MB: retain at most the Stage 4 heap budget of 32 MB.
    private val maxPedacos: Int = 2,
    private val relogio: () -> Long = { android.os.SystemClock.elapsedRealtime() },
) {
    data class Trecho(val inicio: Long, val dados: ByteArray) {
        val fim: Long get() = inicio + dados.size
    }

    private var original: String? = null
    private var final: URL? = null
    private var total: Long = -1
    private var usadaEm = 0L
    private val trechos = object : LinkedHashMap<Long, ByteArray>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, ByteArray>?): Boolean =
            size > maxPedacos
    }

    @Synchronized fun preparar(chave: String): Boolean {
        val agora = relogio()
        val quente = original == chave && final != null && total > 0 && agora - usadaEm <= ttlMs
        if (!quente) {
            original = chave
            final = null
            total = -1
            trechos.clear()
        }
        usadaEm = agora
        return quente
    }

    @Synchronized fun configurar(url: URL, tamanho: Long) {
        final = url
        total = tamanho
        usadaEm = relogio()
    }

    @Synchronized fun destino(): URL? = final
    @Synchronized fun tamanho(): Long = total

    @Synchronized fun guardar(inicio: Long, dados: ByteArray) {
        if (dados.isNotEmpty()) trechos[inicio] = dados
        usadaEm = relogio()
    }

    @Synchronized fun trecho(posicao: Long): Trecho? {
        val achado = trechos.entries.firstOrNull { posicao >= it.key && posicao < it.key + it.value.size }
            ?: return null
        usadaEm = relogio()
        return Trecho(achado.key, achado.value)
    }

    @Synchronized fun invalidarDestino() {
        final = null
        total = -1
        trechos.clear()
    }
}
