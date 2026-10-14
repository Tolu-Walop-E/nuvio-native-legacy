// Simkl como fonte da retomada e destino do "+". Ver simkl.h: la estao as
// rotas (todas conferidas na documentacao oficial), o que ficou de fora e o
// modelo de fios.
#include "simkl.h"
#include "simklauth.h"
#include "nuvem.h"
#include "rede.h"
#include "js.h"
#include "idioma.h"
#include "trakt.h"
#include "traktscrobble.h"
#include "descoberta.h"
#include "artemetahub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define SMK_API "https://api.simkl.com"
// Retomada: o mesmo teto da fileira (CONT_MAX de descoberta.c) com folga para
// o "a seguir" disputar lugar com os pausados.
#define SMK_PLAY_MAX SIMKL_CONTINUAR_MAX
// Plan to Watch conhecido, para a guarda do "-". 400 ids x 24 B = ~10 KB; a
// lista de "quero ver" de uma pessoa real cabe com sobra.
#define SMK_PTW_MAX 400
#define SMK_ANIME_BUSCAS_MAX 24
#define SMK_ANIME_CACHE_MAX 24
#define SMK_ANIME_CORPO_MAX (512u * 1024u)
#define SMK_ANIME_TTL (7 * 24 * 60 * 60)
#define SMK_ANIME_ERRO_TTL 30

int simkl_ativo(void) { return simklauth_token()[0] != 0; }

const char *simkl_aviso_sem_vinculo(int querSimkl) {
  return (querSimkl && !simkl_ativo()) ? SIMKL_VINCULE : NULL;
}

// ------------------------------------------------------------ JSON por nivel

// Valor da chave `chave` na PROFUNDIDADE 1 do primeiro objeto de [ini,fim).
// js_texto para no primeiro "imdb" da faixa inteira; num item de playback de
// serie o bloco "episode" pode vir antes do "show", e num item de all-items o
// "show" e irmao de campos como "last_watched". Ler por nivel e o que garante
// que o titulo e o imdb sao DA OBRA. *vi/*vf delimitam o valor cru.
static int valorRaiz(const char *ini, const char *fim, const char *chave,
                     const char **vi, const char **vf) {
  const char *p = ini;
  size_t n = strlen(chave);
  int prof = 0;
  if (!p) return 0;
  if (!fim) fim = p + strlen(p);
  while (p < fim && *p != '{') p++;
  for (; p < fim && *p; p++) {
    if (*p == '"') {
      const char *a = p + 1, *q = a;
      while (q < fim && *q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;
      if (prof == 1 && (size_t)(q - a) == n && !strncmp(a, chave, n)) {
        const char *v = q + 1;
        while (v < fim && (unsigned char)*v <= ' ') v++;
        if (v < fim && *v == ':') {
          for (v++; v < fim && (unsigned char)*v <= ' '; v++) ;
          *vi = v;
          if (*v == '{' || *v == '[') *vf = js_fim(v);
          else if (*v == '"') {
            const char *r = v + 1;
            while (r < fim && *r && *r != '"') r += (*r == '\\' && r[1]) ? 2 : 1;
            *vf = r < fim ? r + 1 : fim;
          } else {
            const char *r = v;
            while (r < fim && *r != ',' && *r != '}' && *r != ']') r++;
            *vf = r;
          }
          return *vf > *vi;
        }
      }
      if (q >= fim || !*q) break;
      p = q;
      continue;
    }
    if (*p == '{' || *p == '[') prof++;
    else if ((*p == '}' || *p == ']') && --prof == 0) break;
  }
  return 0;
}

// O objeto `chave` da raiz ("show", "movie", "episode"). 0 quando falta ou e
// null — all-items manda "next_to_watch": null e playback pode nao ter
// "episode".
static int objetoRaiz(const char *ini, const char *fim, const char *chave,
                      const char **oi, const char **of) {
  return valorRaiz(ini, fim, chave, oi, of) && **oi == '{';
}

// Titulo, ano e ids.imdb de um objeto de obra (show/movie/anime). 0 sem imdb.
static int lerObra(const char *oi, const char *of, CatItem *d, char *imdb,
                   size_t nImdb) {
  const char *ii, *ifim;
  imdb[0] = 0;
  js_texto_raiz_em(oi, of, "title", d->titulo, sizeof d->titulo);
  if (objetoRaiz(oi, of, "ids", &ii, &ifim))
    js_texto(ii, ifim, "imdb", imdb, nImdb);
  // Um id que nao e "tt..." nao serve a ninguem aqui: metahub, Cinemeta e os
  // addons indexam pelo IMDb. Recusar na leitura evita um card sem arte.
  if (imdb[0] != 't' || imdb[1] != 't') { imdb[0] = 0; return 0; }
  { int ano = (int)js_num(oi, of, "year", 0.0);
    if (ano > 1800) snprintf(d->meta, sizeof d->meta, "%d", ano); }
  return 1;
}

// ------------------------------------------------------------ leitores puros

static int lerPlaybackItem(const char *p, const char *f, CatItem *d,
                           long long *id) {
    const char *oi, *of, *ei = NULL, *ef = NULL, *vi, *vf;
    char imdb[24], quando[40] = "";
    int serie = 0, anime = 0;
    memset(d, 0, sizeof *d);
    if (objetoRaiz(p, f, "movie", &oi, &of)) serie = 0;
    else if (objetoRaiz(p, f, "show", &oi, &of)) serie = 1;
    else if (objetoRaiz(p, f, "anime", &oi, &of)) serie = anime = 1;
    else return 0;
    if (!lerObra(oi, of, d, imdb, sizeof imdb)) return 0;
    if (serie) {
      int t = 0, e = 0;
      if (!objetoRaiz(p, f, "episode", &ei, &ef)) return 0;
      // ANIME SO COM A NUMERACAO DO TVDB. A doc: "Anime episodes return both
      // AniDB episode numbers (season, number) and TVDB numbers (tvdb_season,
      // tvdb_number)". O resto do app fala TVDB; o AniDB poria o card no
      // episodio errado — pior que nao mostrar.
      t = (int)js_num(ei, ef, "tvdb_season", 0.0);
      e = (int)js_num(ei, ef, "tvdb_number", 0.0);
      if (!anime && (t <= 0 || e <= 0)) {
        t = (int)js_num(ei, ef, "season", 0.0);
        e = (int)js_num(ei, ef, "episode", 0.0);
        if (e <= 0) e = (int)js_num(ei, ef, "number", 0.0);
      }
      if (t <= 0 || e <= 0) return 0;
      d->temporada = t;
      d->episodio = e;
      js_texto_raiz_em(ei, ef, "title", d->nomeEpisodio, sizeof d->nomeEpisodio);
      snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", imdb, t, e);
      snprintf(d->tipo, sizeof d->tipo, "series");
    } else {
      snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
      snprintf(d->tipo, sizeof d->tipo, "movie");
    }
    // `progress` e porcentagem em float (0-100, "45.5"). Os limites de 1% a
    // 90% sao aplicados em montarContinuar, igual para as tres fontes.
    if (valorRaiz(p, f, "progress", &vi, &vf)) d->progresso = (int)atof(vi);
    if (js_texto_raiz_em(p, f, "paused_at", quando, sizeof quando))
      d->retomadoMs = js_ms_iso(quando);
    if (id) {
      // O "id" da RAIZ e o do registro de playback — o que DELETE
      // /sync/playback/{id} pede. "64bit integer" na doc: atof guarda inteiro
      // exato ate 2^53, e o id do Simkl esta longe disso.
      *id = valorRaiz(p, f, "id", &vi, &vf) ? (long long)atof(vi) : 0;
    }
    return 1;
}

int simkl_ler_playback(const char *json, CatItem *dst, long long *ids, int max) {
  const char *p;
  int n = 0;
  if (!json || !dst) return 0;
  for (p = js_raiz_array(json); p && *p == '{' && n < max; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    long long id = 0;
    if (!lerPlaybackItem(p, f, &dst[n], &id)) continue;
    if (ids) ids[n] = id;
    n++;
  }
  return n;
}

// /sync/playback nao promete ordem. Guarda os `max` mais recentes enquanto
// percorre a resposta INTEIRA; assim um item novo depois da posicao 12 nao e
// perdido antes da ordenacao da fileira.
static int lerPlaybackRecentes(const char *json, CatItem *dst, long long *ids,
                               int max) {
  const char *p;
  int n = 0;
  if (!json || !dst || max <= 0) return 0;
  for (p = js_raiz_array(json); p && *p == '{'; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p);
    CatItem item;
    long long id = 0;
    int alvo;
    if (!lerPlaybackItem(p, f, &item, &id)) continue;
    if (n < max) alvo = n++;
    else {
      int i, velho = 0;
      for (i = 1; i < n; i++)
        if (dst[i].retomadoMs < dst[velho].retomadoMs) velho = i;
      if (item.retomadoMs <= dst[velho].retomadoMs) continue;
      alvo = velho;
    }
    dst[alvo] = item;
    if (ids) ids[alvo] = id;
  }
  return n;
}

