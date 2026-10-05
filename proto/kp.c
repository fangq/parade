/* Prototype: TTF-metric-driven greedy vs integer Knuth-Plass line breaking,
   pull-form DP (restartable => incremental), optional hysteresis. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
typedef int64_t i64;
static int adv[128]; static int kern[128][128]; static int upm;
static i64 PT = 65536;                   /* scaled points per pt */
static i64 fsize = 10;                   /* 10pt font */
#define MAXW 4096
typedef struct { int n; i64 w[MAXW]; i64 W[MAXW+1]; } Para;
static i64 SP, ST, SH, L;                /* space natural/stretch/shrink, line width */

static i64 wordw(const char *s, int len){ i64 u=0; for(int i=0;i<len;i++){ int c=(unsigned char)s[i]; if(c<32||c>126) c='?'; u+=adv[c]; if(i+1<len){int d=(unsigned char)s[i+1]; if(d>=32&&d<127) u+=kern[c][d];} } return u*fsize*PT/upm; }
static void prefix(Para *p){ p->W[0]=0; for(int i=0;i<p->n;i++) p->W[i+1]=p->W[i]+p->w[i]; }
/* line = words a..b-1 */
static inline i64 nat(Para*p,int a,int b){ return p->W[b]-p->W[a]+(i64)(b-1-a)*SP; }

static int UNCAP=1;
static i64 badness(i64 t, i64 s){ if(UNCAP){ if(t==0) return 0; if(s<=0) return 100000000; i64 r=t*1000/s; if(r>100000) r=100000; i64 b=r*r*r/10000000; return b>100000000?100000000:b; }
  /* TeX's integer badness */
  i64 r; if(t==0) return 0; if(s<=0) return 10000;
  if(t<=7230584) r=(t*297)/s; else if(s>=1663497) r=t/(s/297); else r=t;
  if(r>1290) return 10000; return (int)((r*r*r+0x20000)/0x40000); }

#define LINEPEN 10
#define ADJDEM 10000
#define INF ((i64)1<<60)
static i64 best[MAXW+1][4]; static int prv[MAXW+1][4];
static int oldbrk[MAXW+2]; static i64 HYST=0; static int HLIM=1<<30;

/* returns fitness class, sets *dem; -1 if infeasible */
static int linecost(Para*p,int a,int b,i64*dem,int last){
  int g=b-1-a; i64 x=nat(p,a,b); i64 bad; int fc;
  if(last){ if(x>L) { if(g==0) {*dem=INF/4;return 2;} return -1; } *dem=(i64)LINEPEN*LINEPEN; return 2; }
  if(x<=L){ if(g==0){ bad=badness(L-x,ST); } else bad=badness(L-x,(i64)g*ST); fc= bad>99?0: bad>12?1:2; }
  else { if(g==0||x-L>(i64)g*SH) return g==0? (*dem=INF/4,2) : -1; bad=badness(x-L,(i64)g*SH); fc= bad>12?3:2; }
  i64 d=LINEPEN+bad; *dem=d*d; return fc; }

/* pull-form DP over breakpoints from..n; states < from are reused (incremental) */
static void kp(Para*p,int from){
  if(from<1){ for(int f=0;f<4;f++){best[0][f]=(f==2)?0:INF;prv[0][f]=-1;} from=1; }
  for(int b=from;b<=p->n;b++){
    for(int f=0;f<4;f++){best[b][f]=INF;prv[b][f]=-1;}
    int last=(b==p->n);
    for(int a=b-1;a>=0;a--){
      i64 x=nat(p,a,b);
      if(x>L && x-L>(i64)(b-1-a)*SH && b-1-a>0) break;     /* overfull: further a only worse */
      i64 d; int fc=linecost(p,a,b,&d,last); if(fc<0) continue;
      if(HYST && !last && !oldbrk[b] && b<=HLIM) d+=HYST;
      for(int fa=0;fa<4;fa++){ if(best[a][fa]>=INF) continue;
        i64 t=best[a][fa]+d+((abs(fa-fc)>1)?ADJDEM:0);
        if(t<best[b][fc] || (t==best[b][fc] && a>prv[b][fc])){best[b][fc]=t;prv[b][fc]=a*4+fa;} }
      if(b-1-a>0 && x>L+(i64)(b-1-a)*SH) break;
    }
  }
}
static int backtrack(Para*p,int*brk){ int b=p->n,f=0; i64 m=INF; for(int k=0;k<4;k++) if(best[b][k]<m){m=best[b][k];f=k;}
  int tmp[MAXW],k=0; while(b>0){ tmp[k++]=b; int pv=prv[b][f]; b=pv/4; f=pv%4; }
  for(int i=0;i<k;i++) brk[i]=tmp[k-1-i]; return k; }
