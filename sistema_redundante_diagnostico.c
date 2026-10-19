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

static double seconds_between(clock_t start, clock_t end)
{
    return (double)(end - start) / CLOCKS_PER_SEC;
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

    clock_t t0 = clock();
    long s1 = sensor(1);
    long s2 = sensor(2);
    long s3 = sensor(3);
    long v = majority(s1, s2, s3);
    if (v == -1) {
        printf("ERRO: sem maioria dos sensores.\n");
        return 1;
    }
    clock_t t1 = clock();

    long contexto[NUM_CONTEXTS] = {0};
    for (int passo = 0; passo < MAX_STEPS; passo += NUM_CONTEXTS) {
        for (int id = 0; id < NUM_CONTEXTS; id++) {
            int etapa = passo + id;
            if (etapa < MAX_STEPS) {
                contexto[id] += v + (etapa + 1);
            }
        }
    }
    clock_t t2 = clock();

    long resultado = 0;
    for (int i = 0; i < NUM_CONTEXTS; i++)
        resultado += contexto[i];
    clock_t t3 = clock();

    /* Votacao final simulada: nao ha troca de dados entre VMs. */
    long peer1 = resultado;
    long peer2 = resultado;
    long saida_votada = majority(resultado, peer1, peer2);
    volatile long saida_atuador = saida_votada;
    clock_t t4 = clock();

    printf("MCU=%d\n", mcu_id);
    printf("S1=%ld S2=%ld S3=%ld\n", s1, s2, s3);
    printf("V%d=%ld\n", mcu_id, v);
    printf("RESULTADO_LOCAL=%ld\n", resultado);
    printf("VOTACAO_FINAL=%ld\n", saida_votada);
    printf("SAIDA_MCU%d=%ld\n", mcu_id, saida_atuador);
    printf("TEMPO_SENSORES=%.9f s\n", seconds_between(t0, t1));
    printf("TEMPO_IMT=%.9f s\n", seconds_between(t1, t2));
    printf("TEMPO_CONSOLIDACAO=%.9f s\n", seconds_between(t2, t3));
    printf("TEMPO_VOTACAO_FINAL=%.9f s\n", seconds_between(t3, t4));
    printf("TEMPO_TOTAL_DIAGNOSTICO=%.9f s\n", seconds_between(t0, t4));
    printf("NOTA: tempos incluem custo de instrumentacao com clock().\n");
    return 0;
}