int simkl_ler_assistindo(const char *json, CatItem *dst, int max) {
  const char *p;
  int n = 0;
  if (!json || !dst) return 0;
  // Sem nada em "watching" o Simkl responde `null` ou `{}` — js_array devolve
  // NULL e o laco nem comeca.
  for (p = js_array(json, NULL, "shows"); p && *p == '{' && n < max;
       p = js_prox(js_fim(p))) {
    const char *f = js_fim(p), *oi, *of;
    CatItem *d = &dst[n];
    char imdb[24], prox[16] = "", quando[40] = "";
    int t = 0, e = 0;
    memset(d, 0, sizeof *d);
    // "S01E05". Em dia com o que foi ao ar vem null, e ai nao ha o que seguir.
    if (!js_texto_raiz_em(p, f, "next_to_watch", prox, sizeof prox)) continue;
    if (sscanf(prox, "S%dE%d", &t, &e) != 2 || t <= 0 || e <= 0) continue;
    if (!objetoRaiz(p, f, "show", &oi, &of)) continue;
    if (!lerObra(oi, of, d, imdb, sizeof imdb)) continue;
    d->temporada = t;
    d->episodio = e;
    d->progresso = 0;
    snprintf(d->imdb, sizeof d->imdb, "%s:%d:%d", imdb, t, e);
    snprintf(d->tipo, sizeof d->tipo, "series");
    // O instante do "a seguir" e o do ULTIMO EPISODIO VISTO, como no Trakt
    // (ult[].quandoMs): e ele que diz quando a pessoa mexeu nesta serie.
    if (js_texto_raiz_em(p, f, "last_watched_at", quando, sizeof quando))
      d->retomadoMs = js_ms_iso(quando);
    n++;
  }
  return n;
}

static int inteiroExato(const char *s, char prefixo, int *valor) {
  char *fim;
  long v;
  if (!s || *s != prefixo || !s[1]) return 0;
  v = strtol(s + 1, &fim, 10);
  if (*fim || v <= 0 || v > 1000000) return 0;
  *valor = (int)v;
  return 1;
}

// Anime e um vetor separado na resposta de all-items. Guarda os mais recentes
// mesmo quando a API nao vier ordenada; so depois a camada de rede busca os
// mappings dos candidatos que realmente podem disputar a fileira.
int simkl_ler_anime_assistindo(const char *json, SimklAnimeProx *dst, int max) {
  const char *p;
  int n = 0;
  if (!json || !dst || max <= 0) return 0;
  for (p = js_array(json, NULL, "anime"); p && *p == '{'; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p), *oi, *of, *ii, *ifim;
    SimklAnimeProx c;
    char imdb[24], prox[16] = "", quando[40] = "";
    int alvo, absoluto = 0;
    memset(&c, 0, sizeof c);
    if (!js_texto_raiz_em(p, f, "next_to_watch", prox, sizeof prox) ||
        !inteiroExato(prox, 'E', &absoluto)) continue;
    // A resposta real de all-items chama a obra de `show`; versoes antigas e
    // exemplos anime-native usam `anime`. Aceitar ambos sem procurar por nome.
    if (!objetoRaiz(p, f, "show", &oi, &of) &&
        !objetoRaiz(p, f, "anime", &oi, &of)) continue;
    if (!lerObra(oi, of, &c.item, imdb, sizeof imdb)) continue;
    if (!objetoRaiz(oi, of, "ids", &ii, &ifim)) continue;
    c.simklId = (long long)js_num(ii, ifim, "simkl", 0.0);
    if (c.simklId <= 0) continue;
    c.absoluto = absoluto;
    c.item.progresso = 0;
    snprintf(c.item.imdb, sizeof c.item.imdb, "%s", imdb);
    snprintf(c.item.tipo, sizeof c.item.tipo, "series");
    if (js_texto_raiz_em(p, f, "last_watched_at", quando, sizeof quando))
      c.item.retomadoMs = js_ms_iso(quando);
    if (n < max) alvo = n++;
    else {
      int i, velho = 0;
      for (i = 1; i < n; i++)
        if (dst[i].item.retomadoMs < dst[velho].item.retomadoMs) velho = i;
      if (c.item.retomadoMs <= dst[velho].item.retomadoMs) continue;
      alvo = velho;
    }
    dst[alvo] = c;
  }
  // Buscar mapping na ordem de utilidade: se o teto de chamadas for atingido,
  // os itens mais recentes tiveram prioridade e os seguintes ficam para outra
  // rodada, sem deslocar um card novo por causa da ordem da resposta.
  { int a, b;
    for (a = 1; a < n; a++) {
      SimklAnimeProx t = dst[a];
      for (b = a - 1; b >= 0 && dst[b].item.retomadoMs < t.item.retomadoMs; b--)
        dst[b + 1] = dst[b];
      dst[b + 1] = t;
    } }
  return n;
}

