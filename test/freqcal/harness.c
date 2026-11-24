/* SPDX-License-Identifier: MIT
 * Host test harness for FREQCAL: fake ICAP services, synthetic carrier + noise (+ optional 2nd carrier).
 * Usage: ./harness MODE NOISE TURN_DELAY_MS STOP_MS AMP2 DIAL_HZ REF_HZ OFF2_HZ
 *   MODE "cal" runs the step calibration, anything else a normal measurement. See run.sh.
 */
/* host test harness for freqcal.c: fake ICAP services, synthetic carrier + noise */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "icap.h"
#define memset fc_memset
#define memcpy fc_memcpy
#include "../../apps/freqcal/freqcal.c"
#undef memset
#undef memcpy

static double sim_ms = 0, gen_samples = 0, phase = 0, phase2 = 0, amp2 = 0, off2 = 0.93;
static double e_ppm = 0.02, step_ppm = 0.25; static int cal_applied = 0; static double popup_ms = -1;
static double dial = 9998500.0, refhz = 10000000.0, snr_amp = 3000, noise = 3000;
static double stop_ms = 300000; static double turn_delay = 10000;
static const char *cfgtext = "win=60\nstep=0.10\n";

static void s_print(const char *s){ printf("[print] %s\n", s); }
static char lines[8][128];
static void s_set_line(int i, const char *s){ if(i>=0&&i<8 && strcmp(lines[i],s)){ strncpy(lines[i],s,127); if(i>=0) printf("%7.1fs L%d %s\n", sim_ms/1000, i, s);} }
static void s_clear(void){}
static int fdkind[8]; static int fdpos[8]; static char fdbuf[8][256]; static FILE *hlog;
static int s_open(const char *n, int mode){
  for(int i=0;i<8;i++) if(!fdkind[i]){
    if(!strcmp(n,"@FREQ")){ fdkind[i]=1; snprintf(fdbuf[i],256,"f=%.0f usb=1 rx=main trv=0\n",dial); fdpos[i]=0; return i; }
    if(!strcmp(n,"FREQCAL.CFG")){ fdkind[i]=2; snprintf(fdbuf[i],256,"%s",cfgtext); fdpos[i]=0; return i; }
    if(!strcmp(n,"FREQCAL.TXT")){ fdkind[i]=3; hlog=fopen("FREQCAL.TXT","w"); return i; }
    return -3; }
  return -2; }
static int s_read(int fd, void *b, uint32_t len){ int n=strlen(fdbuf[fd]+fdpos[fd]); if(n>(int)len)n=len; memcpy(b,fdbuf[fd]+fdpos[fd],n); fdpos[fd]+=n; return n; }
static int s_write(int fd, const void *b, uint32_t len){ if(fdkind[fd]==3&&hlog) fwrite(b,1,len,hlog); return len; }
static int s_close(int fd){ if(fdkind[fd]==3&&hlog) fclose(hlog); fdkind[fd]=0; return 0; }
static void s_sleep(uint32_t ms){ sim_ms += ms; }
static void s_yield(void){}
static uint32_t s_tick(void){ return (uint32_t)sim_ms; }
static int s_mem(struct icap_meminfo *mi){ (void)mi; return 0; }
static int s_rs(struct icap_radio *rs){ memset(rs,0,sizeof *rs); rs->freq_hz=(uint32_t)dial; return 0; }
static int s_stop(void){ return sim_ms > stop_ms; }
static uint32_t s_tid(void){ return 1; }
static int s_clock(struct icap_time *t){ memset(t,0,sizeof *t); t->year=2026;t->mon=11;t->day=19; int s=(int)(sim_ms/1000); t->hour=s/3600; t->min=(s/60)%60; t->sec=s%60; return 0; }
static double gauss(void){ double u=(rand()+1.0)/(RAND_MAX+2.0), v=(rand()+1.0)/(RAND_MAX+2.0); return sqrt(-2*log(u))*cos(2*M_PI*v); }
static int s_aread(int16_t *b, int n){
  double avail = sim_ms*8 - gen_samples; if (avail < 1) return 0; if (n > avail) n = (int)avail;
  if (popup_ms >= 0 && !cal_applied && sim_ms - popup_ms > turn_delay) { e_ppm += 1.0*step_ppm; cal_applied = 1; printf("   >>> sim: REF +5 applied, e=%.4f ppm\n", e_ppm); }
  double tone = refhz - dial*(1+e_ppm*1e-6);
  for(int i=0;i<n;i++){ phase += 2*M_PI*tone/8000; phase2 += 2*M_PI*(tone+off2)/8000; double v = snr_amp*sin(phase) + amp2*snr_amp*sin(phase2) + noise*gauss(); if(v>32767)v=32767; if(v<-32768)v=-32768; b[i]=(int16_t)v; }
  gen_samples += n; return n; }
static int s_arate(void){ return 8000; }
static void s_dec(const char *s){ (void)s; }
static int s_popup(const char *t){ printf("   [POPUP %.0fs] %s\n", sim_ms/1000, t); if (popup_ms < 0) popup_ms = sim_ms; return 0; }

int main(int argc, char **argv){
  if (argc > 1 && !strcmp(argv[1], "cal")) { cfgtext = "ref=10000000\nwin=20\ncal=1.0\nrefnow=58.9\n"; stop_ms = 220000; }
  if (argc > 2) noise = atof(argv[2]);
  if (argc > 3) turn_delay = atof(argv[3]);
  if (argc > 4) stop_ms = atof(argv[4]);
  if (argc > 5) amp2 = atof(argv[5]);
  if (argc > 6) dial = atof(argv[6]);
  if (argc > 7) refhz = atof(argv[7]);
  if (argc > 8) off2 = atof(argv[8]);
  static struct icap_svc svc;
  svc.abi_version=3; svc.radio_id=0x705; svc.size=sizeof svc;
  svc.print=s_print; svc.set_line=s_set_line; svc.clear=s_clear; svc.f_open=s_open; svc.f_read=s_read; svc.f_write=s_write; svc.f_close=s_close;
  svc.sleep_ms=s_sleep; svc.yield=s_yield; svc.tick_ms=s_tick; svc.mem_info=s_mem; svc.radio_state=s_rs; svc.should_stop=s_stop; svc.task_id=s_tid;
  svc.clock_now=s_clock; svc.audio_read=s_aread; svc.audio_rate=s_arate; svc.decode_out=s_dec; svc.popup=s_popup;
  printf("true error %.4f ppm, expected advice REF %+.0f\n", e_ppm, -e_ppm/step_ppm);
  app_main(&svc);
  return 0; }
