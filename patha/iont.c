typedef unsigned int u32; typedef unsigned long u64;
extern int open(const char*,int,...); extern int ioctl(int,unsigned long,...);
extern int write(int,const void*,unsigned long); extern void _exit(int);
extern int *__errno_location(void);
static void P(const char*s){unsigned n=0;while(s[n])n++;write(1,s,n);}
static void Pv(long v){char b[24];int i=0;if(v<0){write(1,"-",1);v=-v;}if(!v){write(1,"0",1);return;}while(v){b[i++]=(char)('0'+v%10);v/=10;}while(i)write(1,&b[--i],1);}
static void Phx(unsigned v){const char*h="0123456789abcdef";int k;for(k=0;k<8;k++)write(1,&h[(v>>(28-4*k))&0xf],1);}
int main(void);
void _start(void){ _exit(main()); }
int main(void){
  int fd=open("/dev/ion",2); unsigned m[8]; int i;
  m[0]=0x2000000u; m[1]=0x2000400u; m[2]=0xffffffffu; m[3]=0x400u; m[4]=0x10u; m[5]=0x2000u; m[6]=1u; m[7]=0x2000000u|1u;
  P("ion fd="); Pv(fd); P("\n");
  for(i=0;i<8;i++){
    struct { unsigned long len, align; unsigned int heap, flags; int handle; } a;
    int j,r; char*p=(char*)&a; for(j=0;j<(int)sizeof(a);j++)p[j]=0;
    a.len=1048576; a.align=4096; a.heap=m[i]; a.flags=0;
    r=ioctl(fd, 0xC0204900UL, &a);
    P("heap_mask=0x"); Phx(m[i]); P(" rc="); Pv(r); P(" errno="); Pv((long)(*__errno_location())); P("\n");
  }
  return 0;
}