int simkl_ler_mapa_anime(const char *json, int absoluto, int *temporada, int *episodio) {
  const char *p;
  int achou = 0, tFinal = 0, eFinal = 0;
  if (!json || absoluto <= 0 || !temporada || !episodio) return 0;
  for (p = js_raiz_array(json); p && *p == '{'; p = js_prox(js_fim(p))) {
    const char *f = js_fim(p), *vi, *vf, *ti, *tf;
    char tipo[20] = "";
    int numero, t, e;
    if (!valorRaiz(p, f, "episode", &vi, &vf)) continue;
    numero = atoi(vi);
    if (numero != absoluto) continue;
    // O array real pode ter `episode:3` DUAS vezes: uma entrada regular e um
    // special 3. next_to_watch E3 significa a regular. Specials sem mapping
    // nao tornam a entrada regular ambigua; duas regulares conflitantes sim.
    if (js_texto_raiz_em(p, f, "type", tipo, sizeof tipo) && strcmp(tipo, "episode"))
      continue;
    // Toda entrada correspondente precisa ter o mesmo mapping positivo. Uma
    // duplicata sem mapping ou com outro mapping e ambigua: nao arriscamos.
    if (!objetoRaiz(p, f, "tvdb", &ti, &tf)) return 0;
    t = (int)js_num(ti, tf, "season", 0.0);
    e = (int)js_num(ti, tf, "episode", 0.0);
    if (t <= 0 || e <= 0) return 0; // S0 ainda nao e tocavel com seguranca.
    if (achou && (t != tFinal || e != eFinal)) return 0;
    tFinal = t; eFinal = e; achou = 1;
  }
  if (!achou) return 0;
  *temporada = tFinal; *episodio = eFinal;
  return 1;
}

int simkl_ler_plantowatch(const char *json, int serie, CatItem *dst, int max) {
  const char *p;
  int n = 0;
  if (!json || !dst) return 0;
  for (p = js_array(json, NULL, serie ? "shows" : "movies"); p && *p == '{' && n < max;
       p = js_prox(js_fim(p))) {
    const char *f = js_fim(p), *oi, *of;
    CatItem *d = &dst[n];
    char imdb[24];
    memset(d, 0, sizeof *d);
    if (!objetoRaiz(p, f, serie ? "show" : "movie", &oi, &of)) continue;
    if (!lerObra(oi, of, d, imdb, sizeof imdb)) continue;
    snprintf(d->imdb, sizeof d->imdb, "%s", imdb);
    snprintf(d->tipo, sizeof d->tipo, "%s", serie ? "series" : "movie");
    d->naLista = 1;
    // O mesmo acabamento de trakt_lista: arte deterministica pelo IMDb, sem
    // um GET por item, e o genero de sempre para a legenda do cartaz.
    arte_metahub_preencher(d);
    snprintf(d->genero, sizeof d->genero, "%s",
             i18n(serie ? "Programa de TV" : "Filme"));
    n++;
  }
  return n;
}

static int imdbValido(const char *s) {
  int i;
  if (!s || s[0] != 't' || s[1] != 't' || !s[2]) return 0;
  for (i = 2; s[i] && s[i] != ':'; i++) if (s[i] < '0' || s[i] > '9') return 0;
  return i < 20;
}

int simkl_corpo_lista(char *dst, size_t tam, const char *imdb,
                      const char *tipo, int adicionar) {
  char id[24];
  const char *dp;
  size_t k;
  int w;
  if (!dst || !tam || !imdbValido(imdb)) return 0;
  dp = strchr(imdb, ':');
  k = dp ? (size_t)(dp - imdb) : strlen(imdb);
  if (k >= sizeof id) return 0;
  memcpy(id, imdb, k); id[k] = 0;
  // Filme e serie em vetores separados, como no Trakt: o id do IMDb resolvido
  // no escopo errado casa outro titulo, ou nenhum. Anime vai em "shows" — a
  // doc do add-to-list e do history/remove aceita os dois ali.
  w = snprintf(dst, tam,
               adicionar ? "{\"%s\":[{\"to\":\"plantowatch\",\"ids\":{\"imdb\":\"%s\"}}]}"
                         : "{\"%s\":[{\"ids\":{\"imdb\":\"%s\"}}]}",
               (tipo && !strcmp(tipo, "series")) ? "shows" : "movies", id);
  return w > 0 && (size_t)w < tam;
}

const char *simkl_rota_lista(int adicionar) {
  return adicionar ? "/sync/add-to-list" : "/sync/history/remove";
}

// ------------------------------------------------------------ historico (visto)
//
// Corpos de /sync/history e /sync/history/remove. Ver simkl.h para as rotas e a
// fonte; aqui so a montagem, pura, para o teste conferir texto contra texto.

// Anexa com snprintf e diz se coube. `*u` avanca; estourou, o corpo inteiro e
// recusado (0) em vez de sair cortado — JSON pela metade o servidor le como
// 400, ou pior, como outro titulo.
static int anexar(char *dst, size_t tam, size_t *u, const char *fmt, int a, int b) {
  int w;
  if (*u >= tam) return 0;
  w = snprintf(dst + *u, tam - *u, fmt, a, b);
  if (w < 0 || (size_t)w >= tam - *u) return 0;
  *u += (size_t)w;
  return 1;
}

static int baseImdb(const char *imdb, char *id, size_t tam) {
  size_t k;
  if (!imdbValido(imdb)) return 0;
  k = strcspn(imdb, ":");
  if (k >= tam) return 0;
  memcpy(id, imdb, k); id[k] = 0;
  return 1;
}

int simkl_corpo_historico_eps(char *dst, size_t tam, const char *imdb,
                              const VistoPar *pares, int qtd) {
  char id[24], feita[SMK_LOTE_MAX];
  size_t u = 0;
  int i, j, prim = 1;
  if (!dst || !tam || !pares || qtd < 1 || !baseImdb(imdb, id, sizeof id)) return 0;
  if (qtd > SMK_LOTE_MAX) qtd = SMK_LOTE_MAX;
  memset(feita, 0, sizeof feita);
  { int w = snprintf(dst, tam, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"},"
                     "\"use_tvdb_anime_seasons\":true,\"seasons\":[", id);
    if (w < 0 || (size_t)w >= tam) return 0;
    u = (size_t)w; }
  // AGRUPA POR TEMPORADA sem assumir ordem, como trakt_episodios_marcar: o
  // lote "ate aqui" atravessa temporadas e cada uma tem de aparecer UMA vez.
  // Sem watched_at: a doc diz que o servidor usa a hora do pedido.
  for (i = 0; i < qtd; i++) {
    int primEp = 1;
    if (feita[i]) continue;
    if (!anexar(dst, tam, &u, prim ? "{\"number\":%d,\"episodes\":[" : ",{\"number\":%d,\"episodes\":[",
                pares[i].temporada, 0)) return 0;
    prim = 0;
    for (j = i; j < qtd; j++) {
      if (feita[j] || pares[j].temporada != pares[i].temporada) continue;
      feita[j] = 1;
      if (!anexar(dst, tam, &u, primEp ? "{\"number\":%d}" : ",{\"number\":%d}",
                  pares[j].episodio, 0)) return 0;
      primEp = 0;
    }
    if (!anexar(dst, tam, &u, "]}", 0, 0)) return 0;
  }
  return anexar(dst, tam, &u, "]}]}", 0, 0);
}

