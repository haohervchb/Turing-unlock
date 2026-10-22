/* Pure host tests. Uses saved blob files and a simulated register bank ONLY. */
#include "tu102_experiment_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t regs[2][0x700/4], imem[2][0x10000/4], dmem[2][0x10000/4];
    uint32_t ic[2],dc[2],ss0,ss1,plm,lo,hi,scratch;
    uint32_t boot0,workspace;
    unsigned reads,writes,fail_read,fail_write,fail_event,fw_starts,booter_starts;
    unsigned write_at_stage[16],mode;
    uint64_t delayed;
    int alias,rom_bad,reset_stuck,barrier_fail,phase_fail;
    unsigned phase_calls;
    const uint8_t *native;
    Tx *tx;
#ifdef TX_EXPERIMENT_V4
    uint32_t feature,readout,gfw;
    unsigned guard_notes,fail_guard_note;
    int bad_descriptor;
#endif
} Model;
static uint8_t native[TX_FWSEC_SIZE],generic[TX_GENERIC_SIZE],booter[TX_BOOTER_SIZE];
static uint8_t signature[TX_SIGNATURE_SIZE],boot[TX_BOOT_CONTAINER_SIZE];
static Model model;
static Tx tx;
static unsigned cases;

static int model_read(void *ctx,uint32_t off,uint32_t *v) {
    Model *m=ctx;
    if(++m->reads==m->fail_read) return 1;
    if(off>=0x33f060 && off<0x33f060+TX_FWSEC_SIZE) {
        *v=tx_u32(m->native+off-0x33f060)^(m->rom_bad?1:0); return 0;
    }
    if(off>=0x33f024 && off<0x33f060) {
        *v=tx_desc[(off-0x33f024)/4];
#ifdef TX_EXPERIMENT_V4
        if(m->bad_descriptor)*v^=1;
#endif
        return 0;
    }
    switch(off) {
    case 0:*v=m->boot0;return 0;
    case 0x409650:*v=m->plm;return 0;
    case 0x409664:*v=m->ss0;return 0;
    case 0x40966c:*v=m->ss1;return 0;
    case 0x625f04:*v=m->workspace;return 0;
    case 0x1fa824:*v=m->lo;return 0;
    case 0x1fa828:*v=m->hi;return 0;
#ifdef TX_EXPERIMENT_V4
    case 0x409660:*v=m->feature;return 0;
    case 0x409668:*v=m->readout;return 0;
    case 0x118234:*v=m->gfw;return 0;
#else
    case 0x118234:*v=0x3ff;return 0;
#endif
    case 0x1438:*v=m->scratch;return 0;
    }
    unsigned bank=(off&0xffff0000u)==TX_SEC;
    assert((off&0xffff0000u)==TX_GSP || bank);
    uint32_t r=off&0xffffu; assert(r<0x700);
    if(r==0x184) *v=m->imem[bank][(m->ic[bank]&0xffff)/4];
    else if(r==0x1c4) *v=m->dmem[bank][(m->dc[bank]&0xffff)/4];
    else if(r==0x10c && m->reset_stuck) *v=6;
    else *v=m->regs[bank][r/4];
    return 0;
}
static int model_write(void *ctx,uint32_t off,uint32_t v) {
    Model *m=ctx; assert(tx_write_allowed(off));
    /* Forbidden addresses include the historical hang-prone RESET_PLM read
     * target and all host-side WPR/feature-selector/display/flash registers. */
    assert(off!=0x8403c4 && off!=0x1fa824 && off!=0x1fa828 && off!=0x409664);
    if(++m->writes==m->fail_write) return 1;
    unsigned b=(off&0xffff0000u)==TX_SEC; uint32_t r=off&0xffffu;
    m->regs[b][r/4]=v;
    if(r==0x180) m->ic[b]=v;
    if(r==0x1c0) m->dc[b]=v;
    if(r==0x184) {
        m->imem[b][(m->ic[b]&0xffff)/4]=v;
        if(m->ic[b]&(1u<<24)) m->ic[b]+=4;
    }
    if(r==0x1c4) {
        m->dmem[b][(m->dc[b]&0xffff)/4]=v;
        if(m->dc[b]&(1u<<24)) m->dc[b]+=4;
    }
    if(r==0x3c0 && v==1) m->regs[b][0x100/4]=m->alias?0x40:0;
    if((r==0x100 || r==0x130) && v==2) {
        assert(r==(m->alias?0x130u:0x100u));
        if(!b) {
            m->fw_starts++;
            assert(m->dmem[0][0x20/4]==4);
            assert(m->dmem[0][0x30/4]==0x300 && m->dmem[0][0x38/4]==0x9300);
            assert(m->dmem[0][0x48/4]==0x410);
            assert(tx_u32(m->tx->pool+0x99ac)==0x15);
            assert(tx_u32(m->tx->pool+0x99f0)==0x2bfee0);
            m->regs[0][0x100/4]=m->mode==3?0:0x10;
            m->lo=0x2bfee00; m->hi=0x2bffc00;
            if(m->mode==1) m->scratch=0x10000;
            if(m->mode==2) m->lo=0x27fe000;
            if(m->mode==4) m->hi=0;
        } else {
            assert(m->fw_starts==1 && m->lo==0x2bfee00);
            m->booter_starts++;
            assert(tx_u64(m->tx->pool+TX_META_OFF+22*8)==TX_FB_SIZE);
            assert(tx_u64(m->tx->pool+TX_META_OFF+16*8)==106ULL*0x100000);
            m->regs[1][0x100/4]=m->mode==7?0:0x10;
            m->regs[1][0x040/4]=m->mode==5?0x91:0;
            m->ss0=m->mode==6?0x02201054:0x88888888;
            m->ss1=8; m->lo=0x1ffffe00; m->hi=0;
        }
    }
    return 0;
}
static int model_delay(void *ctx,uint32_t us) {
    Model *m=ctx; m->delayed+=us; assert(m->delayed<20000000); return 0;
}
static int model_barrier(void *ctx) { return ((Model *)ctx)->barrier_fail; }
#ifdef TX_EXPERIMENT_V4
static int model_guard_note(void *ctx,uint32_t off,uint32_t want,uint32_t actual,uint32_t mask) {
    Model *m=ctx;(void)off;(void)want;(void)actual;(void)mask;
    return ++m->guard_notes==m->fail_guard_note;
}
#endif
static int model_event(void *ctx,unsigned stage,uint32_t a,uint32_t b) {
    (void)a;(void)b; Model *m=ctx; m->write_at_stage[stage]=m->writes;
    return stage==m->fail_event;
}
static int model_next_input(void *ctx) {
    Model *m=ctx;
    assert(m->tx->stage==TX_STAGE_BOOTER_RESET && m->tx->mutated);
    assert(m->fw_starts==1 && !m->booter_starts);
    assert(!(m->regs[0][0x100/4]&2) && !(m->regs[1][0x100/4]&2));
    m->phase_calls++;
    return m->phase_fail;
}
static void setup(void) {
    uint8_t *pool=tx.pool;
    memset(&model,0,sizeof(model)); memset(&tx,0,sizeof(tx));
    tx.pool=pool; tx.device_address=0x800000000ULL; tx.opaque=&model;
    tx.read=model_read;tx.write=model_write;tx.delay=model_delay;
    tx.barrier=model_barrier;tx.event=model_event;tx.native_fwsec=native;tx.generic_bl=generic;
    model.tx=&tx; model.native=native; model.boot0=0x162000a1; model.workspace=0x20e09;
    model.ss0=0x02201054;model.ss1=4;model.plm=0xffffff8f;model.lo=0x1ffffe00;
    model.regs[0][0x108/4]=0x100; model.regs[1][0x108/4]=0x100;
#ifdef TX_EXPERIMENT_V4
    model.feature=0x2f3;model.readout=0x00400080;model.gfw=0x3ff;
    model.regs[0][0x100/4]=model.regs[1][0x100/4]=0x10;
    tx.guard_note=model_guard_note;
#endif
    assert(tx_prepare(&tx,booter,signature,boot)==TX_OK);
}
static void load(const char *name,uint8_t *data,size_t bytes) {
    FILE *f=fopen(name,"rb");assert(f);
    assert(fread(data,1,bytes,f)==bytes && fgetc(f)==EOF); assert(!fclose(f));
}
static void metadata_test(void) {
    setup(); const uint8_t *m=tx.pool+TX_META_OFF;
    assert(tx_u64(m)==0xdc3aae21371a60b3ULL);
    assert(tx_u64(m+19*8)==TX_FRTS && tx_u64(m+24*8)==0x20000);
    assert(m[241]==1 && !m[240] && !m[242]);
    assert(tx_u64(m+31*8)==0);
    for(size_t i=0;i<0x9600;i++) assert(tx.pool[i]==native[i]);
    for(size_t i=0x9600;i<TX_FWSEC_SIZE;i++)
        if(tx.pool[i]!=native[i]) assert((i>=0x99ac&&i<0x99b0)||(i>=0x99d0&&i<0x9a00));
    assert(!memcmp(tx.pool+TX_SIGNATURE_OFF,signature,TX_SIGNATURE_SIZE));
    uint64_t root=tx_u64(tx.pool+TX_RADIX_OFF), l2=tx_u64(tx.pool+TX_RADIX_OFF+0x1000);
    assert(root==tx.device_address+TX_RADIX_OFF+0x1000);
    assert(l2==tx.device_address+TX_RADIX_OFF+0x2000);
    uint64_t pages=(TX_ELF_SIZE+4095ULL)>>12;
    for(uint64_t i=0;i<pages;i++) assert(tx_u64(tx.pool+TX_RADIX_OFF+0x2000+8*i)==tx.device_address+TX_DATA_OFF+4096*i);
    assert(tx_u64(tx.pool+TX_RADIX_OFF+0x2000+8*pages)==0);
    cases++;
}
int main(void) {
    load("artifacts/2080ti-fwsec-from-v4/fwsec_50hx_prod.bin",native,sizeof(native));
    load("third_party/cmp50hx-unlock/efi-unlock/blobs/bl_gsp_tu102.bin",generic,sizeof(generic));
    load("third_party/cmp50hx-unlock/efi-unlock/blobs/booter_ucode_prod.bin",booter,sizeof(booter));
    load("third_party/cmp50hx-unlock/efi-unlock/blobs/v67_payload.bin",signature,sizeof(signature));
    load("third_party/cmp50hx-unlock/efi-unlock/blobs/gsp_rm_boot_dbg.bin",boot,sizeof(boot));
    tx.pool=malloc(TX_POOL_SIZE); assert(tx.pool);
    metadata_test();
    setup();assert(tx_run(&tx)==TX_OK && tx.unlocked && tx.quiescent);
    assert(model.fw_starts==1 && model.booter_starts==1); cases++;
    unsigned write_total=model.writes;
    unsigned boundaries[16];memcpy(boundaries,model.write_at_stage,sizeof(boundaries));
    setup();model.alias=1;assert(tx_run(&tx)==TX_OK);cases++;
    setup();tx.next_input=model_next_input;
    uint8_t *snapshot=malloc(TX_POOL_SIZE);assert(snapshot);memcpy(snapshot,tx.pool,TX_POOL_SIZE);
    assert(tx_run(&tx)==TX_OK && model.phase_calls==1);
    assert(!memcmp(snapshot,tx.pool,TX_POOL_SIZE));free(snapshot);cases++;
    setup();tx.next_input=model_next_input;model.phase_fail=1;
    assert(tx_run(&tx)==TX_IO && tx.mutated && !tx.quiescent && !model.booter_starts);cases++;
    setup();tx.next_input=model_next_input;model.mode=1;
    assert(tx_run(&tx)==TX_VERIFY && !model.phase_calls && !model.booter_starts);cases++;
    setup();model.boot0^=1;assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.workspace^=0x100;assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.rom_bad=1;assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.plm=0xbadf5620;assert(tx_run(&tx)==TX_VERIFY && !model.writes);cases++;
    for(unsigned mode=1;mode<=7;mode++) {
        setup();model.mode=mode;int r=tx_run(&tx);assert(r && tx.mutated && !tx.quiescent);
        assert(model.booter_starts==(mode>=5?1u:0u));cases++;
    }
    setup();model.reset_stuck=1;assert(tx_run(&tx)==TX_TIMEOUT && !model.fw_starts);cases++;
    setup();model.barrier_fail=1;assert(tx_run(&tx)==TX_IO && !model.fw_starts);cases++;
    for(unsigned stage=1;stage<=TX_STAGE_DONE;stage++) {
        setup();model.fail_event=stage;assert(tx_run(&tx)==TX_LOG);
        assert(model.writes==model.write_at_stage[stage]);cases++;
    }
    for(unsigned stage=TX_STAGE_FWSEC_RESET;stage<=TX_STAGE_CLEANUP;stage++) {
        for(unsigned delta=1;delta<=2;delta++) {
            setup();model.fail_write=boundaries[stage]+delta;
            if(model.fail_write>write_total) continue;
            assert(tx_run(&tx)==TX_IO && model.writes==model.fail_write && tx.mutated);
            cases++;
        }
    }
    const unsigned read_failures[]={1,4,23,100,9800,9890,9900};
    for(unsigned i=0;i<sizeof(read_failures)/sizeof(read_failures[0]);i++) {
        setup();model.fail_read=read_failures[i];assert(tx_run(&tx)==TX_IO);cases++;
    }
    setup();tx.device_address=0xff000000;assert(tx_prepare(&tx,booter,signature,boot)==TX_GUARD);cases++;
    setup();tx.device_address++;assert(tx_prepare(&tx,booter,signature,boot)==TX_GUARD);cases++;
    setup();tx.device_address=0x10000000000ULL-TX_POOL_SIZE;
    assert(tx_prepare(&tx,booter,signature,boot)==TX_OK);cases++;
    setup();tx.device_address=0x10000000000ULL-TX_POOL_SIZE+4096;
    assert(tx_prepare(&tx,booter,signature,boot)==TX_GUARD);cases++;
    setup();assert(tx_write(&tx,0x409664,0x88888888)==TX_GUARD && !model.writes);cases++;
    setup();assert(tx_write(&tx,0x8403c4,0xff)==TX_GUARD && !model.writes);cases++;
#ifdef TX_EXPERIMENT_V4
    const uint32_t pairs[][2]={{0x02201054,4},{0x02201054,6},{0x02221154,4}};
    for(unsigned i=0;i<3;i++) {
        setup();model.ss0=pairs[i][0];model.ss1=pairs[i][1];
        assert(tx_guards(&tx)==TX_OK && tx.baseline_valid && !model.writes);
        assert(tx.initial_ss0==model.ss0 && tx.initial_ss1==model.ss1);
        assert(tx_run(&tx)==TX_OK && tx.unlocked && tx.quiescent);cases++;
        for(unsigned j=0;j<3;j++)if(i!=j) {
            setup();model.ss0=pairs[i][0];model.ss1=pairs[i][1];assert(tx_guards(&tx)==TX_OK);
            model.ss0=pairs[j][0];model.ss1=pairs[j][1];
            assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
        }
    }
    /* Every one-bit alteration of each observed pair must either be another
     * exact observed tuple or refuse. No Cartesian-product/wildcard acceptance. */
    for(unsigned p=0;p<3;p++)for(unsigned word=0;word<2;word++)for(unsigned bit=0;bit<32;bit++) {
        setup();model.ss0=pairs[p][0];model.ss1=pairs[p][1];
        if(word)model.ss1^=1u<<bit;else model.ss0^=1u<<bit;
        int known=tx_observed_pair(model.ss0,model.ss1);
        assert(tx_guards(&tx)==(known?TX_OK:TX_GUARD) && !model.writes);cases++;
    }
    setup();model.ss0=0x02221154;model.ss1=6;
    assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.ss0=0x88888888;model.ss1=8;
    assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    for(unsigned field=0;field<8;field++)for(unsigned bit=0;bit<32;bit++) {
        setup();
        uint32_t *values[]={&model.feature,&model.readout,&model.plm,&model.workspace,
            &model.lo,&model.hi,&model.regs[0][0x100/4],&model.regs[1][0x100/4]};
        *values[field]^=1u<<bit;
        assert(tx_guards(&tx)!=TX_OK && !tx.baseline_valid && !model.writes);cases++;
    }
    for(unsigned i=1;i<=27;i++) {
        setup();model.fail_read=i;
        assert(tx_run(&tx)==TX_IO && !model.writes && !tx.baseline_valid);cases++;
        setup();model.fail_guard_note=i;
        assert(tx_run(&tx)==TX_LOG && !model.writes && !tx.baseline_valid);cases++;
    }
    setup();model.bad_descriptor=1;assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.gfw^=1;assert(tx_run(&tx)==TX_GUARD && !model.writes);cases++;
    setup();model.rom_bad=1;model.fail_guard_note=28;
    assert(tx_run(&tx)==TX_LOG && !model.writes);cases++;
    setup();tx.mutated=1;assert(tx_guards(&tx)==TX_GUARD && !model.reads);cases++;
    setup();tx.native_fwsec=NULL;assert(tx_guards(&tx)==TX_GUARD && !model.reads);cases++;
#endif
    free(tx.pool);
    printf("PASS: %u offline loader cases; exact blobs, metadata/radix bounds, alias CPU start, guards, timeouts, stage and I/O failures. No hardware access.\n",cases);
    return 0;
}
