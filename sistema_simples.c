/* sistema_simples.c
 * Instrumentacao da configuracao SIMPLES para comparacao
 * com o programa sistema_redundante.c.
 * Tres sensores, votacao local, processamento convencional
 * (sem IMT), MAX_STEPS=100000 e saida local.
 * Sem comunicacao TCP ou votacao inter-MCUs.
 * Compilar: gcc -O0 -std=c11 -Wall -Wextra -Werror sistema_simples.c -o sistema_simples
 * Executar: time ./sistema_simples
 */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STEPS 100000
#define WARMUP_MIN 20
#define WARMUP_MAX 500
#define MEASURE_ROUNDS 100
#define STABLE_WINDOW 10
#define STABLE_CHECKS 3
#define STABLE_TOLERANCE 0.15

static volatile sig_atomic_t interrupted = 0;
static void stop_requested(int signo) { (void)signo; interrupted = 1; }

static struct timespec now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) { perror("clock_gettime"); exit(1); }
    return t;
}
static double elapsed(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

/* Reproducao literal da logica do programa simples de referencia. */
static long sensor(int id) {
    if (id == 1) return 100;
    if (id == 2) return 100;
    if (id == 3) return 100;
    return -1;
}
static long majority(long s1, long s2, long s3) {
    if (s1 == s2 || s1 == s3) return s1;
    if (s2 == s3) return s2;
    return -1;
}
static long processamento(long entrada) {
    long resultado = 0;
    for (int passo = 0; passo < MAX_STEPS; passo++) {
        resultado += entrada + (passo + 1);
    }
    return resultado;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(const double *a, int n) {
    double tmp[MEASURE_ROUNDS];
    if (n <= 0 || n > MEASURE_ROUNDS) return 0.0;
    for (int i = 0; i < n; ++i) tmp[i] = a[i];
    qsort(tmp, (size_t)n, sizeof(tmp[0]), cmp_double);
    return (n % 2) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
}
static int stable_recent(const double history[WARMUP_MAX], int used) {
    if (used < WARMUP_MIN) return 0;
    double left = median(history + used - 2 * STABLE_WINDOW, STABLE_WINDOW);
    double right = median(history + used - STABLE_WINDOW, STABLE_WINDOW);
    double denom = right > 0.000000001 ? right : 0.000000001;
    double difference = left - right;
    if (difference < 0.0) difference = -difference;
    return difference / denom <= STABLE_TOLERANCE;
}
static void print_stats(const char *name, const double *a, int n) {
    if (n <= 0) return;
    double smallest = a[0], largest = a[0], sum = 0.0;
    for (int i = 0; i < n; ++i) {
        if (a[i] < smallest) smallest = a[i];
        if (a[i] > largest) largest = a[i];
        sum += a[i];
    }
    double sorted[MEASURE_ROUNDS];
    memcpy(sorted, a, (size_t)n * sizeof(a[0]));
    qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp_double);
    int p95idx = (95 * n + 99) / 100 - 1;
    printf("%s_MIN=%.9f s\n%s_MEDIA=%.9f s\n%s_MEDIANA=%.9f s\n"
           "%s_P95=%.9f s\n%s_MAX=%.9f s\n",
           name, smallest, name, sum / n, name, median(a, n),
           name, sorted[p95idx], name, largest);
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = stop_requested;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *filename = "simples_medicoes.csv";
    FILE *csv = fopen(filename, "w");
    if (!csv) { perror("fopen CSV"); return 1; }
    fprintf(csv, "rodada,fase,s1,s2,s3,votado,resultado_local,tempo_cpu_s,processamento_real_s,decisao_real_s\n");

    struct timespec session_start = now();
    double history[WARMUP_MAX] = {0};
    double samples_cpu[MEASURE_ROUNDS] = {0};
    double samples_proc[MEASURE_ROUNDS] = {0};
    double samples_wall[MEASURE_ROUNDS] = {0};
    int warmups = 0, measured = 0, consecutive = 0, stable_latched = 0;
    long last_result = -1, last_voted = -1;
    struct timespec batch_start = {0}, batch_end = {0};
    int status = 1;

    for (uint64_t round = 1; !interrupted && measured < MEASURE_ROUNDS; ++round) {
        int is_measure = stable_latched;
        struct timespec t0 = now();
        clock_t cpu_start = clock();
        long s1 = sensor(1), s2 = sensor(2), s3 = sensor(3);
        long voted = majority(s1, s2, s3);
        if (voted == -1) {
            fprintf(stderr, "ERRO: sem maioria local dos sensores\n");
            goto cleanup;
        }
        long result = processamento(voted);
        volatile long saida_atuador = result;
        (void)saida_atuador;
        struct timespec t_proc = now();
        clock_t cpu_end = clock();

        double cpu_s = (double)(cpu_end - cpu_start) / CLOCKS_PER_SEC;
        double proc_s = elapsed(t0, t_proc);
        double decision_s = proc_s; /* Sem troca TCP nem votacao entre MCUs. */
        fprintf(csv, "%" PRIu64 ",%s,%ld,%ld,%ld,%ld,%ld,%.9f,%.9f,%.9f\n",
                round, is_measure ? "MEDICAO" : "AQUECIMENTO",
                s1, s2, s3, voted, result, cpu_s, proc_s, decision_s);
        fflush(csv); /* Fora do trecho cronometrado, como no redundante. */
        last_result = result;
        last_voted = voted;
        if (is_measure) {
            if (measured == 0) {
                batch_start = t0;
                printf("ESTABILIZADO=SIM RODADA_INICIAL_MEDICAO=%" PRIu64 "\n", round);
            }
            samples_cpu[measured] = cpu_s;
            samples_proc[measured] = proc_s;
            samples_wall[measured] = decision_s;
            ++measured;
            batch_end = t_proc;
        } else {
            if (warmups >= WARMUP_MAX) {
                fprintf(stderr, "ERRO: estabilizacao nao atingida em %d rodadas\n", WARMUP_MAX);
                goto cleanup;
            }
            history[warmups++] = decision_s;
            if (!stable_latched) {
                if (stable_recent(history, warmups)) ++consecutive;
                else consecutive = 0;
                if (consecutive >= STABLE_CHECKS) stable_latched = 1;
            }
        }
    }
    if (interrupted) { fprintf(stderr, "Interrompido; CSV parcial preservado.\n"); goto cleanup; }

    printf("SISTEMA=SIMPLES VMS=1 CONEXOES_TCP=0\n");
    printf("AQUECIMENTO=%d MEDICOES=%d\n", warmups, measured);
    printf("S1=100 S2=100 S3=100 VOTADO=%ld\n", last_voted);
    printf("RESULTADO_LOCAL=%ld SAIDA_ATUADOR=%ld\n", last_result, last_result);
    printf("ARQUIVO_CSV=%s\n", filename);
    printf("TEMPO_LOTE_100_RODADAS_REAL=%.9f s (inclui registros entre rodadas)\n",
           elapsed(batch_start, batch_end));
    printf("TEMPO_SESSAO_COMPLETA_REAL=%.9f s (inclui aquecimento)\n",
           elapsed(session_start, now()));
    print_stats("CPU_RODADA", samples_cpu, measured);
    print_stats("TEMPO_DECISAO_REAL", samples_wall, measured);
    print_stats("TEMPO_PROCESSAMENTO_REAL", samples_proc, measured);
    printf("NOTA: sistema simples sem comunicacao entre MCUs; CPU nao equivale a tempo decorrido.\n");
    status = 0;
cleanup:
    fclose(csv);
    return status;
}