int simkl_corpo_historico_titulo(char *dst, size_t tam, const char *imdb,
                                 const char *tipo, const int *temporadas,
                                 int nt, int visto) {
  char id[24];
  size_t u = 0;
  int i, serie = tipo && !strcmp(tipo, "series");
  if (!dst || !tam || !baseImdb(imdb, id, sizeof id)) return 0;
  if (!serie || visto) {
    // Filme (os dois sentidos) e serie marcada: o objeto so com ids. Para
    // serie, a guia mark-as-watched: "Drop both seasons and episodes" marca
    // todo episodio.
    int w = snprintf(dst, tam, "{\"%s\":[{\"ids\":{\"imdb\":\"%s\"}}]}",
                     serie ? "shows" : "movies", id);
    return w > 0 && (size_t)w < tam;
  }
  // DESMARCAR SERIE SEM TEMPORADAS APAGA A SERIE DA BIBLIOTECA do Simkl (aviso
  // da pagina remove-from-history). Com a lista de temporadas, cada entrada sem
  // `episodes` e expandida pelo servidor e a serie fica na biblioteca. Sem
  // temporada conhecida nao ha corpo seguro: 0, e nada e mandado.
  if (!temporadas || nt < 1) return 0;
  { int w = snprintf(dst, tam, "{\"shows\":[{\"ids\":{\"imdb\":\"%s\"},\"seasons\":[", id);
    if (w < 0 || (size_t)w >= tam) return 0;
    u = (size_t)w; }
  for (i = 0; i < nt; i++)
    if (!anexar(dst, tam, &u, i ? ",{\"number\":%d}" : "{\"number\":%d}", temporadas[i], 0))
      return 0;
  return anexar(dst, tam, &u, "]}]}", 0, 0);
}

const char *simkl_rota_historico(int visto) {
  return visto ? "/sync/history" : "/sync/history/remove";
}

// ------------------------------------------------------------ transporte

// Um pedido a api. `consulta` vai DEPOIS dos tres parametros obrigatorios.
// `metodo`: 'G' GET, 'P' POST, 'D' DELETE. O token entra so no cabecalho, e
// nada aqui imprime a URL — o client_id nao e segredo, mas o costume do app e
// logar so o caminho (ver rede_url_publica).
static char *pedir(char metodo, const char *caminho, const char *consulta,
                   const char *corpo, int *st) {
  char url[640], cid[200], nome[120], aut[400];
  const char *cab[3];
  char *r;
  *st = 0;
  if (!simkl_ativo() || !nuvem_simkl_cliente()[0]) return NULL;
  nuvem_url_escapar(nuvem_simkl_cliente(), cid, sizeof cid);
  nuvem_url_escapar(nuvem_simkl_app()[0] ? nuvem_simkl_app() : "nuvio", nome, sizeof nome);
  snprintf(url, sizeof url, "%s%s?client_id=%s&app-name=%s&app-version=1.0.1%s%s",
           SMK_API, caminho, cid, nome, consulta ? "&" : "", consulta ? consulta : "");
  snprintf(aut, sizeof aut, "Authorization: Bearer %s", simklauth_token());
  cab[0] = aut;
  cab[1] = "Accept: application/json";
  cab[2] = NULL;
  if (metodo == 'P')      r = rede_postar_st(url, 20, cab, corpo ? corpo : "", st);
  else if (metodo == 'D') r = rede_apagar(url, 20, cab, st);
  else                    r = rede_baixar_st(url, 25, cab, st);
  memset(aut, 0, sizeof aut);
  return r;
}

static char *pegar(const char *caminho, const char *consulta, int *st) {
  char *r = pedir('G', caminho, consulta, NULL, st);
  if (r && (*st < 200 || *st >= 300)) {
    printf("[simkl] %s -> HTTP %d\n", caminho, *st);
    fflush(stdout);
    free(r);
    return NULL;
  }
  return r;
}

// Assinatura barata de um corpo (FNV-1a). 0 = sem corpo.
static unsigned long assinar(const char *s) {
  unsigned long h = 1469598103UL;
  if (!s || !*s) return 0;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619UL;
  return h ? h : 1;
}

// ------------------------------------------------------------ estado

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;

// Registros de playback da ultima leitura (chave composta -> id), para o
// DELETE, e os ids "tt:S:E" que sao "a seguir". Tabelas laterais pela mesma
// razao de trakt.c: sizeof(CatItem) e o cabecalho do cache em disco.
static struct { char chave[28]; long long id; } play[SMK_PLAY_MAX];
static int nPlay;
static char proxIds[SMK_PLAY_MAX][28];
static int nProx;
// Plan to Watch conhecido: o que a ultima leitura trouxe e o que o "+" daqui
// pos depois dela. E so a guarda do "-" (ver simkl.h).
static char ptw[SMK_PTW_MAX][24];
static int nPtw;

// CACHE PELO /sync/activities. A doc e explicita: "For continuous sync, do NOT
// call /sync/all-items on a timer", e ameaca suspender o client_id. A regra
// aqui e a mais simples que a cumpre: o corpo das atividades e guardado por
// assinatura; igual ao da ultima leitura = nada mudou em lista nenhuma, e os
// corpos guardados valem. As atividades custam um GET pequeno por ciclo, que e
// o que a doc chama de "the cheapest call in the API". Nao usa date_from: as
// listas lidas aqui (watching, plantowatch) sao pequenas, e o incremental
// exigiria guardar e fundir o estado entre arranques.
static unsigned long assCw, assPtw;
static char *corpoPlay, *corpoAssist, *corpoAssistAnime, *corpoPtwFilmes, *corpoPtwSeries;

// Cache de respostas completas por OBRA Simkl. Assim a passagem E12 -> E13
// reutiliza a mesma tabela sem um GET novo. O limite de entradas e de bytes
// deixa o custo previsivel inclusive para anime longo. `carregando` + condicao
// deduplicam duas montagens simultaneas da mesma obra.
typedef struct {
  long long id;
  char *corpo;
  time_t expira;
  unsigned long uso;
  int carregando;
} AnimeCache;
static AnimeCache animeCache[SMK_ANIME_CACHE_MAX];
static pthread_mutex_t animeTrava = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t animeCond = PTHREAD_COND_INITIALIZER;
static unsigned long animeUso;

static pthread_mutex_t travaScr = PTHREAD_MUTEX_INITIALIZER;
static ScrobbleEstado scrEstado;
static struct { int acao; char id[64]; double pct; int tem; } scrPend;
static int scrFioVivo;

static void trocar(char **dst, char *novo) { free(*dst); *dst = novo; }

