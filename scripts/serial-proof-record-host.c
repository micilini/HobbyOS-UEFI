#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/memory/stack_test.c"
static char wire[768];static size_t length;static bool accepted=true;
int kvsnprintf(char *out,size_t size,const char *format,va_list args){return vsnprintf(out,size,format,args);}
int ksnprintf(char *out,size_t size,const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(out,size,format,args);va_end(args);return n;}
uint32_t tasktest_transport_crc32(const char *data,uint32_t bytes)
{uint32_t crc=~0u;for(uint32_t i=0;i<bytes;++i){crc^=(unsigned char)data[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}return ~crc;}
bool serial_proof_record(const void *bytes,size_t size)
{assert(size<sizeof(wire));memcpy(wire,bytes,size);wire[size]=0;length=size;return accepted;}
int main(void)
{
    assert(stack_test_emit_checked("[HOSTED] value=%u\n",42u));
    char expected[128];const char *body="[HOSTED] value=42";
    snprintf(expected,sizeof(expected),"\n%s crc=%08x\n",body,tasktest_transport_crc32(body,strlen(body)));
    assert(strlen(expected)==length && !memcmp(wire,expected,length));
    accepted=false;assert(!stack_test_emit_checked("[HOSTED] value=42\n"));accepted=true;
    assert(!stack_test_emit_checked("missing newline"));
    assert(!stack_test_emit_checked("%0800u\n",1u));
    puts("Checked diagnostic record actual formatter: PASS exact_crc transport_failure malformed overflow");
}
