#include "../src/ajustes.c"
#include <assert.h>
#include <unistd.h>

int main(void) {
  char dir[] = "/tmp/nuvio-aj-rede-XXXXXX", path[700];
  FILE *f;
  assert(mkdtemp(dir));
  snprintf(path, sizeof path, "%s/envio-151.txt", dir);
  f = fopen(path, "w"); assert(f); fputs("1\n", f); fclose(f);
  ajustes_dir(dir);
  assert(ajustes_rede_modo() == 0);
  assert(!strcmp(CHAVE[AJ_REDE_MODO], "redeModoLocal"));
  assert(somenteDesteAparelho(AJ_REDE_MODO));
  assert(!dePerfil(AJ_REDE_MODO));
  mudarValor(AJ_REDE_MODO, 1);
  assert(ajustes_rede_modo() == 1);
  valor[AJ_REDE_MODO] = 0;
  ajustes_dir(dir);
  assert(ajustes_rede_modo() == 1);
  ajustes_aplicar_blob("{\"redeModoLocal\":0,\"rede_modo_local\":0}");
  assert(ajustes_rede_modo() == 1);
  snprintf(path, sizeof path, "%s/ajustes.txt", dir); unlink(path);
  snprintf(path, sizeof path, "%s/envio-151.txt", dir); unlink(path);
  rmdir(dir);
  puts("ajustes_rede_modo: default, round-trip and device scope ok");
  return 0;
}