void simkl_esquecer(void) {
  int i;
  pthread_mutex_lock(&trava);
  nPlay = nProx = nPtw = 0;
  assCw = assPtw = 0;
  trocar(&corpoPlay, NULL); trocar(&corpoAssist, NULL); trocar(&corpoAssistAnime, NULL);
  trocar(&corpoPtwFilmes, NULL); trocar(&corpoPtwSeries, NULL);
  pthread_mutex_unlock(&trava);
  pthread_mutex_lock(&animeTrava);
  for (i = 0; i < SMK_ANIME_CACHE_MAX; i++) {
    free(animeCache[i].corpo);
    memset(&animeCache[i], 0, sizeof animeCache[i]);
  }
  pthread_cond_broadcast(&animeCond);
  pthread_mutex_unlock(&animeTrava);
  pthread_mutex_lock(&travaScr);
  memset(&scrEstado, 0, sizeof scrEstado);
  scrPend.tem = 0;
  pthread_mutex_unlock(&travaScr);
}

static unsigned long atividades(void) {
  int st = 0;
  char *r = pegar("/sync/activities", NULL, &st);
  unsigned long h = assinar(r);
  free(r);
  return h;
}

// Devolve corpo mallocado. `permitirRede=0` faz apenas consulta de cache;
// `usouRede` conta chamadas reais para o teto por rodada.
static char *episodiosAnime(long long id, int permitirRede, int *usouRede) {
  AnimeCache *e = NULL;
  char caminho[80], *r;
  time_t agora;
  int st = 0, i, alvo = -1;
  if (usouRede) *usouRede = 0;
  if (id <= 0) return NULL;
  pthread_mutex_lock(&animeTrava);
  for (;;) {
    agora = time(NULL);
    e = NULL;
    for (i = 0; i < SMK_ANIME_CACHE_MAX; i++)
      if (animeCache[i].id == id) { e = &animeCache[i]; break; }
    if (e && e->carregando) {
      pthread_cond_wait(&animeCond, &animeTrava);
      continue;
    }
    if (e && e->expira > agora) {
      char *c = e->corpo ? strdup(e->corpo) : NULL;
      e->uso = ++animeUso;
      pthread_mutex_unlock(&animeTrava);
      return c;
    }
    if (!permitirRede) { pthread_mutex_unlock(&animeTrava); return NULL; }
    if (e) alvo = (int)(e - animeCache);
    else {
      for (i = 0; i < SMK_ANIME_CACHE_MAX; i++)
        if (!animeCache[i].id && !animeCache[i].carregando) { alvo = i; break; }
      if (alvo < 0) {
        for (i = 0; i < SMK_ANIME_CACHE_MAX; i++)
          if (!animeCache[i].carregando &&
              (alvo < 0 || animeCache[i].uso < animeCache[alvo].uso)) alvo = i;
      }
    }
    if (alvo < 0) { pthread_mutex_unlock(&animeTrava); return NULL; }
    e = &animeCache[alvo];
    free(e->corpo); e->corpo = NULL;
    e->id = id; e->expira = 0; e->uso = ++animeUso; e->carregando = 1;
    break;
  }
  pthread_mutex_unlock(&animeTrava);

  if (usouRede) *usouRede = 1;
  snprintf(caminho, sizeof caminho, "/anime/episodes/%lld", id);
  r = pegar(caminho, NULL, &st);

  pthread_mutex_lock(&animeTrava);
  // A entrada fica reservada enquanto carregando, portanto continua sendo a
  // nossa. Um erro ganha apenas 30 s de repouso; mapping ausente em resposta
  // valida fica no corpo por 7 dias e nao martela a API a cada home.
  if (e->id != id || !e->carregando) {
    pthread_mutex_unlock(&animeTrava);
    free(r);
    return NULL;
  }
  if (r && strlen(r) <= SMK_ANIME_CORPO_MAX) {
    e->corpo = strdup(r);
    e->expira = time(NULL) + SMK_ANIME_TTL;
  } else {
    e->expira = time(NULL) + SMK_ANIME_ERRO_TTL;
  }
  e->carregando = 0;
  pthread_cond_broadcast(&animeCond);
  pthread_mutex_unlock(&animeTrava);
  return r;
}

int simkl_e_a_seguir(const char *id) {
  int i, sim = 0;
  if (!id) return 0;
  pthread_mutex_lock(&trava);
  for (i = 0; i < nProx && !sim; i++) sim = !strcmp(proxIds[i], id);
  pthread_mutex_unlock(&trava);
  return sim;
}

static int mesmaSerie(const char *a, const char *b) {
  size_t la = strcspn(a, ":"), lb = strcspn(b, ":");
  return la == lb && !strncmp(a, b, la);
}

