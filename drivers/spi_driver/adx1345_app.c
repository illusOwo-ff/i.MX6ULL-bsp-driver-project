/* adxl345_app.c */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

int main(void)
{
    int fd = open("/dev/adxl345", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    int16_t accel[3];
    int ret = read(fd, accel, sizeof(accel));
    if (ret < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    printf("X: %d  Y: %d  Z: %d\n", accel[0], accel[1], accel[2]);

    close(fd);
    return 0;
}