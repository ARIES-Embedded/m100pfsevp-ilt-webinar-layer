// SPDX-License-Identifier: MIT
/*
 * Interrupt Latency Timer example drivber for the Microchip PolarFire SoC
 *
 * This was derived form the FPGA fabric lsram example by Microchip and
 * modified for the Aries-Embedded demonstration Interrupt-Latency-Timer
 * IP-Core used as an example in various webinar sessions.
 *
 * Copyright (c) 2021 Microchip Technology Inc. All rights reserved.
 *
 * Modifictations for the ILT userspace driver:
 * Copyright (c) 2021 Aries Embedded GmbH, <mvd@aries-embedded.de>
 */

#include <sys/stat.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SYSFS_PATH_LEN (128)
#define ID_STR_LEN (32)
#define UIO_DEVICE_PATH_LEN (32)
#define NUM_UIO_DEVICES	(32)

/* factor to calculate delay register value from milliseconds
 * LSB is 40 ns, a ms is 1000000 ns */
#define ILT_MILLISEC_FACTOR (1000000/40)

#define BIT(x) (1<<x)


/* UIO device name to look for */
char uio_id_str[] = "aries_ilt";
char sysfs_template[] = "/sys/class/uio/uio%d/%s";


/* typedefs for register description straight form the
 * interrupt latency timer ip core datasheet */

/* interrupt request generatot modes */
enum aries_ilt_IRG_mode {
    irg_disabled,
    irg_free_running,
    irg_delay_after_ACK0,
    irg_delay_after_ACK3
};

/* masks and bits for Master Contorl and Status Register */
#define MCSR_MASK_IRG_MODE   0x03

#define MCSR_FLAG_START_DELAY_COUNTER        BIT(7)
#define MCSR_FLAG_FRT_LATCH                  BIT(24)
#define MCSR_FLAG_FRT_LATCH_VALID_L          BIT(24)
#define MCSR_FLAG_FRT_CLEAR                  BIT(25)
#define MCSR_FLAG_FRT_LATCH_VALID_H          BIT(25)
#define MCSR_FLAG_FRT_LATCH_OVERWRITTEN_L    BIT(26)
#define MCSR_FLAG_FRT_LATCH_OVERWRITTEN_H    BIT(27)
#define MCSR_FLAG_ILT_ENABLE                 BIT(31)


/* masks and bits for Interrupt Acknowledge and Status Register */
/* write access */
#define IASR_FLAG_W_ACK_0                    BIT(0)
#define IASR_FLAG_W_ACK_1                    BIT(1)
#define IASR_FLAG_W_ACK_2                    BIT(2)
#define IASR_FLAG_W_ACK_3                    BIT(3)
#define IASR_FLAG_W_CLR_LATCH_0              BIT(4)
#define IASR_FLAG_W_CLR_LATCH_1              BIT(5)
#define IASR_FLAG_W_CLR_LATCH_2              BIT(6)
#define IASR_FLAG_W_CLR_LATCH_3              BIT(7)
#define IASR_FLAG_W_CLR_INT_COUNTER          BIT(8)
#define IASR_FLAG_W_CLR_ACK0_MISSING         BIT(9)
#define IASR_FLAG_W_CLR_ACK3_MISSING         BIT(10)
#define IASR_FLAG_W_CLR_ALL_COUNTERS         BIT(31)

/* read access */
#define IASR_FLAG_R_VALID_0                  BIT(0)
#define IASR_FLAG_R_VALID_1                  BIT(1)
#define IASR_FLAG_R_VALID_2                  BIT(2)
#define IASR_FLAG_R_VALID_3                  BIT(3)
#define IASR_FLAG_R_OVERWRITTEN_0            BIT(4)
#define IASR_FLAG_R_OVERWRITTEN_1            BIT(5)
#define IASR_FLAG_R_OVERWRITTEN_2            BIT(6)
#define IASR_FLAG_R_OVERWRITTEN_3            BIT(7)
#define IASR_FLAG_R_DELAY_COUNTER_RUNNING    BIT(8)
#define IASR_FLAG_R_LATENCY_COUNTER_RUNNING  BIT(9)
#define IASR_FLAG_R_ACK3_WAIT                BIT(16)
#define IASR_FLAG_R_ACK0_WAIT                BIT(24)


/* ILT ip-core register map */
typedef struct aries_ilt_regmap {
    uint32_t    core_id;
    uint32_t    master_csr;
    uint32_t    fr_timer_latch_low;
    uint32_t    fr_timer_latch_high;
    uint32_t    int_gen_delay;
    uint32_t    reserved_1;
    uint32_t    reserved_2;
    uint32_t    reserved_3;
    uint32_t    int_ack_sr;
    uint32_t    int_count;
    uint32_t    missed_ack0_counter;
    uint32_t    missed_ack3_counter;
    uint32_t    ack0_latency_latch;
    uint32_t    ack1_latency_latch;
    uint32_t    ack2_latency_latch;
    uint32_t    ack3_latency_latch;
} aries_ilt_regmap_t;



