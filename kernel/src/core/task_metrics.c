#include "task_metrics.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../drivers/serial.h"
static task_metrics_stats_t g_stats;
static volatile uint8_t g_ui_telemetry; static task_cpu_sampler_t g_selftest_sampler;
void task_metrics_ui_telemetry_set(bool enabled){__atomic_store_n(&g_ui_telemetry,enabled?1:0,__ATOMIC_RELEASE);}
bool task_metrics_ui_telemetry_enabled(void){return __atomic_load_n(&g_ui_telemetry,__ATOMIC_ACQUIRE)!=0;}
static int find(const task_cpu_sampler_t*s,task_id_t id){for(uint32_t i=0;i<s->count;i++)if(s->entries[i].valid&&s->entries[i].id==id)return(int)i;return-1;}
bool task_metrics_muldiv_u64(uint64_t v,uint64_t m,uint64_t d,uint64_t*out){
 if(!out||!d)return false;
 uint64_t w=v/d,r=v%d;if(w&&m>UINT64_MAX/w)return false;uint64_t result=w*m,q=0,rem=0;
 for(int bit=63;bit>=0;bit--){if(q>UINT64_MAX/2)return false;q*=2;if(rem>=d-rem){rem-=d-rem;if(q==UINT64_MAX)return false;q++;}else rem+=rem;if((m>>bit)&1ULL){if(rem>=d-r){rem-=d-r;if(q==UINT64_MAX)return false;q++;}else rem+=r;}}
 if(UINT64_MAX-result<q)return false;
 *out=result+q;return true;
}
void task_metrics_stats_snapshot(task_metrics_stats_t*out){if(!out)return;out->runtime_regressions=__atomic_load_n(&g_stats.runtime_regressions,__ATOMIC_RELAXED);out->over_100_samples=__atomic_load_n(&g_stats.over_100_samples,__ATOMIC_RELAXED);out->dropped_baselines=__atomic_load_n(&g_stats.dropped_baselines,__ATOMIC_RELAXED);}
void task_cpu_sampler_reset(task_cpu_sampler_t*s){if(s)memset(s,0,sizeof(*s));}
bool task_cpu_sampler_sample(task_cpu_sampler_t*s,const task_snapshot_t*snap,uint32_t n,uint64_t now,task_cpu_sample_t*out,uint32_t cap){
 if(!s||!snap||!out||cap<n||!now)return false;
 bool wall_ok=!s->initialized||now>s->sample_time_ns;uint64_t wall=(s->initialized&&wall_ok)?now-s->sample_time_ns:0;task_cpu_baseline_entry_t *next=s->scratch;uint32_t nc=0;
 for(uint32_t i=0;i<n;i++){memset(&out[i],0,sizeof(out[i]));int p=find(s,snap[i].id);if(snap[i].state==TASK_ZOMBIE)out[i].valid=1;else if(s->initialized&&wall&&p>=0){uint64_t old=s->entries[p].runtime_ns;if(snap[i].runtime_ns_total<old){out[i].valid=out[i].anomalous=1;s->anomalies++;__atomic_add_fetch(&g_stats.runtime_regressions,1,__ATOMIC_RELAXED);}else{uint64_t value=0;if(!task_metrics_muldiv_u64(snap[i].runtime_ns_total-old,1000,wall,&value)){value=UINT32_MAX;out[i].anomalous=1;s->anomalies++;}out[i].cpu_x10=value>UINT32_MAX?UINT32_MAX:(uint32_t)value;out[i].valid=1;if(out[i].cpu_x10>1000){if(!out[i].anomalous)s->anomalies++;out[i].anomalous=1;__atomic_add_fetch(&g_stats.over_100_samples,1,__ATOMIC_RELAXED);}}}else if(s->initialized&&!wall_ok&&p>=0){out[i].valid=out[i].anomalous=1;s->anomalies++;__atomic_add_fetch(&g_stats.runtime_regressions,1,__ATOMIC_RELAXED);}if(nc<TASK_CPU_BASELINE_MAX)next[nc++]=(task_cpu_baseline_entry_t){snap[i].id,snap[i].runtime_ns_total,1};else{s->dropped_baselines++;__atomic_add_fetch(&g_stats.dropped_baselines,1,__ATOMIC_RELAXED);}}
 memcpy(s->entries,next,nc*sizeof(next[0]));s->count=nc;s->sample_time_ns=now;s->initialized=1;return true;
}
static uint32_t dec(char*out,uint32_t p,uint32_t z,uint64_t v,uint32_t min){char r[24];uint32_t n=0;do{r[n++]=(char)('0'+v%10);v/=10;}while(v&&n<sizeof(r));while(n<min)r[n++]='0';if(p+n>=z)return UINT32_MAX;while(n)out[p++]=r[--n];return p;}
bool task_metrics_format_runtime(uint64_t ns,char*out,uint32_t z){if(!out||!z)return false;uint64_t ms=ns/1000000ULL,h=ms/3600000ULL,m=(ms/60000ULL)%60,s=(ms/1000ULL)%60,x=ms%1000;uint32_t p=0;if(h){p=dec(out,p,z,h,2);if(p==UINT32_MAX||p+1>=z)goto fail;out[p++]=':';}p=dec(out,p,z,m,2);if(p==UINT32_MAX||p+1>=z)goto fail;out[p++]=':';p=dec(out,p,z,s,2);if(p==UINT32_MAX||p+1>=z)goto fail;out[p++]='.';p=dec(out,p,z,x,3);if(p==UINT32_MAX||p>=z)goto fail;out[p]=0;return true;fail:out[0]=0;return false;}
bool task_metrics_sampler_selftest(void){task_metrics_stats_t saved=g_stats;task_cpu_sampler_t *s=&g_selftest_sampler;task_cpu_sample_t o[2];task_snapshot_t t[2]={{.id=0x100000001ULL,.state=TASK_RUNNING},{.id=0x200000002ULL,.state=TASK_RUNNING}};task_cpu_sampler_reset(s);bool ok=task_cpu_sampler_sample(s,t,1,1000,o,1)&&!o[0].valid;ok=ok&&task_cpu_sampler_sample(s,t,1,2000,o,1)&&o[0].valid&&o[0].cpu_x10==0;t[0].runtime_ns_total=500;ok=ok&&task_cpu_sampler_sample(s,t,1,3000,o,1)&&o[0].cpu_x10==500;t[0].runtime_ns_total=1500;ok=ok&&task_cpu_sampler_sample(s,t,1,4000,o,1)&&o[0].cpu_x10==1000;t[0].runtime_ns_total=3000;ok=ok&&task_cpu_sampler_sample(s,t,1,5000,o,1)&&o[0].anomalous;t[0].runtime_ns_total=1;ok=ok&&task_cpu_sampler_sample(s,t,1,6000,o,1)&&o[0].anomalous;t[0].state=TASK_ZOMBIE;ok=ok&&task_cpu_sampler_sample(s,t,1,7000,o,1)&&o[0].valid&&o[0].cpu_x10==0;t[0].state=TASK_RUNNING;ok=ok&&task_cpu_sampler_sample(s,t,2,8000,o,2)&&!o[1].valid;ok=ok&&task_cpu_sampler_sample(s,&t[1],1,9000,o,1)&&s->count==1;ok=ok&&!task_cpu_sampler_sample(s,t,2,10000,o,1)&&!task_cpu_sampler_sample(0,t,1,10000,o,1);uint64_t f=0;ok=ok&&task_metrics_muldiv_u64(UINT64_MAX-1,1000,UINT64_MAX,&f)&&f==999;g_stats=saved;serial_write_all(ok?"[ACCOUNT][SELFTEST] SAMPLER_GUARDS_OK\n":"[ACCOUNT][SELFTEST] SAMPLER_GUARDS_FAIL\n");return ok;}
bool task_metrics_format_selftest(void){char b[40];bool ok=task_metrics_format_runtime(0,b,sizeof(b))&&!strcmp(b,"00:00.000");ok=ok&&task_metrics_format_runtime(1234000000ULL,b,sizeof(b))&&!strcmp(b,"00:01.234");ok=ok&&task_metrics_format_runtime(65001000000ULL,b,sizeof(b))&&!strcmp(b,"01:05.001");ok=ok&&task_metrics_format_runtime(10921009000000ULL,b,sizeof(b))&&!strcmp(b,"03:02:01.009");ok=ok&&task_metrics_format_runtime(360000000000000ULL,b,sizeof(b))&&!strcmp(b,"100:00:00.000");ok=ok&&task_metrics_format_runtime(UINT64_MAX,b,sizeof(b))&&!task_metrics_format_runtime(0,b,4)&&!task_metrics_format_runtime(0,0,sizeof(b))&&!task_metrics_format_runtime(0,b,0);serial_write_all(ok?"[ACCOUNT][SELFTEST] FORMAT_GUARDS_OK\n":"[ACCOUNT][SELFTEST] FORMAT_GUARDS_FAIL\n");return ok;}
bool task_metrics_long_selftest(void){uint64_t o=0;bool ok=task_metrics_muldiv_u64(0,1000,1,&o)&&o==0;ok=ok&&task_metrics_muldiv_u64(1,1000,3,&o)&&o==333&&task_metrics_muldiv_u64(UINT64_MAX-1,1000,UINT64_MAX,&o)&&o==999;ok=ok&&task_metrics_muldiv_u64(UINT64_MAX,1,UINT64_MAX,&o)&&o==1&&!task_metrics_muldiv_u64(UINT64_MAX,UINT64_MAX,1,&o)&&!task_metrics_muldiv_u64(1,1,0,&o);
#ifdef HOBBYOS_ACCOUNT_NEGATIVE_UNSAFE_MULDIV
uint64_t unsafe=((UINT64_MAX-1)*1000ULL)/UINT64_MAX;if(unsafe!=999)serial_write_all("[ACCOUNT][NEGATIVE] UNSAFE_MULDIV_DETECTED\n");else ok=false;
#endif
serial_write_all(ok?"[ACCOUNT][LONG] PASS\n":"[ACCOUNT][LONG] FAIL\n");return ok;}
