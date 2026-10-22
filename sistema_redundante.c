/* sistema_redundante.c
 * Tres canais QEMU (Q1/Q2/Q3), votacao local dos sensores,
 * IMT em software, resultados TCP reais e maioria final com prioridade.
 *
 * Evolucao do sistema_redundante_tmr.c: conexoes persistentes,
 * rodadas identificadas e medicao apos aquecimento operacional.
 * NAO modifica sistema_simples.c. NAO sincroniza os clocks das VMs.
 *
 * Compilar: gcc -O0 -std=c11 -Wall -Wextra -Werror \
 *              sistema_redundante.c -o sistema_redundante
 * Executar: ./sistema_redundante 1   (ou 2 ou 3)
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NUM_CONTEXTS 3
#define MAX_STEPS 100000
#define WARMUP_MIN 20
#define WARMUP_MAX 500
#define MEASURE_ROUNDS 100
#define STABLE_WINDOW 10
#define STABLE_CHECKS 3
#define STABLE_TOLERANCE 0.15 /* Diferenca relativa entre medianas consecutivas */
#define STARTUP_WAIT_SEC 300 /* Apenas disponibilidade das VMs, fora das medicoes */

#ifdef TEST_LOCAL
#define BASE_PORT 19200
static const char *const ips[4] = {NULL, "127.0.0.1", "127.0.0.1", "127.0.0.1"};
#else
#define BASE_PORT 9100
static const char *const ips[4] = {NULL, "192.168.90.11", "192.168.90.12", "192.168.90.13"};
#endif

static volatile sig_atomic_t interrupted = 0;
static void stop_requested(int signo) { (void)signo; interrupted = 1; }

static double elapsed(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}
static struct timespec now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) { perror("clock_gettime"); exit(1); }
    return t;
}
static long sensor(int id) { return (id >= 1 && id <= 3) ? 100 : -1; }
static long majority(long a, long b, long c) {
    if (a == b || a == c) return a;
    if (b == c) return b;
    return -1;
}
/* Mantem 100000 etapas distribuidas nos tres contextos IMT.
 * Evita recalcular (voted + passo + 1/2/3) tres vezes por trio.
 * A progressao aritmetica evita multiplicacao ou soma repetida do indice,
 * SEM substituir o processamento por formula fechada. */
static long process_imt(long voted) {
    long contexto[NUM_CONTEXTS] = {0};
    long termo = voted + 1;
    const int grupos = MAX_STEPS / NUM_CONTEXTS;
    for (int grupo = 0; grupo < grupos; ++grupo) {
        contexto[0] += termo;
        contexto[1] += termo + 1;
        contexto[2] += termo + 2;
        termo += NUM_CONTEXTS;
    }
    if (MAX_STEPS % NUM_CONTEXTS >= 1) contexto[0] += termo;
    if (MAX_STEPS % NUM_CONTEXTS >= 2) contexto[1] += termo + 1;
    return contexto[0] + contexto[1] + contexto[2];
}

static int send_exact(int fd, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n) {
        ssize_t wrote = send(fd, p, n, MSG_NOSIGNAL);
        if (wrote < 0 && errno == EINTR && !interrupted) continue;
        if (wrote <= 0) return -1;
        p += wrote; n -= (size_t)wrote;
    }
    return 0;
}
static int recv_exact(int fd, void *data, size_t n) {
    unsigned char *p = data;
    while (n) {
        ssize_t got = recv(fd, p, n, 0);
        if (got < 0 && errno == EINTR && !interrupted) continue;
        if (got <= 0) return -1;
        p += got; n -= (size_t)got;
    }
    return 0;
}
static void put_u64(unsigned char b[8], uint64_t v) {
    for (int j = 7; j >= 0; --j) { b[j] = (unsigned char)(v & 255U); v >>= 8; }
}
static uint64_t get_u64(const unsigned char b[8]) {
    uint64_t v = 0;
    for (int j = 0; j < 8; ++j) v = (v << 8) | b[j];
    return v;
}
/* Pacote minimo (17 bytes): rodada, resultado e flag local de estabilizacao.
 * Antes eram 24 bytes (7 de preenchimento). Atualizar TODAS as VMs juntas.
 * Nao muda o protocolo logico, apenas elimina bytes inuteis. */
#define PACKET_BYTES 17
static void make_packet(unsigned char buf[PACKET_BYTES], uint64_t round, long value, int stable) {
    put_u64(buf, round);
    put_u64(buf + 8, (uint64_t)(int64_t)value);
    buf[16] = stable ? 1U : 0U;
}