int simkl_continuar(CatItem *saida, int max) {
  CatItem *prox;
  SimklAnimeProx *anime;
  long long ids[SMK_PLAY_MAX];
  unsigned long h;
  char *cPlay = NULL, *cAssist = NULL, *cAnime = NULL;
  int n, nP, nA, nAMapeado = 0, buscasAnime = 0, i, k;
  if (!saida || max <= 0) return 0;
  if (!simkl_ativo()) {
    pthread_mutex_lock(&trava); nPlay = nProx = 0; pthread_mutex_unlock(&trava);
    return 0;
  }
  if (max > SMK_PLAY_MAX) max = SMK_PLAY_MAX;

  h = atividades();
  pthread_mutex_lock(&trava);
  if (h && h == assCw && corpoPlay && corpoAssist && corpoAssistAnime) {
    cPlay = strdup(corpoPlay);
    cAssist = strdup(corpoAssist);
    cAnime = strdup(corpoAssistAnime);
  }
  pthread_mutex_unlock(&trava);
  if (!cPlay || !cAssist || !cAnime) {
    int st = 0;
    free(cPlay); free(cAssist); free(cAnime);
    cPlay = pegar("/sync/playback", NULL, &st);
    cAssist = pegar("/sync/all-items/shows/watching", NULL, &st);
    cAnime = pegar("/sync/all-items/anime/watching", "next_watch_info=yes", &st);
    pthread_mutex_lock(&trava);
    // So guarda o trio inteiro: meia resposta valendo como "nada mudou" ate a
    // proxima atividade seria uma fileira desfalcada por horas.
    if (h && cPlay && cAssist && cAnime) {
      trocar(&corpoPlay, strdup(cPlay));
      trocar(&corpoAssist, strdup(cAssist));
      trocar(&corpoAssistAnime, strdup(cAnime));
      assCw = h;
    } else assCw = 0;
    pthread_mutex_unlock(&trava);
    printf("[simkl] retomada: baixada (%s)\n", h ? "atividade nova" : "sem atividades");
  } else {
    printf("[simkl] retomada: atividades iguais, corpos guardados\n");
  }

  n = lerPlaybackRecentes(cPlay, saida, ids, max);
  pthread_mutex_lock(&trava);
  nPlay = 0;
  for (i = 0; i < n; i++)
    if (ids[i] > 0 && nPlay < SMK_PLAY_MAX) {
      snprintf(play[nPlay].chave, sizeof play[nPlay].chave, "%s", saida[i].imdb);
      play[nPlay].id = ids[i];
      nPlay++;
    }
  nProx = 0;
  pthread_mutex_unlock(&trava);

  // "A SEGUIR" disputa lugar como no Trakt: serie ja pausada nao entra de novo;
  // com a lista cheia, entra no lugar do item mais velho se for mais novo.
  // No heap e so durante a montagem: 64 CatItem sao ~1 MB, e isto roda uma
  // vez por ciclo de descoberta.
  prox = (CatItem *)malloc(sizeof(CatItem) * SMK_PLAY_MAX);
  anime = (SimklAnimeProx *)malloc(sizeof(SimklAnimeProx) * SMK_PLAY_MAX);
  nP = prox ? simkl_ler_assistindo(cAssist, prox, SMK_PLAY_MAX) : 0;
  for (k = 0; k < nP; k++) {
    int ja = 0, alvo;
    for (i = 0; i < n && !ja; i++) ja = mesmaSerie(saida[i].imdb, prox[k].imdb);
    if (ja) continue;
    if (n < max) alvo = n++;
    else {
      int vel = 0;
      for (i = 1; i < n; i++) if (saida[i].retomadoMs < saida[vel].retomadoMs) vel = i;
      if (saida[vel].retomadoMs >= prox[k].retomadoMs) continue;
      alvo = vel;
    }
    saida[alvo] = prox[k];
    pthread_mutex_lock(&trava);
    if (nProx < SMK_PLAY_MAX)
      snprintf(proxIds[nProx++], sizeof proxIds[0], "%s", prox[k].imdb);
    pthread_mutex_unlock(&trava);
  }

  // Anime entra na MESMA disputa cronologica, mas so depois de uma conversao
  // exata da obra Simkl + E absoluto para TVDB S/E. Uma serie ja pausada e
  // descartada antes do GET. Cache hits nao contam para o teto de 24 chamadas.
  nA = anime ? simkl_ler_anime_assistindo(cAnime, anime, SMK_PLAY_MAX) : 0;
  for (k = 0; k < nA; k++) {
    char *mapa;
    char imdbBase[sizeof anime[k].item.imdb];
    int ja = 0, alvo, t = 0, e = 0, usouRede = 0;
    for (i = 0; i < n && !ja; i++) ja = mesmaSerie(saida[i].imdb, anime[k].item.imdb);
    if (ja) continue;
    // Mapping nao altera o carimbo. Se a fileira de candidatos ja esta cheia
    // e este anime nao pode vencer nem o mais velho, nao gaste um GET para
    // descobrir S/E de um item que sera descartado de qualquer maneira.
    if (n >= max) {
      int vel = 0;
      for (i = 1; i < n; i++) if (saida[i].retomadoMs < saida[vel].retomadoMs) vel = i;
      if (saida[vel].retomadoMs >= anime[k].item.retomadoMs) continue;
    }
    mapa = episodiosAnime(anime[k].simklId, buscasAnime < SMK_ANIME_BUSCAS_MAX, &usouRede);
    if (usouRede) buscasAnime++;
    if (!mapa || !simkl_ler_mapa_anime(mapa, anime[k].absoluto, &t, &e)) {
      free(mapa);
      continue;
    }
    free(mapa);
    anime[k].item.temporada = t;
    anime[k].item.episodio = e;
    snprintf(imdbBase, sizeof imdbBase, "%s", anime[k].item.imdb);
    snprintf(anime[k].item.imdb, sizeof anime[k].item.imdb, "%.*s:%d:%d",
             (int)strcspn(imdbBase, ":"), imdbBase, t, e);
    if (n < max) alvo = n++;
    else {
      int vel = 0;
      for (i = 1; i < n; i++) if (saida[i].retomadoMs < saida[vel].retomadoMs) vel = i;
      if (saida[vel].retomadoMs >= anime[k].item.retomadoMs) continue;
      alvo = vel;
    }
    saida[alvo] = anime[k].item;
    nAMapeado++;
    pthread_mutex_lock(&trava);
    if (nProx < SMK_PLAY_MAX)
      snprintf(proxIds[nProx++], sizeof proxIds[0], "%s", anime[k].item.imdb);
    pthread_mutex_unlock(&trava);
  }
  free(prox);
  free(anime);
  free(cPlay);
  free(cAssist);
  free(cAnime);

  // Mais recente primeiro: e a ordem em que a fileira corta.
  { int a, b;
    for (a = 1; a < n; a++) {
      CatItem t = saida[a];
      for (b = a - 1; b >= 0 && saida[b].retomadoMs < t.retomadoMs; b--) saida[b + 1] = saida[b];
      saida[b + 1] = t;
    } }
  printf("[simkl] retomada: %d pausado(s) + %d serie(s) + %d/%d anime mapeado(s), "
         "%d GET mapping -> %d\n", nPlay, nP, nAMapeado, nA, buscasAnime, n);
  fflush(stdout);
  // A arte e o texto sao buscados por montarContinuar SOMENTE depois do corte
  // visivel. Enfeitar os 64 candidatos aqui faria dezenas de GETs para itens
  // que nunca chegam a fileira.
  return n;
}

// --- scrobble ---------------------------------------------------------------

static int enviarScrobble(int acao, const char *id, double pct) {
  char corpo[400], caminho[64], *r;
  int st = 0, ok;
  scrobble_corpo(corpo, sizeof corpo, id, pct);
  snprintf(caminho, sizeof caminho, "/scrobble/%s", scrobble_nome(acao));
  r = pedir('P', caminho, NULL, corpo, &st);
  ok = (st >= 200 && st < 300) || st == 409;
  printf("[simkl] %s %s %.1f%% -> %s (HTTP %d)\n",
         scrobble_nome(acao), id, pct, ok ? "ok" : "falhou", st);
  fflush(stdout);
  free(r);
  if (!ok && acao == SCR_STOP) {
    // Stop e o unico evento que fecha historico. Se o endpoint de scrobble
    // falhar, usa o endpoint de history ja comprovado pelo app; assim excluir
    // o segundo envio no player nao transforma uma falha transitoria em perda.
    char base[64], *dp;
    int t = 0, e = 0;
    snprintf(base, sizeof base, "%s", id);
    dp = strchr(base, ':');
    if (dp) { sscanf(dp + 1, "%d:%d", &t, &e); *dp = 0; }
    if (t > 0 && e > 0) {
      VistoPar par = { (short)t, (short)e };
      ok = simkl_episodios_marcar(base, &par, 1, 1);
    } else {
      ok = simkl_titulo_marcar(base, "movie", NULL, 0, 1);
    }
  }
  if (ok && (acao == SCR_PAUSE || acao == SCR_STOP)) {
    // O /sync/activities pode demorar alguns segundos para refletir a escrita.
    // Forcar uma leitura impede o corpo antigo de sobreviver pela assinatura.
    pthread_mutex_lock(&trava); assCw = 0; pthread_mutex_unlock(&trava);
  }
  // A primeira remontagem ao sair do player pode correr antes do POST. Quando
  // o stop termina, pede UMA segunda rodada (descoberta coalesce pedidos) para
  // que o novo next-up apareca sem reiniciar o app ou abrir o site do Simkl.
  if (ok && acao == SCR_STOP) desc_refazer_continuar();
  return ok;
}