static int greedy(Para*p,int*brk){ int k=0,a=0; while(a<p->n){ int b=a+1; while(b<p->n && nat(p,a,b+1)<=L) b++; brk[k++]=b; a=b; } return k; }

typedef struct { int lines,loose; double sumr,maxr,sumsq; int nr; } Stat;
static void stats(Para*p,int*brk,int k,Stat*s){ int a=0; s->lines+=k;
  for(int i=0;i<k;i++){ int b=brk[i]; if(i<k-1 && b-1-a>0){ double r=(double)(L-nat(p,a,b))/((double)(b-1-a)*(L>=nat(p,a,b)?ST:SH));
      double sp=(SP+ r*(r>=0?ST:SH))/(double)PT; s->sumr+=fabs(r); if(r>s->maxr)s->maxr=r; if(r>1)s->loose++; s->sumsq+=sp*sp; s->nr++; } a=b; } }

static Para P[2000]; static int np;
int main(int argc,char**argv){ if(getenv("TEXCAP")) UNCAP=0;
  double lw=argc>1?atof(argv[1]):345; L=(i64)(lw*PT);
  FILE*f=fopen("widths.txt","r"); fscanf(f,"%d",&upm); for(int c=32;c<127;c++) fscanf(f,"%d",&adv[c]);
  int npair; fscanf(f,"%d",&npair); for(int i=0;i<npair;i++){int a,b,v; fscanf(f,"%d %d %d",&a,&b,&v); kern[a][b]=v;} fclose(f);
  SP=wordw(" ",1); ST=SP/2; SH=SP/3;
  /* corpus: paragraphs separated by blank lines */
  FILE*t=fopen(argc>2?argv[2]:"/usr/share/common-licenses/GPL-3","r"); static char buf[1<<20]; size_t len=fread(buf,1,sizeof(buf)-1,t); buf[len]=0; fclose(t);
  char*s=buf; while(*s){ while(*s=='\n'){s++;} if(!*s)break; char*e=strstr(s,"\n\n"); if(!e)e=s+strlen(s);
    Para*p=&P[np]; p->n=0; char*q=s; while(q<e){ while(q<e&&(*q==' '||*q=='\n'))q++; char*w=q; while(q<e&&*q!=' '&&*q!='\n')q++; if(q>w&&p->n<MAXW) p->w[p->n++]=wordw(w,q-w); }
    if(p->n>=12){ prefix(p); np++; } s=e; }
  int brk[MAXW]; Stat g={0},k={0}; int words=0;
  for(int i=0;i<np;i++){ words+=P[i].n; int n=greedy(&P[i],brk); stats(&P[i],brk,n,&g); kp(&P[i],0); n=backtrack(&P[i],brk); stats(&P[i],brk,n,&k); }
  printf("line width %.0fpt, %d paragraphs, %d words, space=%.2fpt +%.2f -%.2f\n",lw,np,words,(double)SP/PT,(double)ST/PT,(double)SH/PT);
  printf("%-8s lines %5d  mean|r| %.3f  max r %6.2f  lines r>1 (gappy) %4d  space sd %.3fpt\n","greedy",g.lines,g.sumr/g.nr,g.maxr,g.loose,sqrt(g.sumsq/g.nr-pow((double)SP/PT,2)));
  printf("%-8s lines %5d  mean|r| %.3f  max r %6.2f  lines r>1 (gappy) %4d  space sd %.3fpt\n","KP",k.lines,k.sumr/k.nr,k.maxr,k.loose,sqrt(k.sumsq/k.nr-pow((double)SP/PT,2)));
  /* timing */
  int R=200; clock_t c0=clock(); for(int r=0;r<R;r++) for(int i=0;i<np;i++) greedy(&P[i],brk); double tg=(double)(clock()-c0)/CLOCKS_PER_SEC;
  c0=clock(); for(int r=0;r<R;r++) for(int i=0;i<np;i++){ kp(&P[i],0); backtrack(&P[i],brk);} double tk=(double)(clock()-c0)/CLOCKS_PER_SEC;
  printf("time per word: greedy %.1f ns, KP %.1f ns  (KP %.2f us per paragraph avg)\n",tg*1e9/R/words,tk*1e9/R/words,tk*1e6/R/np);
  /* incremental + stability: insert a long word mid-paragraph */
  int tested=0,mism=0,touchedLines=0; double tfull=0,tinc=0; i64 HY[4]={0,(i64)100000,(i64)10000000,INF/8}; int movedA[4]={0}; Stat SA[5]; memset(SA,0,sizeof(SA));
  for(int i=0;i<np;i++){ Para*p=&P[i]; if(p->n<60||p->n>=MAXW-1) continue; tested++;
    int ob[MAXW],on; kp(p,0); on=backtrack(p,ob);
    int pos=p->n/2; Para q=*p; memmove(&q.w[pos+1],&q.w[pos],(q.n-pos)*sizeof(i64)); q.w[pos]=wordw("extraordinarily",15); q.n++; prefix(&q);
    for(int h=0;h<4;h++){ HLIM=pos; HYST=HY[h]; memset(oldbrk,0,sizeof(oldbrk)); for(int j=0;j<on;j++) oldbrk[ob[j]<=pos?ob[j]:ob[j]+1]=1;
      kp(&q,0); int fb[MAXW],fn=backtrack(&q,fb);
      int moved=0; for(int j=0;j<fn&&fb[j]<=pos;j++) if(!oldbrk[fb[j]]) moved++;   /* lines above the edit that changed */
      movedA[h]+=moved; stats(&q,fb,fn,&SA[h]); if(h==0){int gb[MAXW]; int gn=greedy(&q,gb); stats(&q,gb,gn,&SA[4]);} 
      if(h==0){ /* incremental from edit point must equal full */
        int nb[MAXW]; c0=clock(); for(int r=0;r<R;r++) kp(&q,0); tfull+=(double)(clock()-c0);
        c0=clock(); for(int r=0;r<R;r++){ kp(&q,pos+1); } tinc+=(double)(clock()-c0); int nn=backtrack(&q,nb);
        if(nn!=fn||memcmp(nb,fb,fn*sizeof(int))) mism++; touchedLines+=fn; } }
  }
  HYST=0;
  printf("incremental re-break (%d paras>=60 words): mismatches vs full %d, speedup %.2fx\n",tested,mism,tfull/tinc);
  const char*nm[4]={"none","1e5","1e7","freeze"}; for(int h=0;h<4;h++) printf("hysteresis %-6s: lines above edit reflowed %4d / %d | mean|r| %.3f  gappy lines %d\n",nm[h],movedA[h],touchedLines,SA[h].sumr/SA[h].nr,SA[h].loose);
  printf("greedy (ref)     : (greedy never reflows lines above) | mean|r| %.3f  gappy lines %d\n",SA[4].sumr/SA[4].nr,SA[4].loose);
  return 0; }