/**
 * some static helpers to cope with ilt
 */

static void enable_ilt( volatile aries_ilt_regmap_t * ilt )
{
    ilt->master_csr |= MCSR_FLAG_ILT_ENABLE;
}


static void disable_ilt( volatile aries_ilt_regmap_t * ilt )
{
    ilt->master_csr &= ~MCSR_FLAG_ILT_ENABLE;
}


static void start_delay_counter( volatile aries_ilt_regmap_t * ilt )
{
    ilt->master_csr |= MCSR_FLAG_START_DELAY_COUNTER;
}

static void ilt_ack_app_interrupt( volatile aries_ilt_regmap_t * ilt )
{
    ilt->int_ack_sr |= ( IASR_FLAG_W_ACK_3 );
}

static void set_igm_mode( volatile aries_ilt_regmap_t * ilt, enum aries_ilt_IRG_mode mode )
{
    uint32_t csr = ilt->master_csr;
    csr &= ~MCSR_MASK_IRG_MODE;
    csr |= mode;
    ilt->master_csr = csr;
}

static void set_ilt_delay( volatile aries_ilt_regmap_t * ilt, uint16_t milliseconds )
{
    ilt->int_gen_delay = (uint32_t)milliseconds * ILT_MILLISEC_FACTOR;
}

static uint64_t read_frt_latch( volatile aries_ilt_regmap_t * ilt)
{
    return (uint64_t)(((uint64_t)(ilt->fr_timer_latch_high)<<32) & ilt->fr_timer_latch_low);
}

static bool get_frt_latch_valid(volatile aries_ilt_regmap_t * ilt)
{
    uint32_t csr = ilt->master_csr;

    // return true if both valid flags are set
    return ((csr & MCSR_FLAG_FRT_LATCH_VALID_H) &&
            (csr & MCSR_FLAG_FRT_LATCH_VALID_L) );
}


/* dump all registers to stdout */
static void debug_dump(volatile aries_ilt_regmap_t * ilt)
{
    int i = 0;

    printf("\nILT dump:\n");

    printf("(0x%02x) Core ID:            0x%08x\n", i, ilt->core_id);
    printf("(0x%02x) Master CSR:         0x%08x\n", i+=4, ilt->master_csr);
    printf("(0x%02x) FRT latch (low):    0x%08x\n", i+=4, ilt->fr_timer_latch_low);
    printf("(0x%02x) FRT latch (hi):     0x%08x\n", i+=4, ilt->fr_timer_latch_high);
    printf("(0x%02x) INT gen delay:      0x%08x\n", i+=4, ilt->int_gen_delay);
    printf("(0x%02x) Reserved:           0x%08x\n", i+=4, ilt->reserved_1);
    printf("(0x%02x) Reserved:           0x%08x\n", i+=4, ilt->reserved_2);
    printf("(0x%02x) Reserved:           0x%08x\n", i+=4, ilt->reserved_3);
    printf("(0x%02x) INT ack/SR:         0x%08x\n", i+=4, ilt->int_ack_sr);
    printf("(0x%02x) INT count:          0x%08x\n", i+=4, ilt->int_count);
    printf("(0x%02x) Missed ACK0 count:  0x%08x\n", i+=4, ilt->missed_ack0_counter);
    printf("(0x%02x) Missed ACK3 count:  0x%08x\n", i+=4, ilt->missed_ack3_counter);
    printf("(0x%02x) ACK0 latency latch: 0x%08x\n", i+=4, ilt->ack0_latency_latch);
    printf("(0x%02x) ACK1 latency latch: 0x%08x\n", i+=4, ilt->ack1_latency_latch);
    printf("(0x%02x) ACK2 latency latch: 0x%08x\n", i+=4, ilt->ack2_latency_latch);
    printf("(0x%02x) ACK3 latency latch: 0x%08x\n", i+=4, ilt->ack3_latency_latch);
}


/*****************************************/
/* function get_uio_device will return   */
/* the uio device number                 */
/*************************************** */
int get_uio_device(char * id)
{
    FILE *fp;
    int i;
    int len;
    char file_id[ID_STR_LEN];
    char sysfs_path[SYSFS_PATH_LEN];

    for (i = 0; i < NUM_UIO_DEVICES; i++) {
        snprintf(sysfs_path, SYSFS_PATH_LEN, sysfs_template, i, "/name");
        fp = fopen(sysfs_path, "r");
        if (fp == NULL)
            break;

        fscanf(fp, "%32s", file_id);
        len = strlen(id);
        if (len > ID_STR_LEN-1)
            len = ID_STR_LEN-1;
        if (strncmp(file_id, id, len) == 0) {
            return i;
        }
    }

    return -1;
}