static void *fioScrobble(void *u) {
  (void)u;
  for (;;) {
    int acao; char id[64]; double pct;
    pthread_mutex_lock(&travaScr);
    if (!scrPend.tem) { scrFioVivo = 0; pthread_mutex_unlock(&travaScr); return NULL; }
    acao = scrPend.acao; pct = scrPend.pct;
    snprintf(id, sizeof id, "%s", scrPend.id);
    scrPend.tem = 0;
    pthread_mutex_unlock(&travaScr);
    enviarScrobble(acao, id, pct);
  }
}

static int decidirScrobbleSimkl(ScrobbleEstado *s, int evento,
                                const char *id, double pct) {
  int mesmo;
  if (!id || !*id) return SCR_NADA;
  mesmo = !strcmp(s->id, id);
  if (evento == SCR_EV_TOCANDO) {
    if (s->ativo && mesmo) return SCR_NADA;
    snprintf(s->id, sizeof s->id, "%s", id);
    s->ativo = 1;
    return SCR_START;
  }
  if (evento == SCR_EV_PAUSOU) {
    if (!s->ativo || !mesmo) return SCR_NADA;
    s->ativo = 0;
    return pct >= 1.0 ? SCR_PAUSE : SCR_NADA;
  }
  if (evento == SCR_EV_SAIU) {
    snprintf(s->id, sizeof s->id, "%s", id);
    s->ativo = 0;
    if (pct >= 90.0) return SCR_STOP;
    return pct >= 1.0 ? SCR_PAUSE : SCR_NADA;
  }
  return SCR_NADA;
}

int simkl_scrobble(int evento, const char *imdb, double posSeg, double durSeg) {
  int acao;
  double pct;
  pthread_t t;
  if (!simkl_ativo() || !imdb || !*imdb || durSeg <= 1.0) return SCR_NADA;
  pct = 100.0 * posSeg / durSeg;
  pthread_mutex_lock(&travaScr);
  acao = decidirScrobbleSimkl(&scrEstado, evento, imdb, pct);
  if (acao != SCR_NADA) {
    scrPend.acao = acao; scrPend.pct = pct; scrPend.tem = 1;
    snprintf(scrPend.id, sizeof scrPend.id, "%s", imdb);
    if (!scrFioVivo) {
      scrFioVivo = 1;
      if (pthread_create(&t, NULL, fioScrobble, NULL) != 0) {
        scrFioVivo = 0;
        scrPend.tem = 0;
        acao = SCR_NADA; // player mantem o fallback normal de /sync/history
      }
      else pthread_detach(t);
    }
  }
  pthread_mutex_unlock(&travaScr);
  return acao;
}

int simkl_marcar(const char *imdb, double posSeg, double durSeg) {
  return simkl_scrobble(SCR_EV_SAIU, imdb, posSeg, durSeg);
}

int simkl_plantowatch(CatItem *saida, int max) {
  unsigned long h;
  char *cF = NULL, *cS = NULL;
  int n = 0, i;
  if (!saida || max <= 0 || !simkl_ativo()) return 0;
  h = atividades();
  pthread_mutex_lock(&trava);
  if (h && h == assPtw && corpoPtwFilmes && corpoPtwSeries) {
    cF = strdup(corpoPtwFilmes);
    cS = strdup(corpoPtwSeries);
  }
  pthread_mutex_unlock(&trava);
  if (!cF || !cS) {
    int st = 0;
    free(cF); free(cS);
    cF = pegar("/sync/all-items/movies/plantowatch", NULL, &st);
    cS = pegar("/sync/all-items/shows/plantowatch", NULL, &st);
    pthread_mutex_lock(&trava);
    if (h && cF && cS) {
      trocar(&corpoPtwFilmes, strdup(cF));
      trocar(&corpoPtwSeries, strdup(cS));
      assPtw = h;
    } else assPtw = 0;
    pthread_mutex_unlock(&trava);
  }
  n = simkl_ler_plantowatch(cF, 0, saida, max);
  n += simkl_ler_plantowatch(cS, 1, saida + n, max - n);
  // SO SUBSTITUI A TABELA COM AS DUAS RESPOSTAS NA MAO. Sem rede, a tabela
  // velha continua valendo: esvazia-la faria o "-" recusar um titulo que esta
  // la de verdade.
  if (cF && cS) {
    pthread_mutex_lock(&trava);
    nPtw = 0;
    for (i = 0; i < n && nPtw < SMK_PTW_MAX; i++)
      snprintf(ptw[nPtw++], sizeof ptw[0], "%s", saida[i].imdb);
    pthread_mutex_unlock(&trava);
  }
  free(cF); free(cS);
  printf("[simkl] plantowatch: %d\n", n);
  fflush(stdout);
  return n;
}

int simkl_na_plantowatch(const char *imdb) {
  int i, sim = 0;
  size_t L;
  if (!imdb || !imdb[0]) return 0;
  L = strcspn(imdb, ":");
  pthread_mutex_lock(&trava);
  for (i = 0; i < nPtw && !sim; i++)
    sim = strlen(ptw[i]) == L && !strncmp(ptw[i], imdb, L);
  pthread_mutex_unlock(&trava);
  return sim;
}

// ------------------------------------------------------------ escritas

static volatile int listaEstado;
static int listaFioVivo;
static char alvoId[24], alvoTipo[8];
static int alvoAdicionar;

static void ptwMarcar(const char *id, int dentro) {
  int i;
  pthread_mutex_lock(&trava);
  for (i = 0; i < nPtw; i++) if (!strcmp(ptw[i], id)) break;
  if (dentro && i == nPtw && nPtw < SMK_PTW_MAX)
    snprintf(ptw[nPtw++], sizeof ptw[0], "%s", id);
  if (!dentro && i < nPtw) { memmove(ptw[i], ptw[i + 1], sizeof ptw[0] * (size_t)(nPtw - i - 1)); nPtw--; }
  // A lista mudou por nossa mao: o proximo ciclo tem de reler, mesmo que as
  // atividades cheguem ao mesmo corpo por algum atraso do servidor.
  assPtw = 0;
  pthread_mutex_unlock(&trava);
}

