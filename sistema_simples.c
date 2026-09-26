// sistema_simples.c

#include <stdio.h>
#include <time.h>

#define MAX_STEPS 100000

// Simulação dos três sensores
long sensor(int id)
{
    if (id == 1) return 100;
    if (id == 2) return 100;
    if (id == 3) return 100;

    return -1;
}

// Votador majoritário V = M(S1, S2, S3)
long majority(long s1, long s2, long s3)
{
    if (s1 == s2 || s1 == s3)
        return s1;

    if (s2 == s3)
        return s2;

    return -1;
}

// Processamento convencional, sem IMT
long processamento(long entrada)
{
    long resultado = 0;

    for (int passo = 0; passo < MAX_STEPS; passo++) {
        resultado += entrada + (passo + 1);
    }

    return resultado;
}

int main(void)
{
    clock_t inicio = clock();

    // Leitura dos três sensores
    long s1 = sensor(1);
    long s2 = sensor(2);
    long s3 = sensor(3);

    // Votação majoritária dos sensores
    long v = majority(s1, s2, s3);

    if (v == -1) {
        printf("ERRO: nao existe maioria entre os sensores.\n");
        return 1;
    }

    // Processamento convencional do valor votado
    long resultado = processamento(v);

    // Representação da saída enviada ao atuador
    volatile long saida_atuador = resultado;

    clock_t fim = clock();

    double tempo_interno =
        (double)(fim - inicio) / CLOCKS_PER_SEC;

    printf("S1=%ld S2=%ld S3=%ld\n", s1, s2, s3);
    printf("VOTADO=%ld\n", v);
    printf("SAIDA_ATUADOR=%ld\n", saida_atuador);
    printf("TEMPO_INTERNO=%f s\n", tempo_interno);

    return 0;
}