/*****************************************/
/* function get_memory_size will return  */
/* the uio device size                   */
/*************************************** */
uint32_t get_memory_size(char *sysfs_path, char *uio_device)
{
    FILE *fp;
    uint32_t sz;

    /*
     * open the file the describes the memory range size.
     * this is set up by the reg property of the node in the device tree
     */
    fp = fopen(sysfs_path, "r");
    if (fp == NULL) {
        fprintf(stderr, "unable to determine size for %s\n", uio_device);
        exit(0);
    }

    fscanf(fp, "0x%016X", &sz);
    fclose(fp);
    return sz;
}


/* simple test in ILT free running mode */
void ilt_free_running_test(volatile aries_ilt_regmap_t *mem_ptr0, int uio_Fd0 )
{
    uint32_t info = 1; /* unmask */
    uint32_t loopcount = 0;

    /* always start with a disabled/reset timer */
    disable_ilt(mem_ptr0);


    printf("starting delay ILT...\n");
    enable_ilt(mem_ptr0);

    printf("setting generator delay...\n");
    set_ilt_delay( mem_ptr0, 1000 );

    printf("setting to free running mode...\n");
    set_igm_mode( mem_ptr0, irg_free_running );

    debug_dump(mem_ptr0);

    printf("starting delay counter...\n");

    start_delay_counter( mem_ptr0 );

    for( loopcount = 0; loopcount < 20; loopcount++) {
        /* wait for interrupt */
        ssize_t nb = read(uio_Fd0, &info, sizeof(info));

        if (nb == (ssize_t)sizeof(info)) {
            /* Do something in response to the interrupt. */

            /* acknowledge userspace handled interrupt (ACK3) */
            ilt_ack_app_interrupt(mem_ptr0);

            printf("\n\nInterrupt #%u!\n", info);
            printf("--------------------------\n");
            debug_dump(mem_ptr0);
            printf("--------------------------\n");
            printf("OS latency: %d ns\n", mem_ptr0->ack0_latency_latch * 40);
            printf("SW latency: %d ns\n", mem_ptr0->ack3_latency_latch * 40);
            printf("--------------------------\n");
        }
    }
    disable_ilt(mem_ptr0);
}


void ilt_test_dispatcher( volatile aries_ilt_regmap_t *mem_ptr0, int uio_Fd0 )
{
    char cmd;

    while (1) {
        printf("\n\t # Choose one of  the following options:\n");
        printf("\t Enter 1 to test in free running mode \n");
        // add further options here ;)
        printf("\t Enter q to Exit\n");
        scanf("%c%*c",&cmd);

        if ((cmd == '3') || (cmd == 'q'))
        {
                break;
        }
        else if (cmd == '1')
        {
            ilt_free_running_test( mem_ptr0, uio_Fd0 );
        }
        else {
            printf("Choose a valid option.\n");
        }
    }
}


/* main userspace "driver" function */
int main(int argc, char* argvp[])
{
    int retCode = 0;
    int uioFd_0,index=0;
    char uio_device[UIO_DEVICE_PATH_LEN];
    char sysfs_path[SYSFS_PATH_LEN];
    volatile aries_ilt_regmap_t *mem_ptr0;
    uint32_t mmap_size;

    /* look for correct UIO device */
    printf("locating device for %s\n", uio_id_str);
        index = get_uio_device(uio_id_str);
    if (index < 0) {
        fprintf(stderr, "can't locate uio device for %s\n", uio_id_str);
        return -1;
    }

    snprintf(uio_device, UIO_DEVICE_PATH_LEN, "/dev/uio%d", index);
    printf("located %s\n", uio_device);


    /* open UIO device */
    uioFd_0 = open(uio_device, O_RDWR);
    if(uioFd_0 < 0) {
        fprintf(stderr, "cannot open %s: %s\n", uio_device, strerror(errno));
        return -1;
    } else {
        printf("opened %s (r,w)\n", uio_device);
    }

    snprintf(sysfs_path, SYSFS_PATH_LEN, sysfs_template, index, "maps/map0/size");
    mmap_size = get_memory_size(sysfs_path, uio_device);
    if (mmap_size == 0) {
        fprintf(stderr, "bad memory size for %s\n", uio_device);
        return -1;
    }

    mem_ptr0 = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, uioFd_0, 0);
    if(mem_ptr0 == MAP_FAILED){
        fprintf(stderr, "Cannot mmap: %s\n", strerror(errno));
        close(uioFd_0);
        return -1;
    }

    // print some debug info
    printf("Memory mapped\n");
    printf("Core ID is 0x%x\n", mem_ptr0->core_id);
    printf("MCSR value is 0x%x\n", mem_ptr0->master_csr);

    //pass on to test chooser
    ilt_test_dispatcher( mem_ptr0, uioFd_0 );

    // clean up when we're finished
    retCode = munmap((void*)mem_ptr0, mmap_size);
    printf("unmapped %s\n", uio_device);
    // note: retCode ignored?

    retCode = close(uioFd_0);
    printf("closed %s\n", uio_device);
    return retCode;
}

