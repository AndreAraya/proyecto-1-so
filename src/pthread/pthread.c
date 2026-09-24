#define HUF_LIBRARY
#include "../serial/serial.c"
#include <pthread.h>

typedef struct { char *bytes; size_t length; } Result;
typedef struct {
    pthread_mutex_t lock;
    size_t next, count;
    const char *input, *output;
    char **names;
    long *positions;
    Result *results;
    int decompress;
} Shared;

static size_t take(Shared *s){
    check(pthread_mutex_lock(&s->lock)==0,"bloquear cola");
    size_t i=s->next++;
    check(pthread_mutex_unlock(&s->lock)==0,"desbloquear cola");
    return i;
}
static void *worker(void *arg){
    Shared *s=arg;
    for(;;){
        size_t i=take(s);if(i>=s->count)break;
        if(!s->decompress){
            char *buffer=NULL;size_t length=0;
            FILE *stream=open_memstream(&buffer,&length);
            check(stream!=NULL,"crear buffer compartido");
            packone(stream,s->input,s->names[i]);
            check(fclose(stream)==0,"cerrar buffer");
            s->results[i]=(Result){buffer,length};
        }else{
            FILE *in=fopen(s->input,"rb");check(in!=NULL,"abrir comprimido en hilo");
            check(fseek(in,s->positions[i],SEEK_SET)==0,"ubicar entrada");
            unpackone(in,s->output);
            check(fclose(in)==0,"cerrar lectura");
        }
    }
    return NULL;
}
static void run_workers(Shared *s,unsigned count){
    check(pthread_mutex_init(&s->lock,NULL)==0,"crear mutex");
    pthread_t threads[64];
    for(unsigned j=0;j<count;j++)check(pthread_create(&threads[j],NULL,worker,s)==0,"crear hilo");
    for(unsigned j=0;j<count;j++)check(pthread_join(threads[j],NULL)==0,"esperar hilo");
    check(pthread_mutex_destroy(&s->lock)==0,"cerrar mutex");
}
static void compress_threaded(const char *dir,const char *archive,unsigned workers){
    uint32_t count;char **names=entries(dir,&count);check(count>0,"directorio vacio");
    Result *results=calloc(count,sizeof *results);check(results!=NULL,"memoria compartida");
    Shared shared={.count=count,.input=dir,.names=names,.results=results};
    run_workers(&shared,workers);
    FILE *out=fopen(archive,"wb");check(out!=NULL,"crear comprimido");
    writeall(out,"HUF1",4);putnum(out,count,4);
    for(uint32_t i=0;i<count;i++){
        check(results[i].bytes!=NULL,"resultado ausente");
        writeall(out,results[i].bytes,results[i].length);
        free(results[i].bytes);free(names[i]);
    }
    free(results);free(names);
    check(fclose(out)==0,"cerrar comprimido");
    printf("Total comprimido con pthread: %" PRIu32 " archivos\n",count);
}
static long *index_archive(FILE *f,uint32_t *count){
    char magic[4];readall(f,magic,4);check(memcmp(magic,"HUF1",4)==0,"formato invalido");
    uint64_t n=getnum(f,4);check(n>0 && n<=100000,"cantidad invalida");
    long *positions=malloc((size_t)n*sizeof *positions);check(positions!=NULL,"memoria de indice");
    for(uint64_t i=0;i<n;i++){
        positions[i]=ftell(f);check(positions[i]>=0,"posicion invalida");
        uint64_t namelen=getnum(f,2);check(namelen>0,"nombre vacio");
        check(fseek(f,(long)namelen+8+16+256*8,SEEK_CUR)==0,"entrada incompleta");
        uint64_t length=getnum(f,8);check(length<=1073741824ULL,"entrada demasiado grande");
        check(fseek(f,(long)length,SEEK_CUR)==0,"datos incompletos");
    }
    long end=ftell(f);check(end>=0 && fseek(f,0,SEEK_END)==0 && end==ftell(f),"datos extra o truncados");
    *count=(uint32_t)n;return positions;
}
static void decompress_threaded(const char *archive,const char *dir,unsigned workers){
    FILE *in=fopen(archive,"rb");check(in!=NULL,"abrir comprimido");
    uint32_t count;long *positions=index_archive(in,&count);
    check(fclose(in)==0,"cerrar indice");
    if(mkdir(dir,0755)!=0)check(errno==EEXIST,"crear salida");
    struct stat st;check(stat(dir,&st)==0 && S_ISDIR(st.st_mode),"salida invalida");
    Shared shared={.count=count,.input=archive,.output=dir,.positions=positions,.decompress=1};
    run_workers(&shared,workers);free(positions);
    printf("Total descomprimido y verificado con pthread: %" PRIu32 " archivos\n",count);
}
int main(int argc,char **argv){
    if(argc!=5){fprintf(stderr,"Uso: %s comprimir DIRECTORIO ARCHIVO.huf HILOS\n"
                              "     %s descomprimir ARCHIVO.huf DIRECTORIO HILOS\n",argv[0],argv[0]);return 2;}
    char *end=NULL;errno=0;unsigned long n=strtoul(argv[4],&end,10);
    check(errno==0 && end!=argv[4] && *end==0 && n>=1 && n<=64,"hilos: 1 a 64");
    if(!strcmp(argv[1],"comprimir"))compress_threaded(argv[2],argv[3],(unsigned)n);
    else if(!strcmp(argv[1],"descomprimir"))decompress_threaded(argv[2],argv[3],(unsigned)n);
    else die("operacion desconocida");
    return 0;
}
