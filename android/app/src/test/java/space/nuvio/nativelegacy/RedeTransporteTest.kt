package space.nuvio.nativelegacy

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class RedeTransporteTest {
    private open class ConexaoFalsa(url: URL) : HttpURLConnection(url) {
        val desconectou = AtomicBoolean()
        override fun connect() = Unit
        override fun usingProxy(): Boolean = false
        override fun disconnect() { desconectou.set(true) }
        override fun getInputStream(): InputStream = ByteArrayInputStream(byteArrayOf(1))
        override fun getResponseCode(): Int = HTTP_OK
        override fun getResponseMessage(): String = "OK"
    }

    @After fun limpar() = TaxaConexao.limparTestes()

    @Test fun aberturaRepeteTimeoutSemConfundirComCancelamento() {
        val aberturas = AtomicInteger()
        val esperas = ArrayList<Long>()
        val fonte = ParaleloDataSource(null, emptyMap(),
            novaConexao = { url ->
                object : ConexaoFalsa(url) {
                    override fun getResponseCode(): Int {
                        aberturas.incrementAndGet()
                        throw SocketTimeoutException("timeout")
                    }
                }
            }, dormir = { esperas += it })
        try {
            fonte.conectarComRetry(URL("https://retry.invalid/video.mkv"), 0, 1023, 2)
        } catch (e: FalhaDownloadParalelo) {
            assertTrue(e.cause is SocketTimeoutException)
            assertEquals("timeout", e.classeRede)
        } finally { fonte.close() }
        assertEquals(2, aberturas.get())
        assertEquals(listOf(250L), esperas)
    }

    @Test fun closeDesconectaAberturaEmVoo() {
        val entrou = CountDownLatch(1)
        val saiu = CountDownLatch(1)
        val conexao = object : ConexaoFalsa(URL("https://cancel.invalid/video.mkv")) {
            override fun getResponseCode(): Int {
                entrou.countDown()
                while (!desconectou.get()) Thread.sleep(5)
                throw IOException("desconectada")
            }
        }
        val fonte = ParaleloDataSource(null, emptyMap(), novaConexao = { conexao }, dormir = {})
        val terminou = AtomicBoolean()
        val thread = Thread {
            try { fonte.conectarComRetry(conexao.url, 0, 1023, 2) }
            catch (_: IOException) { terminou.set(true) }
            finally { saiu.countDown() }
        }
        thread.start()
        assertTrue(entrou.await(2, TimeUnit.SECONDS))
        fonte.close()
        assertTrue(saiu.await(2, TimeUnit.SECONDS))
        thread.join(200)
        assertTrue(conexao.desconectou.get())
        assertTrue(terminou.get())
        assertFalse(thread.isAlive)
    }

    @Test fun sessaoQuenteReusaMapeamentoETrechoAteOTtl() {
        var agora = 1_000L
        val sessao = SessaoParalela(ttlMs = 45_000L, relogio = { agora })
        val uri = "https://resolver.invalid/file"
        assertFalse(sessao.preparar(uri))
        sessao.configurar(URL("https://cdn.invalid/file"), 10_000)
        sessao.guardar(100, byteArrayOf(1, 2, 3, 4))
        agora += 20_000
        assertTrue(sessao.preparar(uri))
        assertEquals("cdn.invalid", sessao.destino()?.host)
        assertNotNull(sessao.trecho(102))
        agora += 46_000
        assertFalse(sessao.preparar(uri))
        assertNull(sessao.destino())
        assertNull(sessao.trecho(102))
    }

    @Test fun hostPausaDepoisDaSegundaRecusaEVoltaComBackoff() {
        var agora = 10_000L
        TaxaConexao.relogio = { agora }
        assertFalse(TaxaConexao.falhou("cdn.invalid", "refused"))
        assertTrue(TaxaConexao.falhou("cdn.invalid", "refused"))
        assertTrue(TaxaConexao.pausado("cdn.invalid"))
        agora += 10_001
        assertFalse(TaxaConexao.pausado("cdn.invalid"))
    }

    @Test fun retryAfterExternoFicaLimitadoAQuinzeSegundos() {
        var agora = 1_000L
        TaxaConexao.relogio = { agora }
        assertTrue(TaxaConexao.falhou("cdn.invalid", "http_503", 120_000L))
        try {
            TaxaConexao.adquirir("cdn.invalid")
            throw AssertionError("host deveria estar pausado")
        } catch (e: TaxaConexao.HostPausado) {
            assertEquals(15_000L, e.restanteMs)
        }
        agora += 15_001L
        assertFalse(TaxaConexao.pausado("cdn.invalid"))
    }

    @Test fun janelaRetidaNaoPassaDeDoisPedacos() {
        val sessao = SessaoParalela()
        sessao.preparar("https://resolver.invalid/file")
        sessao.configurar(URL("https://cdn.invalid/file"), 100_000)
        sessao.guardar(0, byteArrayOf(1))
        sessao.guardar(10, byteArrayOf(2))
        sessao.guardar(20, byteArrayOf(3))
        assertNull(sessao.trecho(0))
        assertNotNull(sessao.trecho(10))
        assertNotNull(sessao.trecho(20))
    }
}
