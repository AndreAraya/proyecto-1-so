#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* HUF1: magic[4], count[u32 LE]; per file: name_len[u16 LE], name,
 * original_size[u64 LE], MD5[16], frequencies[256][u64 LE],
 * compressed_size[u64 LE], packed bits MSB first. */
typedef struct { uint64_t freq; int left, right, symbol, min_symbol; } Node;
typedef struct { unsigned char bits[256]; int length; } Code;

static void die(const char *message) { fprintf(stderr, "Error: %s\n", message); exit(1); }
static void check(int ok, const char *message) { if (!ok) die(message); }
static void putnum(FILE *f, uint64_t v, unsigned n) {
    for (unsigned i=0; i<n; i++) { check(fputc((int)(v&255),f)!=EOF,"escritura del archivo"); v>>=8; }
}
static uint64_t getnum(FILE *f, unsigned n) {
    uint64_t v=0;
    for(unsigned i=0;i<n;i++){ int c=fgetc(f); check(c!=EOF,"archivo truncado"); v|=(uint64_t)(unsigned)c<<(8*i); }
    return v;
}
static void writeall(FILE *f,const void *p,size_t n){check(fwrite(p,1,n,f)==n,"escritura del archivo");}
static void readall(FILE *f,void *p,size_t n){check(fread(p,1,n,f)==n,"archivo truncado");}
static void digest(const unsigned char *data,size_t n,unsigned char out[16]){
    EVP_MD_CTX *ctx=EVP_MD_CTX_new(); unsigned len=0;
    check(ctx!=NULL,"memoria MD5");
    check(EVP_DigestInit_ex(ctx,EVP_md5(),NULL)==1 &&
          EVP_DigestUpdate(ctx,data,n)==1 &&
          EVP_DigestFinal_ex(ctx,out,&len)==1 && len==16,"calculo MD5");
    EVP_MD_CTX_free(ctx);
}
static int less(const Node *a,const Node *b){
    if(a->freq!=b->freq)return a->freq<b->freq;
    return a->min_symbol<b->min_symbol;
}
static int build(Node nodes[512],const uint64_t freqs[256]){
    int active[512],count=0,used=0;
    for(int i=0;i<256;i++)if(freqs[i]){
        nodes[used]=(Node){freqs[i],-1,-1,i,i}; active[count++]=used++;
    }
    if(!count)return -1;
    while(count>1){
        int p=0,q=1;
        if(less(&nodes[active[q]],&nodes[active[p]])){int t=p;p=q;q=t;}
        for(int i=2;i<count;i++){
            if(less(&nodes[active[i]],&nodes[active[p]])){q=p;p=i;}
            else if(less(&nodes[active[i]],&nodes[active[q]]))q=i;
        }
        int left=active[p],right=active[q];
        nodes[used]=(Node){nodes[left].freq+nodes[right].freq,left,right,-1,
                           nodes[left].min_symbol<nodes[right].min_symbol?
                           nodes[left].min_symbol:nodes[right].min_symbol};
        if(p>q){int t=p;p=q;q=t;}
        active[q]=active[--count];active[p]=active[--count];active[count++]=used++;
    }
    return active[0];
}
static void makecodes(const Node *nodes,int index,Code codes[256],
                      unsigned char path[256],int depth){
    if(nodes[index].symbol>=0){
        Code *c=&codes[nodes[index].symbol];c->length=depth?depth:1;
        if(depth)memcpy(c->bits,path,(size_t)depth);else c->bits[0]=0;
        return;
    }
    check(depth<255,"profundidad Huffman");
    path[depth]=0;makecodes(nodes,nodes[index].left,codes,path,depth+1);
    path[depth]=1;makecodes(nodes,nodes[index].right,codes,path,depth+1);
}
static unsigned char *readfile(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");check(f!=NULL,"abrir archivo de entrada");
    check(fseek(f,0,SEEK_END)==0,"medir entrada");long n=ftell(f);
    check(n>=0 && (uint64_t)n<=1073741824ULL,"archivo demasiado grande (limite 1 GiB)");
    check(fseek(f,0,SEEK_SET)==0,"reposicionar entrada");
    unsigned char *data=malloc((size_t)n?(size_t)n:1);
    check(data!=NULL,"memoria de entrada");readall(f,data,(size_t)n);
    check(fclose(f)==0,"cerrar entrada");*size=(size_t)n;return data;
}
static char *join(const char *dir,const char *name){
    size_t a=strlen(dir),b=strlen(name);check(a+b+2>=a && a+b+2>=b,"ruta larga");
    char *p=malloc(a+b+2);check(p!=NULL,"memoria de ruta");
    memcpy(p,dir,a);p[a]='/';memcpy(p+a+1,name,b+1);return p;
}
static int cmpnames(const void *a,const void *b){return strcmp(*(char *const*)a,*(char *const*)b);}
static char **entries(const char *dir,uint32_t *number){
    DIR *d=opendir(dir);check(d!=NULL,"abrir directorio de entrada");
    char **names=NULL;size_t count=0,cap=0;struct dirent *e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char *p=join(dir,e->d_name);struct stat st;
        check(lstat(p,&st)==0,"examinar entrada");free(p);
        if(!S_ISREG(st.st_mode))continue;
        check(strlen(e->d_name)<=65535,"nombre demasiado largo");
        check(count<UINT32_MAX,"demasiados archivos");
        if(count==cap){cap=cap?cap*2:16;names=realloc(names,cap*sizeof *names);check(names!=NULL,"memoria de lista");}
        names[count]=strdup(e->d_name);check(names[count]!=NULL,"memoria de nombre");count++;
    }
    check(closedir(d)==0,"cerrar directorio");
    qsort(names,count,sizeof *names,cmpnames);*number=(uint32_t)count;return names;
}
static void packone(FILE *out,const char *dir,const char *name){
    char *path=join(dir,name);size_t n;unsigned char *data=readfile(path,&n);free(path);
    uint64_t freq[256]={0};for(size_t i=0;i<n;i++)freq[data[i]]++;
    Node nodes[512];int root=build(nodes,freq);Code codes[256]={0};unsigned char work[256];
    if(root>=0)makecodes(nodes,root,codes,work,0);
    unsigned char md5[16];digest(data,n,md5);
    putnum(out,strlen(name),2);writeall(out,name,strlen(name));putnum(out,n,8);
    writeall(out,md5,16);for(int i=0;i<256;i++)putnum(out,freq[i],8);
    long lengthpos=ftell(out);check(lengthpos>=0,"posicion de salida");putnum(out,0,8);
    uint64_t bytes=0;unsigned char byte=0;int bits=0;
    for(size_t i=0;i<n;i++){
        const Code *c=&codes[data[i]];
        for(int j=0;j<c->length;j++){
            byte=(unsigned char)((byte<<1)|c->bits[j]);
            if(++bits==8){check(fputc(byte,out)!=EOF,"datos comprimidos");bytes++;byte=0;bits=0;}
        }
    }
    if(bits){byte=(unsigned char)(byte<<(8-bits));check(fputc(byte,out)!=EOF,"ultimo byte");bytes++;}
    long end=ftell(out);check(end>=0 && fseek(out,lengthpos,SEEK_SET)==0,"longitud comprimida");
    putnum(out,bytes,8);check(fseek(out,end,SEEK_SET)==0,"reposicionar salida");
    free(data);printf("Comprimido: %s (%zu -> %" PRIu64 " bytes de datos)\n",name,n,bytes);
}
#ifndef HUF_LIBRARY
static void compressdir(const char *dir,const char *archive){
    uint32_t count;char **names=entries(dir,&count);check(count>0,"directorio sin archivos regulares");
    /* La ruta del archivo comprimido debe quedar fuera del directorio de entrada. */
    FILE *f=fopen(archive,"wb");check(f!=NULL,"crear archivo comprimido");
    writeall(f,"HUF1",4);putnum(f,count,4);
    for(uint32_t i=0;i<count;i++){packone(f,dir,names[i]);free(names[i]);}
    free(names);check(fclose(f)==0,"cerrar archivo comprimido");
    printf("Total comprimido: %" PRIu32 " archivos\n",count);
}
#endif
static void unpackone(FILE *f,const char *dir){
    uint64_t namelen=getnum(f,2);check(namelen>0,"nombre vacio");
    char *name=malloc((size_t)namelen+1);check(name!=NULL,"memoria nombre");
    readall(f,name,(size_t)namelen);name[namelen]=0;
    check(!strchr(name,'/')&&!strchr(name,'\\')&&strcmp(name,".")&&strcmp(name,"..")&&
          strlen(name)==namelen,"nombre no seguro en comprimido");
    uint64_t n=getnum(f,8);check(n<=1073741824ULL,"archivo demasiado grande");
    unsigned char expected[16],actual[16];readall(f,expected,16);
    uint64_t freq[256],total=0;
    for(int i=0;i<256;i++){freq[i]=getnum(f,8);check(freq[i]<=n && total<=n-freq[i],"frecuencias invalidas");total+=freq[i];}
    check(total==n,"tamano no corresponde a frecuencias");
    uint64_t packed=getnum(f,8);
    check(packed<=1073741824ULL,"datos comprimidos demasiado grandes");
    unsigned char *data=malloc(n?(size_t)n:1);check(data!=NULL,"memoria de salida");
    Node nodes[512];int root=build(nodes,freq);
    uint64_t produced=0;int cursor=root;
    for(uint64_t i=0;i<packed;i++){
        int byte=fgetc(f);check(byte!=EOF,"datos comprimidos truncados");
        for(int bit=7;bit>=0;bit--){
            int value=(byte>>bit)&1;
            if(produced==n){check(value==0,"bits sobrantes invalidos");continue;}
            check(root>=0,"datos para archivo vacio");
            if(nodes[root].symbol>=0){
                check(value==0,"bit de simbolo unico invalido");
                data[produced++]=(unsigned char)nodes[root].symbol;
            }else{
                cursor=value?nodes[cursor].right:nodes[cursor].left;
                check(cursor>=0,"arbol invalido");
                if(nodes[cursor].symbol>=0){
                    data[produced++]=(unsigned char)nodes[cursor].symbol;
                    cursor=root;
                }
            }
        }
    }
    check(produced==n && (root<0 || cursor==root),"datos Huffman incompletos");
    digest(data,(size_t)n,actual);
    check(memcmp(actual,expected,16)==0,"MD5 no coincide");
    char *path=join(dir,name);
    FILE *out=fopen(path,"wbx");check(out!=NULL,"crear archivo de salida (ya existe o sin permiso)");
    writeall(out,data,(size_t)n);check(fclose(out)==0,"cerrar salida");
    printf("Verificado MD5: %s (%" PRIu64 " bytes)\n",name,n);
    free(path);free(data);free(name);
}
#ifndef HUF_LIBRARY
static void decompress(const char *archive,const char *dir){
    FILE *f=fopen(archive,"rb");check(f!=NULL,"abrir comprimido");
    char magic[4];readall(f,magic,4);check(memcmp(magic,"HUF1",4)==0,"formato HUF1 invalido");
    uint64_t count=getnum(f,4);check(count>0 && count<=100000,"cantidad de archivos invalida");
    if(mkdir(dir,0755)!=0)check(errno==EEXIST,"crear directorio de salida");
    struct stat st;check(stat(dir,&st)==0 && S_ISDIR(st.st_mode),"salida no es directorio");
    for(uint64_t i=0;i<count;i++)unpackone(f,dir);
    check(fgetc(f)==EOF && !ferror(f),"datos extra despues del ultimo archivo");
    check(fclose(f)==0,"cerrar comprimido");
    printf("Total descomprimido y verificado: %" PRIu64 " archivos\n",count);
}

int main(int argc,char **argv){
    if(argc!=4){fprintf(stderr,"Uso: %s comprimir DIRECTORIO ARCHIVO.huf\n"
                              "     %s descomprimir ARCHIVO.huf DIRECTORIO\n",argv[0],argv[0]);return 2;}
    if(!strcmp(argv[1],"comprimir"))compressdir(argv[2],argv[3]);
    else if(!strcmp(argv[1],"descomprimir"))decompress(argv[2],argv[3]);
    else die("operacion desconocida");
    return 0;
}
#endif
