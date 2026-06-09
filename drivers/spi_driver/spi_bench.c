/* spi_bench.c */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/* 用法：spi_bench /dev/spidevX.Y <字节数> <迭代次数> */
int main(int argc, char *argv[])
{
    int fd, len, iter, i;
    struct spi_ioc_transfer tr;
    struct timespec start, end;
    long elapsed_us;

    if (argc < 4) {
        printf("Usage: %s /dev/spidevX.Y <bytes> <iterations>\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    len = atoi(argv[2]);
    iter = atoi(argv[3]);

    uint8_t *tx = calloc(len, 1);
    uint8_t *rx = calloc(len, 1);
    /* 填充测试数据 */
    for (i = 0; i < len; i++) tx[i] = i & 0xFF;

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;

    /* 计时开始 */
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < iter; i++) {
        if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            perror("SPI transfer");
            break;
        }
    }

    /* 计时结束 */
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                 (end.tv_nsec - start.tv_nsec) / 1000;

    printf("%d x %dB: total %ld us, avg %ld us/xfer, %.1f KB/s\n",
           iter, len, elapsed_us, elapsed_us / iter,
           (double)iter * len * 1000000.0 / elapsed_us / 1024.0);

    free(tx); free(rx);
    close(fd);
    return 0;
}
