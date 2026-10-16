#!/bin/bash
# A escolha de perfil com PIN abre a sessao do perfil (#280: Home sem as
# fileiras de addon depois de um perfil com PIN).
#
# O PIN e confirmado DENTRO de perfilsel_atualizar, no meio do quadro. Antes, o
# mesmo quadro caia no "concluido -> TELA_HOME" do fim da tela de escolha e a
# checagem do comeco do quadro (que exige TELA_ESCOLHA_PERFIL) nunca rodava:
# perfilAbrirSessao ficava de fora e a descoberta recusava toda volta.
#
# Tira de src/app.c o abrirEscolhaDePerfil e o bloco da tela de escolha de
# app_atualizar, de verdade, e roda quadros contra dubles: escolha por OK (no
# evento), por PIN (no meio do quadro) e o Voltar (sem troca de sessao).
#
#   bash tests/perfil_pin_abre_sessao.sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-perfil-pin.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/t.c" <<'PY'
import pathlib, re, sys
app = pathlib.Path('src/app.c').read_text()
def bloco(fonte, inicio):
    i = fonte.index(inicio)
    j = fonte.index('{', i)
    n = 0
    for k in range(j, len(fonte)):
        if fonte[k] == '{': n += 1
        elif fonte[k] == '}':
            n -= 1
            if n == 0: return fonte[i:k + 1]
    raise SystemExit('bloco sem fim: ' + inicio)
ajudante = bloco(app, 'static int abrirEscolhaDePerfil(Uint32 agora)')
quadro = app[app.index('void app_atualizar(float dt, Uint32 agora) {'):]
# A checagem do comeco do quadro e a tela de escolha inteira, como estao.
topo = 'if (abrirEscolhaDePerfil(agora)) return;'
assert topo in quadro.split('if (tela == TELA_ESCOLHA_PERFIL) {')[0], 'checagem do comeco do quadro'
escolha = bloco(quadro, 'if (tela == TELA_ESCOLHA_PERFIL) {')
assert 'perfilsel_atualizar(dt, agora);' in escolha
corpo = ajudante + '\n'
proprios = {'abrirEscolhaDePerfil', 'if', 'return', 'sizeof', 'printf', 'fflush'}
nomes = sorted(set(re.findall(r'\b([a-z_][a-zA-Z0-9_]*)\(', ajudante + escolha)) - proprios)
c = r'''#include <stdio.h>
#include <stdint.h>
typedef uint32_t Uint32;
enum { TELA_HOME, TELA_LOGIN, TELA_ESCOLHA_PERFIL };
enum { MENU_INICIO = 1 };
#define TROCA_PERFIL_TETO_MS 6000
static int tela = TELA_ESCOLHA_PERFIL, perfilAntes = 1;
static Uint32 trocaPerfilDesde;
static uint64_t sessao = 1, perfilSessaoAplicada = 1;
static int concluido, preparando, sair, nAbriu, nSync, pinNoQuadro;
'''
especiais = {
  'perfilsel_concluido': 'static int perfilsel_concluido() { return concluido; }',
  'perfilsel_preparando': 'static int perfilsel_preparando() { return preparando; }',
  'perfilsel_preparar': 'static int perfilsel_preparar(int l, Uint32 a) { (void)a; preparando = l; return 0; }',
  'perfilsel_quer_sair': 'static int perfilsel_quer_sair() { int v = sair; sair = 0; return v; }',
  'perfis_sessao_geracao': 'static uint64_t perfis_sessao_geracao() { return sessao; }',
  'perfis_ativo': 'static int perfis_ativo() { return 1; }',
  'perfis_pode_dispensar': 'static int perfis_pode_dispensar() { return 1; }',
  'perfilAbrirSessao': 'static void perfilAbrirSessao(int a) { (void)a; nAbriu++; perfilSessaoAplicada = sessao; }',
  'sync_iniciar': 'static void sync_iniciar() { nSync++; }',
  # O PIN: a resposta do servidor e consumida aqui, no meio do quadro.
  'perfilsel_atualizar': 'static void perfilsel_atualizar(float d, Uint32 a) { (void)d; (void)a; if (pinNoQuadro) { pinNoQuadro = 0; sessao++; concluido = 1; } }',
}
for n in nomes:
    c += (especiais.get(n) or 'static int %s() { return 0; }' % n) + '\n'
c += corpo
c += 'static void quadro(float dt, Uint32 agora) {\n  ' + topo + '\n  ' + escolha + '\n}\n'
c += r'''
static int falhas;
#define CONFERE(c, m) do { if (c) printf("ok: %s\n", m); else { printf("FALHOU: %s\n", m); falhas++; } } while (0)
static void zerar(void) {
  tela = TELA_ESCOLHA_PERFIL; sessao = perfilSessaoAplicada = 1;
  concluido = preparando = sair = nAbriu = nSync = pinNoQuadro = 0;
}
int main(void) {
  // 1. OK num perfil sem PIN: conclui no evento, o comeco do quadro abre.
  zerar();
  sessao++; concluido = 1;             // perfilsel_evento -> escolher()
  quadro(0.016f, 100);
  CONFERE(nAbriu == 1 && preparando && tela == TELA_ESCOLHA_PERFIL, "OK sem PIN abre a sessao do perfil");
  quadro(0.016f, 116);
  CONFERE(nAbriu == 1, "e abre uma vez so");

  // 2. PIN: conclui DENTRO de perfilsel_atualizar. Era o defeito.
  zerar();
  pinNoQuadro = 1;
  quadro(0.016f, 100);
  CONFERE(nAbriu == 1, "PIN confirmado no meio do quadro abre a sessao do perfil");
  CONFERE(preparando && tela == TELA_ESCOLHA_PERFIL, "e a Home do perfil e preparada atras da tela de escolha");
  CONFERE(perfilSessaoAplicada == sessao, "a sessao aplicada e a do perfil escolhido");
  quadro(0.016f, 116);
  CONFERE(nAbriu == 1, "o quadro seguinte nao abre de novo");

  // 3. Voltar: segue com o perfil que ja estava, sem sessao nova.
  zerar();
  sair = 1;
  quadro(0.016f, 100);
  CONFERE(nAbriu == 0 && tela == TELA_HOME, "Voltar nao abre sessao e vai para a Home");

  printf(falhas ? "perfil_pin_abre_sessao: %d falha(s)\n" : "perfil_pin_abre_sessao: ok\n", falhas);
  return falhas != 0;
}
'''
pathlib.Path(sys.argv[1]).write_text(c)
PY
cc -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-deprecated-non-prototype -Wno-unknown-warning-option "$work/t.c" -o "$work/t"
"$work/t"
