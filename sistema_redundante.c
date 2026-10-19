#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_CONTEXTS 3
#define MAX_STEPS 100000

long sensor(int id)
{
    if (id >= 1 && id <= 3)
        return 100;
    return -1;
}

long majority(long a, long b, long c)
{
    if (a == b || a == c)
        return a;
    if (b == c)
        return b;
    return -1;
}

int main(int argc, char *argv[])
{
    int mcu_id = 1;

    if (argc > 1)
        mcu_id = atoi(argv[1]);

    if (mcu_id < 1 || mcu_id > 3) {
        printf("ERRO: use MCU 1, 2 ou 3.\n");
        return 1;
    }

    clock_t inicio = clock();

    long s1 = sensor(1);
    long s2 = sensor(2);
    long s3 = sensor(3);

    long v = majority(s1, s2, s3);

    if (v == -1) {
        printf("ERRO: sem maioria dos sensores.\n");
        return 1;
    }

    long contexto[NUM_CONTEXTS] = {0};

    for (int passo = 0; passo < MAX_STEPS; passo += NUM_CONTEXTS) {
        for (int id = 0; id < NUM_CONTEXTS; id++) {
            int etapa = passo + id;
            if (etapa < MAX_STEPS) {
                contexto[id] += v + (etapa + 1);
            }
        }
    }

    long resultado = 0;
    for (int i = 0; i < NUM_CONTEXTS; i++)
        resultado += contexto[i];

    /* Resultados simulados das outras MCUs: nao ha comunicacao entre VMs. */
    long peer1 = resultado;
    long peer2 = resultado;

    /* Segunda votacao majoritaria simulada. */
    long saida_votada = majority(resultado, peer1, peer2);
    volatile long saida_atuador = saida_votada;

    clock_t fim = clock();
    double tempo_interno = (double)(fim - inicio) / CLOCKS_PER_SEC;

    printf("MCU=%d\n", mcu_id);
    printf("S1=%ld S2=%ld S3=%ld\n", s1, s2, s3);
    printf("V%d=%ld\n", mcu_id, v);
    printf("RESULTADO_LOCAL=%ld\n", resultado);
    printf("VOTACAO_FINAL=%ld\n", saida_votada);
    printf("SAIDA_MCU%d=%ld\n", mcu_id, saida_atuador);
    printf("TEMPO_INTERNO=%f s\n", tempo_interno);
    return 0;
}
