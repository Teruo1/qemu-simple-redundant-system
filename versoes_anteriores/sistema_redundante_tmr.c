/* sistema_redundante_tmr.c
 * Prova de conceito: 3 VMs QEMU com troca real de resultados TCP,
 * IMT em software e votacao por maioria em cada VM.
 *
 * Rede ja configurada: Q1=192.168.90.11, Q2=192.168.90.12,
 * Q3=192.168.90.13. Cada VM usa sua propria identificacao (1,2,3).
 * Todos os resultados da rodada sao recebidos antes da selecao por
 * prioridade, para evitar decisoes diferentes entre os canais.
 *
 * Este prototipo exige os 3 canais ativos. Nao implementa eleicao
 * distribuida ou substituicao automatica de uma VM ausente.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NUM_CONTEXTS 3
#define MAX_STEPS 100000
#ifdef TEST_LOCAL
#define BASE_PORT 19200
#else
#define BASE_PORT 9100
#endif

#ifdef TEST_LOCAL
static const char *const ips[4] = {NULL, "127.0.0.1", "127.0.0.1", "127.0.0.1"};
#else
static const char *const ips[4] = {NULL, "192.168.90.11", "192.168.90.12", "192.168.90.13"};
#endif

static long sensor(int id) {
    return (id >= 1 && id <= 3) ? 100 : -1;
}
static long majority(long a, long b, long c) {
    if (a == b || a == c) return a;
    if (b == c) return b;
    return -1;
}
static double seconds_between(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}
static int send_exact(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
static int recv_exact(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
static void encode_result(long value, unsigned char buf[8]) {
    uint64_t u = (uint64_t)(int64_t)value;
    for (int i = 7; i >= 0; --i) {
        buf[i] = (unsigned char)(u & 0xffU);
        u >>= 8;
    }
}
static long decode_result(const unsigned char buf[8]) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | buf[i];
    return (long)(int64_t)u;
}
static int make_listener(int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + id));
    if (inet_pton(AF_INET, ips[id], &addr.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int connect_to(int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(BASE_PORT + id));
    if (inet_pton(AF_INET, ips[id], &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc != 2 || strlen(argv[1]) != 1 || argv[1][0] < '1' || argv[1][0] > '3') {
        fprintf(stderr, "Uso: %s 1|2|3\n", argv[0]);
        return 1;
    }
    const int mcu_id = argv[1][0] - '0';
    int peer_fd[4] = {-1, -1, -1, -1};
    int listener = -1;
    struct timespec t_total0, t_proc0, t_proc1, t_vote;
    clock_gettime(CLOCK_MONOTONIC, &t_total0);
    clock_t cpu_global0 = clock();

    /* Rede TCP ponto a ponto, agora com conexao direta em todos os pares. */
    if (mcu_id < 3) {
        listener = make_listener(mcu_id);
        if (listener < 0) { perror("listen/bind"); return 1; }
    }
    for (int i = 1; i < mcu_id; ++i) {
        peer_fd[i] = connect_to(i);
        if (peer_fd[i] < 0) {
            perror("connect (inicie Q1, depois Q2, depois Q3)");
            return 1;
        }
        unsigned char my_id = (unsigned char)mcu_id;
        if (send_exact(peer_fd[i], &my_id, 1) != 0) {
            perror("envio da identificacao"); return 1;
        }
    }
    for (int faltam = 3 - mcu_id; faltam > 0; --faltam) {
        int fd = accept(listener, NULL, NULL);
        unsigned char remote_id = 0;
        if (fd < 0 || recv_exact(fd, &remote_id, 1) != 0 ||
            remote_id <= mcu_id || remote_id > 3 || peer_fd[remote_id] != -1) {
            fprintf(stderr, "ERRO: conexao/identificacao de canal invalida\n");
            return 1;
        }
        peer_fd[remote_id] = fd;
    }
    if (listener >= 0) close(listener);

    /* Mesma sequencia funcional anterior: sensores, votador, IMT. */
    clock_gettime(CLOCK_MONOTONIC, &t_proc0);
    clock_t inicio = clock(); /* Mesmo ponto inicial do sistema original. */
    long s1 = sensor(1), s2 = sensor(2), s3 = sensor(3);
    long v = majority(s1, s2, s3);
    if (v == -1) { fprintf(stderr, "ERRO: sem maioria dos sensores\n"); return 1; }
    long contexto[NUM_CONTEXTS] = {0};
    int passo = 0;
    for (; passo + 2 < MAX_STEPS; passo += NUM_CONTEXTS) {
        contexto[0] += v + (passo + 1);
        contexto[1] += v + (passo + 2);
        contexto[2] += v + (passo + 3);
    }
    for (; passo < MAX_STEPS; ++passo)
        contexto[passo % NUM_CONTEXTS] += v + (passo + 1);
    long resultado = contexto[0] + contexto[1] + contexto[2];
    clock_gettime(CLOCK_MONOTONIC, &t_proc1);

    /* Resultados vindos efetivamente das outras VMs, sem copias locais. */
    long r[4] = {0, 0, 0, 0};
    r[mcu_id] = resultado;
    unsigned char buf[8];
    encode_result(resultado, buf);
    for (int i = 1; i <= 3; ++i) {
        if (i != mcu_id && send_exact(peer_fd[i], buf, sizeof(buf)) != 0) {
            fprintf(stderr, "ERRO: nao foi possivel enviar a MCU%d\n", i);
            return 1;
        }
    }
    for (int i = 1; i <= 3; ++i) {
        if (i == mcu_id) continue;
        if (recv_exact(peer_fd[i], buf, sizeof(buf)) != 0) {
            fprintf(stderr, "ERRO: nao foi possivel receber da MCU%d\n", i);
            return 1;
        }
        r[i] = decode_result(buf);
    }
    long saida_votada = majority(r[1], r[2], r[3]);
    if (saida_votada == -1) {
        fprintf(stderr, "ERRO: resultados sem maioria\n");
        return 1;
    }
    /* Prioridade deterministica entre canais com o resultado majoritario. */
    int prioridade = (r[1] == saida_votada) ? 1 :
                     (r[2] == saida_votada) ? 2 : 3;
    volatile long saida_atuador = saida_votada; /* Saida simulada em cada VM. */
    clock_gettime(CLOCK_MONOTONIC, &t_vote);
    clock_t fim = clock();
    printf("MCU=%d\nS1=%ld S2=%ld S3=%ld\nV%d=%ld\n", mcu_id, s1, s2, s3, mcu_id, v);
    printf("RESULTADO_LOCAL=%ld\nRESULTADOS_TMR=%ld,%ld,%ld\n", resultado, r[1], r[2], r[3]);
    printf("VOTACAO_FINAL=%ld\nCANAL_PRIORITARIO=MCU%d\n", saida_votada, prioridade);
    printf("SAIDA_MCU%d=%ld\n", mcu_id, saida_atuador);
    printf("AUTORIZACAO_LOGICA_DA_SAIDA=%s\n", (mcu_id == prioridade) ? "SIM" : "NAO");
    printf("TEMPO_INTERNO=%.9f s\n", (double)(fim - inicio) / CLOCKS_PER_SEC);
    printf("TEMPO_CONEXAO_CPU=%.9f s\n", (double)(inicio - cpu_global0) / CLOCKS_PER_SEC);
    printf("TEMPO_TOTAL_CPU_INCLUI_CONEXAO=%.9f s\n", (double)(fim - cpu_global0) / CLOCKS_PER_SEC);
    printf("TEMPO_PROCESSAMENTO_REAL=%.9f s\n", seconds_between(t_proc0, t_proc1));
    printf("TEMPO_COMUNICACAO_VOTACAO_REAL=%.9f s\n", seconds_between(t_proc1, t_vote));
    printf("TEMPO_DECISAO_REAL=%.9f s\n", seconds_between(t_proc0, t_vote));
    printf("TEMPO_TOTAL_REAL_INCLUI_CONEXAO=%.9f s\n", seconds_between(t_total0, t_vote));
    for (int i = 1; i <= 3; ++i) if (peer_fd[i] >= 0) close(peer_fd[i]);
    return 0;
}
