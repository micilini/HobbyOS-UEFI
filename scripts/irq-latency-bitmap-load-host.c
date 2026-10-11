/* Actual complete-word/tail helper, including unaligned and high-frame inputs. */
#include "../kernel/src/utils/bitmap.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
int main(void)
{
    uint8_t data[32];
    for(unsigned i=0;i<32;++i)data[i]=(uint8_t)(i*37+19);
    for(unsigned offset=0;offset<8;++offset)for(unsigned size=0;size<=16;++size){
        Bitmap b={.buffer=data+offset,.size=size,.total_bits=size*8};
        for(unsigned index=0;index<3;++index){
            uint64_t expected=0;
            for(unsigned j=0;j<8 && index*8+j<size;++j)expected+=(uint64_t)data[offset+index*8+j]<<(8*j);
            assert(bitmap_load_word(&b,index)==expected);
        }
    }
    size_t bits=16777219,bytes;assert(bitmap_required_bytes(bits,&bytes));
    uint8_t *a=malloc(bytes+1),*r=calloc(bytes+1,1);assert(a&&r);
    Bitmap allocated={.buffer=a+1,.size=bytes,.total_bits=bits};
    Bitmap reserved={.buffer=r+1,.size=bytes,.total_bits=bits};
    for(size_t i=0;i<bytes;++i)allocated.buffer[i]=255;
    bitmap_set(&allocated,bits-1,false);bitmap_set(&reserved,bits-1,true);
    size_t free_bits;BitmapSearchStats stats;
    assert(!bitmap_validate_allocation(&allocated,&reserved,&free_bits,&stats));
    assert(free_bits==1 && stats.words_examined==(bits+63)/64);
    bitmap_set(&reserved,bits-1,false);
    assert(bitmap_validate_allocation(&allocated,&reserved,&free_bits,&stats) && free_bits==1);
    size_t found;assert(bitmap_find_first_zero(&allocated,0,&found,&stats) && found==bits-1);
    free(a);free(r);
    puts("IRQ latency actual-C full bitmap word/tail and high corruption: PASS");
}
