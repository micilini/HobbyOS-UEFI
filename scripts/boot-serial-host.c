#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void boot_serial_host_out(uint16_t port, uint8_t value);
uint8_t boot_serial_host_in(uint16_t port);

#include BOOT_SERIAL_ACTUAL_SOURCE
#include BOOT_GOP_ACTUAL_SOURCE

typedef struct {
    uint16_t base;
    uint8_t lcr, pending;
    bool queued, blocked;
    unsigned resets, discarded, reconfigured_pending, polls;
    char output[65536];
    size_t length;
} host_uart_t;

static host_uart_t ports[2];

static host_uart_t *find_uart(uint16_t port, unsigned *reg)
{
    for (unsigned i=0;i<2;++i) {
        if (port>=ports[i].base && port<ports[i].base+8) {
            *reg=port-ports[i].base;
            return &ports[i];
        }
    }
    assert(false);
    return NULL;
}

static void advance(host_uart_t *uart)
{
    if (!uart->queued || uart->blocked) return;
    assert(uart->length+1<sizeof(uart->output));
    uart->output[uart->length++]=(char)uart->pending;
    uart->output[uart->length]=0;
    uart->queued=false;
}

void boot_serial_host_out(uint16_t port, uint8_t value)
{
    if (port==0x80) return;
    unsigned reg;
    host_uart_t *uart=find_uart(port,&reg);
    if (reg==UART_LCR) {
        if (uart->queued) ++uart->reconfigured_pending;
        uart->lcr=value;
    } else if (reg==UART_IIR_FCR && (value&4)) {
        ++uart->resets;
        if (uart->queued) ++uart->discarded;
        uart->queued=false;
    } else if (reg==UART_RBR_THR_DLL && !(uart->lcr&0x80)) {
        assert(!uart->queued && !uart->blocked);
        uart->pending=value;
        uart->queued=true;
    }
}

uint8_t boot_serial_host_in(uint16_t port)
{
    unsigned reg;
    host_uart_t *uart=find_uart(port,&reg);
    if (reg==UART_LSR) {
        ++uart->polls;
        if (uart->blocked) return 0;
        advance(uart);
        return 0x60;
    }
    return reg==UART_LCR ? uart->lcr : 0;
}

static void reset(BootInfo *boot)
{
    memset(ports,0,sizeof(ports));
    memset(boot,0,sizeof(*boot));
    ports[0].base=0x4100;
    ports[1].base=0x40c0;
    boot->serial.count=2;
    for (unsigned i=0;i<2;++i) {
        boot->serial.ports[i].io_base=ports[i].base;
        boot->serial.ports[i].kind=HOBBYOS_SERIAL_KIND_16550_IO;
    }
}

int main(void)
{
    BootInfo boot;
    reset(&boot);
    serial_broadcast_boot_banner(&boot);
    serial_mode_line(&boot,"[UEFI][GOP] Query OK  ",2,1024,768,0);
    serial_write_all(&boot,"tail\n");
    for (unsigned i=0;i<2;++i) advance(&ports[i]);
    const char *expected="\r\nHobbyOS Started (UEFI Bootloader)\r\n"
        "[UEFI][GOP] Query OK   mode=0x0000000000000002 W=0x0000000000000400"
        " H=0x0000000000000300 ST=0x0000000000000000\r\ntail\r\n";
#ifdef EXPECT_FRAGMENT_RESET
    for (unsigned i=0;i<2;++i) {
        assert(ports[i].resets>1 && ports[i].discarded>0);
        assert(ports[i].reconfigured_pending>0);
        assert(strcmp(ports[i].output,expected)!=0);
    }
    printf("INCOMING_FRAGMENT_RESET_REPRODUCED discarded=%u resets=%u\n",
           ports[0].discarded,ports[0].resets);
#else
    for (unsigned i=0;i<2;++i) {
        assert(ports[i].resets==1 && !ports[i].discarded);
        assert(!ports[i].reconfigured_pending && ports[i].lcr==3);
        assert(strcmp(ports[i].output,expected)==0);
    }
    reset(&boot);
    serial_broadcast_boot_banner(&boot);
    for (unsigned i=0;i<2;++i) {
        advance(&ports[i]);
        ports[i].length=0;
        ports[i].output[0]=0;
    }
    for (unsigned i=0;i<1000;++i) {
        serial_write_all(&boot,"A");
        serial_write_hex64_all(&boot,UINT64_C(0xfedcba9876543210));
        serial_write_all(&boot,"\n");
    }
    for (unsigned i=0;i<2;++i) {
        advance(&ports[i]);
        assert(ports[i].length==19000 && ports[i].resets==1 && !ports[i].discarded);
        for (unsigned n=0;n<1000;++n)
            assert(memcmp(ports[i].output+n*19,"AFEDCBA9876543210\r\n",19)==0);
    }
    reset(&boot);
    serial_write_all(NULL,"unused");
    serial_write_hex64_all(NULL,1);
    boot.serial.count=0;
    serial_write_all(&boot,"unused");
    serial_write_hex64_all(&boot,1);
    for (unsigned i=0;i<2;++i) assert(!ports[i].resets && !ports[i].polls);
    reset(&boot);
    serial_broadcast_boot_banner(&boot);
    for (unsigned i=0;i<2;++i) advance(&ports[i]);
    unsigned before0=ports[0].polls, before1=ports[1].polls;
    size_t length0=ports[0].length, length1=ports[1].length;
    ports[0].blocked=true;
    serial_write_all(&boot,"X");
    advance(&ports[1]);
    assert(ports[0].polls-before0==2000000 && ports[0].length==length0);
    assert(ports[1].polls-before1==1 && ports[1].length==length1+1);
    assert(ports[1].output[length1]=='X');
    for (unsigned i=0;i<2;++i) assert(ports[i].resets==1 && !ports[i].discarded);
    puts("BOOT_SERIAL_LIFETIME_PASS cases=4");
#endif
    return 0;
}
