// sistema_redundante.c

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NUM_CONTEXTS 3
#define MAX_STEPS 100000

// Simulação dos três sensores
long sensor(int id)
{
    if (id == 1) return 100;
    if (id == 2) return 100;
    if (id == 3) return 100;

    return -1;
}

// Votador majoritário local
long majority(long s1, long s2, long s3)
{
    if (s1 == s2 || s1 == s3)
        return s1;

    if (s2 == s3)
        return s2;

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

    // Leitura dos três sensores
    long s1 = sensor(1);
    long s2 = sensor(2);
    long s3 = sensor(3);

    // Votador local:
    // MCU1 executa V1, MCU2 executa V2 e MCU3 executa V3
    long v = majority(s1, s2, s3);

    if (v == -1) {
        printf("ERRO: nao existe maioria entre os sensores.\n");
        return 1;
    }

    // Três contextos lógicos do IMT
    long contexto1 = 0;
    long contexto2 = 0;
    long contexto3 = 0;

    /*
     * IMT por interleaving:
     * executa um passo do contexto 1,
     * um passo do contexto 2
     * e um passo do contexto 3.
     *
     * A carga total permanece igual a MAX_STEPS.
     */
    for (int passo = 0; passo < MAX_STEPS; passo += NUM_CONTEXTS) {

        // Contexto 1
        contexto1 += v + (passo + 1);

        // Contexto 2
        if ((passo + 1) < MAX_STEPS)
            contexto2 += v + (passo + 2);

        // Contexto 3
        if ((passo + 2) < MAX_STEPS)
            contexto3 += v + (passo + 3);
    }

    long resultado =
        contexto1 + contexto2 + contexto3;

    volatile long saida_atuador = resultado;

    clock_t fim = clock();

    double tempo_interno =
        (double)(fim - inicio) / CLOCKS_PER_SEC;

    printf("MCU=%d\n", mcu_id);
    printf("S1=%ld S2=%ld S3=%ld\n", s1, s2, s3);
    printf("V%d=%ld\n", mcu_id, v);
    printf("SAIDA_MCU%d=%ld\n", mcu_id, saida_atuador);
    printf("TEMPO_INTERNO=%f s\n", tempo_interno);

    return 0;
}