static int configure_stream(int fd) {
    int flag = 1;
    struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    return 0;
}
static int make_listener(int mcu) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + mcu));
    if (inet_pton(AF_INET, ips[mcu], &addr.sin_addr) != 1 ||
        bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 2) < 0) {
        perror("bind/listen (verifique o IP da interface ens3)");
        close(fd);
        return -1;
    }
    return fd;
}
/* Tenta conectar ate que o outro canal esteja ativo. A espera ocorre
 * SOMENTE durante a inicializacao, antes da primeira rodada medida. */
static int connect_to(int other) {
    struct timespec start = now();
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + other));
    if (inet_pton(AF_INET, ips[other], &addr.sin_addr) != 1) return -1;
    while (!interrupted && elapsed(start, now()) < STARTUP_WAIT_SEC) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
            if (configure_stream(fd) == 0) return fd;
            close(fd);
            return -1;
        }
        int error = errno;
        close(fd);
        if (error != ECONNREFUSED && error != ENETUNREACH &&
            error != EHOSTUNREACH && error != EADDRNOTAVAIL && error != ETIMEDOUT) {
            errno = error;
            return -1;
        }
        const struct timespec retry = {.tv_sec = 0, .tv_nsec = 100000000L};
        (void)nanosleep(&retry, NULL); /* Disponibilidade da rede, fora das medicoes. */
    }
    errno = ETIMEDOUT;
    return -1;
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
    return n % 2 ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
}
/* Critério operacional de aquecimento: comparacao das medianas de
 * duas janelas adjacentes de 10 rodadas de TEMPO_DECISAO_REAL.
 * E heuristica de estabilidade, nao uma prova de tempo deterministico. */
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
int main(int argc, char **argv) {
    if (argc != 2 || strlen(argv[1]) != 1 || argv[1][0] < '1' || argv[1][0] > '3') {
        fprintf(stderr, "Uso: %s 1|2|3\n", argv[0]);
        return 1;
    }
    struct sigaction action = {0};
    action.sa_handler = stop_requested;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    const int mcu = argv[1][0] - '0';
    int fds[4] = {-1, -1, -1, -1};
    int listener = -1;
    int status = 1;
    FILE *csv = NULL;
    char filename[80];
    snprintf(filename, sizeof(filename), "tmr_medicoes_mcu%d.csv", mcu);
    csv = fopen(filename, "w");
    if (!csv) { perror("fopen CSV"); return 1; }
    fprintf(csv, "rodada,fase,mcu,resultado_local,q1,q2,q3,votacao,prioridade,autorizada,"
                 "tempo_cpu_s,processamento_real_s,comunicacao_votacao_real_s,decisao_real_s\n");

    struct timespec setup_start = now();
    if (mcu < 3) {
        listener = make_listener(mcu);
        if (listener < 0) goto cleanup;
    }
    for (int i = 1; i < mcu; ++i) {
        fds[i] = connect_to(i);
        if (fds[i] < 0) { perror("conexao entre VMs"); goto cleanup; }
        unsigned char id = (unsigned char)mcu;
        if (send_exact(fds[i], &id, 1) != 0) { perror("identificacao TCP"); goto cleanup; }
    }
    for (int left = 3 - mcu; left > 0; --left) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { perror("accept TCP"); goto cleanup; }
        unsigned char id = 0;
        if (configure_stream(fd) != 0 || recv_exact(fd, &id, 1) != 0 ||
            id <= mcu || id > 3 || fds[id] != -1) {
            fprintf(stderr, "ERRO: identificacao TCP invalida\n");
            close(fd);
            goto cleanup;
        }
        fds[id] = fd;
    }
    if (listener >= 0) { close(listener); listener = -1; }
    printf("MCU=%d CONEXOES_TCP=2 TEMPO_SETUP_REDE_REAL=%.9f s\n",
           mcu, elapsed(setup_start, now()));

    double history[WARMUP_MAX] = {0};
    double samples_cpu[MEASURE_ROUNDS] = {0};
    double samples_wall[MEASURE_ROUNDS] = {0};
    double samples_proc[MEASURE_ROUNDS] = {0};
    double samples_comm[MEASURE_ROUNDS] = {0};
    int warmups = 0, measured = 0, consecutive = 0, stable_latched = 0;
    int all_stable_latched = 0;
    long last_voted = -1;
    long last_local = -1;
    int last_priority = 0;
    struct timespec batch_start = {0}, batch_end = {0};

    for (uint64_t round = 1; !interrupted && measured < MEASURE_ROUNDS; ++round) {
        const int local_ready = stable_latched;
        struct timespec t0 = now();
        clock_t cpu_start = clock();
        long s1 = sensor(1), s2 = sensor(2), s3 = sensor(3);
        long voted_sensor = majority(s1, s2, s3);
        if (voted_sensor == -1) { fprintf(stderr, "ERRO: votacao de sensores\n"); goto cleanup; }
        long result = process_imt(voted_sensor);
        struct timespec t_proc = now();

        long r[4] = {0, 0, 0, 0};
        int ready[4] = {0, 0, 0, 0};
        r[mcu] = result;
        ready[mcu] = local_ready;
        unsigned char outgoing[PACKET_BYTES];
        make_packet(outgoing, round, result, local_ready);
        for (int i = 1; i <= 3; ++i) {
            if (i != mcu && send_exact(fds[i], outgoing, sizeof(outgoing)) != 0) {
                perror("envio TCP"); goto cleanup;
            }
        }
        for (int i = 1; i <= 3; ++i) {
            if (i == mcu) continue;
            unsigned char incoming[PACKET_BYTES];
            if (recv_exact(fds[i], incoming, sizeof(incoming)) != 0 ||
                get_u64(incoming) != round || incoming[16] > 1U) {
                fprintf(stderr, "ERRO: recepcao TCP/rodada incorreta MCU%d rodada=%" PRIu64 "\n", i, round);
                goto cleanup;
            }
            r[i] = (long)(int64_t)get_u64(incoming + 8);
            ready[i] = incoming[16];
        }
        long voted_result = majority(r[1], r[2], r[3]);
        if (voted_result == -1) {
            fprintf(stderr, "ERRO: resultados sem maioria, rodada=%" PRIu64 "\n", round);
            goto cleanup;
        }
        int priority = (r[1] == voted_result) ? 1 :
                       (r[2] == voted_result) ? 2 : 3;
        volatile long actuator = voted_result;
        (void)actuator;
        struct timespec t_vote = now();
        clock_t cpu_end = clock();

        const double cpu_s = (double)(cpu_end - cpu_start) / CLOCKS_PER_SEC;
        const double proc_s = elapsed(t0, t_proc);
        const double comm_s = elapsed(t_proc, t_vote);
        const double decision_s = elapsed(t0, t_vote);
        const int everyone_ready = ready[1] && ready[2] && ready[3];
        const int is_measure = everyone_ready;
        const char *phase = is_measure ? "MEDICAO" : "AQUECIMENTO";
        fprintf(csv, "%" PRIu64 ",%s,%d,%ld,%ld,%ld,%ld,%ld,%d,%d,%.9f,%.9f,%.9f,%.9f\n",
                round, phase, mcu, result, r[1], r[2], r[3], voted_result,
                priority, (mcu == priority), cpu_s, proc_s, comm_s, decision_s);
        fflush(csv); /* Fora do intervalo cronometrado. */
        last_voted = voted_result;
        last_local = result;
        last_priority = priority;
        if (is_measure) {
            if (!all_stable_latched) {
                batch_start = t0;
                printf("TODOS_ESTABILIZADOS=SIM RODADA_INICIAL_MEDICAO=%" PRIu64 "\n", round);
                all_stable_latched = 1;
            }
            samples_cpu[measured] = cpu_s;
            samples_wall[measured] = decision_s;
            samples_proc[measured] = proc_s;
            samples_comm[measured] = comm_s;
            ++measured;
            batch_end = t_vote;
        } else {
            if (warmups >= WARMUP_MAX) {
                fprintf(stderr, "ERRO: criterio de estabilizacao nao atingido nas tres VMs em %d rodadas\n",
                        WARMUP_MAX);
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
    if (interrupted) { fprintf(stderr, "Interrompido pelo usuario; dados parciais preservados.\n"); goto cleanup; }

    printf("MCU=%d AQUECIMENTO=%d MEDICOES=%d\n", mcu, warmups, measured);
    printf("RESULTADO_LOCAL=%ld VOTACAO_FINAL=%ld CANAL_PRIORITARIO=MCU%d\n",
           last_local, last_voted, last_priority);
    printf("SAIDA_AUTORIZADA=%s ARQUIVO_CSV=%s\n", (mcu == last_priority) ? "SIM" : "NAO", filename);
    printf("TEMPO_LOTE_100_RODADAS_REAL=%.9f s (inclui registros entre rodadas)\n",
           elapsed(batch_start, batch_end));
    printf("TEMPO_SESSAO_COMPLETA_REAL=%.9f s (inclui inicio da rede e aquecimento)\n",
           elapsed(setup_start, now()));
    print_stats("CPU_RODADA", samples_cpu, measured);
    print_stats("TEMPO_DECISAO_REAL", samples_wall, measured);
    print_stats("TEMPO_PROCESSAMENTO_REAL", samples_proc, measured);
    print_stats("TEMPO_COMUNICACAO_VOTACAO_REAL", samples_comm, measured);
    printf("NOTA: tempos sao observacoes locais; CPU nao equivale a tempo decorrido.\n");
    status = 0;
cleanup:
    if (csv) fclose(csv);
    if (listener >= 0) close(listener);
    for (int i = 1; i <= 3; ++i) if (fds[i] >= 0) close(fds[i]);
    return status;
}
