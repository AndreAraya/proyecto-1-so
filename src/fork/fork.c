#define HUF_LIBRARY
#include "../serial/serial.c"
#include <sys/wait.h>
#include <unistd.h>

/* Cada hijo envía un byte de resultado al padre por pipe. El volumen de datos
 * se intercambia mediante archivos temporales y el padre reúne las entradas
 * en el mismo orden que la implementación serial. */
typedef struct { pid_t pid; int fd; char *temp; } Job;

static void transfer(FILE *in,FILE *out){
    unsigned char buffer[65536];size_t n;
    while((n=fread(buffer,1,sizeof buffer,in))>0)writeall(out,buffer,n);
    check(!ferror(in),"leer entrada temporal");
}
static char *temporary(char *base,size_t i){
    char name[32];snprintf(name,sizeof name,"%06zu.part",i);
    return join(base,name);
}
static void finish(Job *jobs,size_t count){
    for(size_t j=0;j<count;j++){
        char status=0;ssize_t got=read(jobs[j].fd,&status,1);
        check(close(jobs[j].fd)==0,"cerrar pipe");
        int state=0;check(waitpid(jobs[j].pid,&state,0)==jobs[j].pid,"esperar hijo");
        check(got==1 && status=='K' && WIFEXITED(state) && WEXITSTATUS(state)==0,
              "fallo un proceso hijo (revisar mensaje previo)");
    }
}
static void child_compress(const char *dir,const char *name,const char *tmp,int fd){
    FILE *f=fopen(tmp,"wb");check(f!=NULL,"crear entrada temporal");
    packone(f,dir,name);check(fclose(f)==0,"cerrar entrada temporal");
    check(write(fd,"K",1)==1,"avisar al padre");close(fd);_exit(0);
}
static void run_compress(const char *dir,const char *archive,unsigned workers){
    uint32_t count;char **names=entries(dir,&count);check(count>0,"directorio vacio");
    char tmpdir[]="/tmp/huf-fork-XXXXXX";
    check(mkdtemp(tmpdir)!=NULL,"crear carpeta temporal");
    FILE *out=fopen(archive,"wb");check(out!=NULL,"crear comprimido");
    writeall(out,"HUF1",4);putnum(out,count,4);
    for(size_t i=0;i<count;){
        check(fflush(out)==0,"sincronizar salida antes de fork");
        Job jobs[64]={0};size_t batch=count-i<workers?count-i:workers;
        for(size_t j=0;j<batch;j++){
            int p[2];check(pipe(p)==0,"crear pipe");
            jobs[j].temp=temporary(tmpdir,i+j);
            pid_t pid=fork();check(pid>=0,"crear proceso hijo");
            if(pid==0){close(p[0]);close(fileno(out));child_compress(dir,names[i+j],jobs[j].temp,p[1]);}
            close(p[1]);jobs[j].pid=pid;jobs[j].fd=p[0];
        }
        finish(jobs,batch);
        for(size_t j=0;j<batch;j++){
            FILE *part=fopen(jobs[j].temp,"rb");check(part!=NULL,"abrir entrada temporal");
            transfer(part,out);check(fclose(part)==0,"cerrar temporal");
            check(unlink(jobs[j].temp)==0,"eliminar temporal");free(jobs[j].temp);
            printf("Comprimido por hijo: %s\n",names[i+j]);
        }
        i+=batch;
    }
    for(uint32_t i=0;i<count;i++)free(names[i]);
    free(names);
    check(fclose(out)==0,"cerrar comprimido");check(rmdir(tmpdir)==0,"cerrar temporales");
    printf("Total comprimido con fork: %" PRIu32 " archivos\n",count);
}
static long *offsets(FILE *f,uint32_t *count){
    char magic[4];readall(f,magic,4);check(memcmp(magic,"HUF1",4)==0,"formato invalido");
    uint64_t n=getnum(f,4);check(n>0 && n<=100000,"cantidad invalida");
    long *positions=malloc((size_t)n*sizeof *positions);check(positions!=NULL,"memoria de posiciones");
    for(uint64_t i=0;i<n;i++){
        positions[i]=ftell(f);check(positions[i]>=0,"posicion invalida");
        uint64_t namelen=getnum(f,2);check(namelen>0,"nombre vacio");
        check(fseek(f,(long)namelen+8+16+256*8,SEEK_CUR)==0,"entrada truncada");
        uint64_t length=getnum(f,8);check(length<=1073741824ULL,"entrada demasiado grande");
        check(fseek(f,(long)length,SEEK_CUR)==0,"datos incompletos");
    }
    long end=ftell(f);check(end>=0 && fseek(f,0,SEEK_END)==0,"medir comprimido");
    check(end==ftell(f),"bytes adicionales en comprimido");
    *count=(uint32_t)n;return positions;
}
static void child_decompress(const char *archive,long position,const char *dir,int fd){
    FILE *in=fopen(archive,"rb");check(in!=NULL,"abrir entrada en hijo");
    check(fseek(in,position,SEEK_SET)==0,"ubicar entrada");
    unpackone(in,dir);check(fclose(in)==0,"cerrar entrada");
    check(write(fd,"K",1)==1,"avisar al padre");close(fd);_exit(0);
}
static void run_decompress(const char *archive,const char *dir,unsigned workers){
    FILE *in=fopen(archive,"rb");check(in!=NULL,"abrir comprimido");
    uint32_t count;long *positions=offsets(in,&count);
    check(fclose(in)==0,"cerrar indice del comprimido");
    if(mkdir(dir,0755)!=0)check(errno==EEXIST,"crear salida");
    struct stat st;check(stat(dir,&st)==0 && S_ISDIR(st.st_mode),"salida invalida");
    for(size_t i=0;i<count;){
        Job jobs[64]={0};size_t batch=count-i<workers?count-i:workers;
        for(size_t j=0;j<batch;j++){
            int p[2];check(pipe(p)==0,"crear pipe");
            pid_t pid=fork();check(pid>=0,"crear proceso hijo");
            if(pid==0){close(p[0]);child_decompress(archive,positions[i+j],dir,p[1]);}
            close(p[1]);jobs[j].pid=pid;jobs[j].fd=p[0];
        }
        finish(jobs,batch);i+=batch;
    }
    free(positions);
    printf("Total descomprimido y verificado con fork: %" PRIu32 " archivos\n",count);
}
int main(int argc,char **argv){
    if(argc!=5){fprintf(stderr,"Uso: %s comprimir DIRECTORIO ARCHIVO.huf TRABAJADORES\n"
                              "     %s descomprimir ARCHIVO.huf DIRECTORIO TRABAJADORES\n",argv[0],argv[0]);return 2;}
    char *end=NULL;errno=0;unsigned long n=strtoul(argv[4],&end,10);
    check(errno==0 && end!=argv[4] && *end==0 && n>=1 && n<=64,"trabajadores: 1 a 64");
    if(!strcmp(argv[1],"comprimir"))run_compress(argv[2],argv[3],(unsigned)n);
    else if(!strcmp(argv[1],"descomprimir"))run_decompress(argv[2],argv[3],(unsigned)n);
    else die("operacion desconocida");
    return 0;
}