static void *fioLista(void *u) {
  char corpo[200], id[24], tipo[8];
  char *r;
  int adicionar, st = 0, ok;
  (void)u;
  pthread_mutex_lock(&trava);
  snprintf(id, sizeof id, "%s", alvoId);
  snprintf(tipo, sizeof tipo, "%s", alvoTipo);
  adicionar = alvoAdicionar;
  pthread_mutex_unlock(&trava);
  ok = simkl_corpo_lista(corpo, sizeof corpo, id, tipo, adicionar);
  r = ok ? pedir('P', simkl_rota_lista(adicionar), NULL, corpo, &st) : NULL;
  // 2xx e a unica prova. O add-to-list responde 201 com "not_found" quando
  // nao achou o id — isso tambem e sucesso de transporte, e o log conta.
  ok = st >= 200 && st < 300;
  if (ok) ptwMarcar(id, adicionar);
  { const char *nf = r ? strstr(r, "\"not_found\"") : NULL;
    printf("[simkl] plantowatch %s %s (%s) -> %s (HTTP %d)%s\n",
           adicionar ? "add" : "del", id, tipo, ok ? "confirmado" : "falhou", st,
           nf && strstr(nf, id) ? " [nao encontrado no Simkl]" : ""); }
  fflush(stdout);
  free(r);
  listaEstado = ok ? SMK_OP_CONFIRMADA : SMK_OP_FALHA;
  pthread_mutex_lock(&trava); listaFioVivo = 0; pthread_mutex_unlock(&trava);
  return NULL;
}

int simkl_lista_tipo(const char *imdb, const char *tipo, int adicionar) {
  pthread_t t;
  size_t k;
  if (!simkl_ativo() || !imdbValido(imdb)) { listaEstado = SMK_OP_FALHA; return 0; }
  if (!adicionar && !simkl_na_plantowatch(imdb)) {
    // Ver simkl.h: /sync/history/remove apagaria o historico do titulo.
    printf("[simkl] del %s: fora do Plan to Watch conhecido; nada mandado\n", imdb);
    fflush(stdout);
    listaEstado = SMK_OP_FALHA;
    return 0;
  }
  pthread_mutex_lock(&trava);
  if (listaFioVivo) { pthread_mutex_unlock(&trava); return 0; }
  k = strcspn(imdb, ":");
  if (k >= sizeof alvoId) k = sizeof alvoId - 1;
  memcpy(alvoId, imdb, k); alvoId[k] = 0;
  snprintf(alvoTipo, sizeof alvoTipo, "%s",
           (tipo && !strcmp(tipo, "series")) || strchr(imdb, ':') ? "series" : "movie");
  alvoAdicionar = adicionar;
  listaEstado = SMK_OP_PENDENTE;
  listaFioVivo = 1;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&t, NULL, fioLista, NULL) != 0) {
    pthread_mutex_lock(&trava); listaFioVivo = 0; pthread_mutex_unlock(&trava);
    listaEstado = SMK_OP_FALHA;
    return 0;
  }
  pthread_detach(t);
  return 1;
}

int simkl_lista_estado(void) { return listaEstado; }

// POST do historico, SINCRONO (quem chama ja esta num fio: visto.c). 2xx
// sozinho nao prova nada: a doc manda olhar `not_found`, que volta com os
// objetos que o servidor nao resolveu. Um `"ids"` dentro dele e titulo que o
// Simkl nao achou pelo imdb — conta como falha, e o log diz.
// `qtd` > 0: lote de episodios; 0: titulo inteiro (`serie` diz qual).
static int postarHistorico(const char *corpo, int visto, const char *id, int qtd, int serie) {
  char *r;
  int st = 0, ok;
  const char *nf;
  r = pedir('P', simkl_rota_historico(visto), NULL, corpo, &st);
  ok = st >= 200 && st < 300;
  nf = r ? strstr(r, "\"not_found\"") : NULL;
  if (ok && nf && strstr(nf, "\"ids\"")) ok = 0;
  if (qtd > 0) printf("[simkl] historico %s %d eps de %s", visto ? "add" : "del", qtd, id);
  else printf("[simkl] historico %s %s %s", visto ? "add" : "del", serie ? "serie" : "filme", id);
  printf(" -> %s (HTTP %d)%s\n", ok ? "ok" : "falhou", st,
         nf && strstr(nf, "\"ids\"") ? " [nao encontrado no Simkl]" : "");
  fflush(stdout);
  free(r);
  return ok;
}

int simkl_episodios_marcar(const char *imdb, const VistoPar *pares, int qtd, int visto) {
  static char corpo[SMK_LOTE_MAX * 20 + 200];
  static pthread_mutex_t travaCorpo = PTHREAD_MUTEX_INITIALIZER;
  int ok;
  if (!simkl_ativo() || !imdb) return 0;
  pthread_mutex_lock(&travaCorpo);
  if (!simkl_corpo_historico_eps(corpo, sizeof corpo, imdb, pares, qtd)) {
    pthread_mutex_unlock(&travaCorpo);
    return 0;
  }
  ok = postarHistorico(corpo, visto, imdb, qtd > SMK_LOTE_MAX ? SMK_LOTE_MAX : qtd, 1);
  pthread_mutex_unlock(&travaCorpo);
  return ok;
}

int simkl_titulo_marcar(const char *imdb, const char *tipo, const int *temporadas,
                        int nt, int visto) {
  char corpo[1200];
  if (!simkl_ativo() || !imdb) return 0;
  if (!simkl_corpo_historico_titulo(corpo, sizeof corpo, imdb, tipo, temporadas, nt, visto)) {
    printf("[simkl] historico %s %s: sem corpo seguro (serie sem temporadas?); nada mandado\n",
           visto ? "add" : "del", imdb);
    fflush(stdout);
    return 0;
  }
  return postarHistorico(corpo, visto, imdb, 0, tipo && !strcmp(tipo, "series"));
}

static void *fioApagar(void *u) {
  long long id = *(long long *)u;
  char caminho[64], *r;
  int st = 0, ok;
  free(u);
  snprintf(caminho, sizeof caminho, "/sync/playback/%lld", id);
  r = pedir('D', caminho, NULL, NULL, &st);
  ok = st >= 200 && st < 300;
  free(r);
  printf("[simkl] playback remover id %lld -> %s (HTTP %d)\n", id, ok ? "ok" : "falhou", st);
  fflush(stdout);
  // SO ESQUECE O ID SE O SERVIDOR ACEITOU, como em trakt_playback_remover: num
  // 5xx a segunda tentativa ainda acha o id.
  if (ok) {
    int i;
    pthread_mutex_lock(&trava);
    for (i = 0; i < nPlay; i++) if (play[i].id == id) { play[i].chave[0] = 0; break; }
    assCw = 0;
    pthread_mutex_unlock(&trava);
  }
  return NULL;
}

int simkl_playback_remover(const char *imdb) {
  long long *id;
  pthread_t t;
  int i;
  if (!simkl_ativo() || !imdb || !imdb[0]) return 0;
  id = (long long *)malloc(sizeof *id);
  if (!id) return 0;
  *id = 0;
  pthread_mutex_lock(&trava);
  for (i = 0; i < nPlay; i++) if (!strcmp(play[i].chave, imdb)) { *id = play[i].id; break; }
  pthread_mutex_unlock(&trava);
  if (!*id) { free(id); return 0; }   // nao veio do Simkl: nada a apagar la
  if (pthread_create(&t, NULL, fioApagar, id) != 0) { free(id); return 0; }
  pthread_detach(t);
  return 1;
}
