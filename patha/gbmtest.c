typedef unsigned int u32; typedef unsigned long u64;
extern int open(const char*,int,...); extern int close(int);
extern void *gbm_create_device(int); extern void *gbm_bo_create(void*,u32,u32,u32,u32);
extern int gbm_bo_get_fd(void*); extern void gbm_device_destroy(void*);
extern int gbm_perform(int, void*, void*);
extern int gbm_bo_get_stride(void*); extern int gbm_bo_get_width(void*); extern int gbm_bo_get_height(void*);
extern int write(int,const void*,unsigned long); extern void _exit(int);
static void P(const char*s){unsigned n=0;while(s[n])n++;write(1,s,n);}
static void Pv(long v){char b[24];int i=0;if(v<0){write(1,"-",1);v=-v;}if(!v){write(1,"0",1);return;}while(v){b[i++]=(char)('0'+v%10);v/=10;}while(i)write(1,&b[--i],1);}
int main(void);
void _start(void){ _exit(main()); }
int main(void){
  const char *nodes[3]; int n;
  nodes[0]="/dev/dri/renderD128"; nodes[1]="/dev/dri/card0"; nodes[2]="/dev/dri/card1";
  for (n=0;n<3;n++){
    int fd=open(nodes[n],2); void *dev; int f;
    P("=== "); P(nodes[n]); P(" ===\n");
    if(fd<0){ P("  open failed\n"); continue; }
    dev=gbm_create_device(fd);
    P("  gbm_create_device="); Pv((long)(unsigned long)dev); P("\n");
    if(!dev){ close(fd); continue; }
    /* flags 1=SCANOUT, 4=LINEAR, 5=both (what the component asks for) */
    { u32 fl[4]; int k; fl[0]=5; fl[1]=1; fl[2]=4; fl[3]=0;
      for(k=0;k<4;k++){
        void *bo=gbm_bo_create(dev,1280,480,0x3231564eu,fl[k]);
        P("  gbm_bo_create(NV12,1280x480,flags="); Pv((long)fl[k]); P(")=");
        Pv((long)(unsigned long)bo);
        if(bo){ int out=-1, r2;
          f=gbm_bo_get_fd(bo); P("  fd="); Pv(f);
          P(" stride="); Pv((long)gbm_bo_get_stride(bo));
          r2=gbm_perform(36, bo, &out);
          P(" gbm_perform(36)="); Pv(r2); P(" out="); Pv(out);
        }
        P("\n");
      } }
  }
  return 0;
}
